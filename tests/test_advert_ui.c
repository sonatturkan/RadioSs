#define _POSIX_C_SOURCE 200809L
#include "radios/window.h"
#include "radios/audio_ui.h"
#include "radios/audio.h"
#include "radios/media_jobs.h"
#include "radios/media_ui.h"
#include "radios/editors.h"
#include "radios/playout_ui.h"
#include "radios/advert_ui.h"
#include "radios/data.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static RadioSConfig config;
static GtkApplication *application;
static RadioSDataClient *reader;
static const gchar *main_sink, *cue_sink, *monitor_sink;
typedef struct { RadioSWindow *window; RadioSRuntime *runtime; GtkWidget *root; RadioSViewContext *ctx; } Window;
typedef gboolean (*Condition)(gpointer);
static void flag_file(const gchar *name, gboolean enabled);
static void flag_wait(const gchar *name);
static RadioSPlayoutSnapshot run_wait(Window *window, RadioSPlayoutState state, guint64 position);

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *dialog_find(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), name)) result = item->data;
    g_list_free(windows); return result;
}
static void pump(void)
{ while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void run_for(guint milliseconds)
{
    gint64 deadline = g_get_monotonic_time() + (gint64)milliseconds * 1000;
    while (g_get_monotonic_time() < deadline) pump();
}
static void wait_until(Condition condition, gpointer data, const gchar *description)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(data) && g_get_monotonic_time() < deadline) pump();
    if (!condition(data)) g_error("Native advertising acceptance timed out: %s", description);
}
static gboolean documents_ready(GtkWidget *root)
{
    GPtrArray *documents = g_object_get_data(G_OBJECT(root), "radios-data-documents");
    for (guint i = 0; documents && i < documents->len; ++i)
        if (!radios_data_document_ready(g_ptr_array_index(documents, i))) return FALSE;
    if (!GTK_IS_CONTAINER(root)) return TRUE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); gboolean ready = TRUE;
    for (GList *item = children; item && ready; item = item->next) ready = documents_ready(item->data);
    g_list_free(children); return ready;
}
static gboolean window_ready(gpointer value)
{
    Window *window = value;
    GtkWidget *main = find(window->root, "task-main-output"), *cue = find(window->root, "task-cue-output");
    return radios_data_connected(window->ctx) && documents_ready(window->root) && main && cue &&
        !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(main)), main_sink) &&
        !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(cue)), cue_sink) &&
        radios_audio_ui_ready(window->ctx, RADIOS_AUDIO_UI_MAIN) &&
        radios_audio_ui_ready(window->ctx, RADIOS_AUDIO_UI_CUE);
}
static Window window_open(void)
{
    Window window = {0}; GError *error = NULL; window.runtime = radios_runtime_new(64, 128);
    window.window = radios_window_new(application, &config, window.runtime, FALSE, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(window.window);
    window.ctx = radios_window_view_context(window.window); g_assert_nonnull(window.ctx); g_assert_nonnull(window.ctx->audio); g_assert_nonnull(window.ctx->playout); g_assert_nonnull(window.ctx->advertisement);
    window.root = GTK_WIDGET(window.ctx->parent); radios_window_present(window.window);
    wait_until(window_ready, &window, "genuine DB identity, saved virtual outputs and engine readiness");
    return window;
}
static void window_close(Window *window)
{
    radios_window_free(window->window); radios_runtime_free(window->runtime);
    window->window = NULL; window->runtime = NULL; window->ctx = NULL; window->root = NULL;
    while (g_main_context_iteration(NULL, FALSE)) {}
}
static gboolean widget_enabled(gpointer widget) { return gtk_widget_is_sensitive(widget); }
static void click(GtkWidget *root, const gchar *name)
{
    GtkWidget *button = find(root, name); g_assert_true(GTK_IS_BUTTON(button));
    wait_until(widget_enabled, button, "native button becomes available after its status poll"); gtk_button_clicked(GTK_BUTTON(button));
}
static void combo(GtkWidget *root, const gchar *name, const gchar *id)
{
    GtkWidget *widget = find(root, name); g_assert_true(GTK_IS_COMBO_BOX(widget));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), id));
}
typedef struct { RadioSDataClient *client; const gchar *path; JsonNode *body, *result; GError *error; gint done; } Request;
static gpointer request_work(gpointer value)
{
    Request *request = value;
    request->result = radios_data_client_request_sync(request->client, "POST", request->path, request->body, NULL, &request->error);
    g_atomic_int_set(&request->done, 1); return NULL;
}
static gboolean request_done(gpointer value) { return g_atomic_int_get(&((Request *)value)->done) != 0; }
static JsonNode *request(const gchar *path, JsonNode *body)
{
    static gint64 next_read;
    while (g_get_monotonic_time() < next_read) pump();
    next_read = g_get_monotonic_time() + 200 * 1000;
    Request task = {reader, path, body, NULL, NULL, 0}; GThread *thread = g_thread_new("playout-DB-read", request_work, &task);
    wait_until(request_done, &task, "authenticated DB readback"); g_thread_join(thread);
    g_assert_no_error(task.error); g_assert_nonnull(task.result); return task.result;
}
static JsonNode *body_new(void)
{
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(body, json_object_new());
    json_object_set_string_member(json_node_get_object(body), "station_id", radios_data_client_station_id(reader)); return body;
}
static JsonNode *deep_copy(JsonNode *node)
{
    gchar *text = json_to_string(node, FALSE); JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    JsonNode *result = json_node_copy(json_parser_get_root(parser)); g_object_unref(parser); g_free(text); return result;
}
static void write_u16(guchar *bytes, guint16 value) { value = GUINT16_TO_LE(value); memcpy(bytes, &value, sizeof value); }
static void write_u32(guchar *bytes, guint32 value) { value = GUINT32_TO_LE(value); memcpy(bytes, &value, sizeof value); }
typedef struct { JsonNode *result; GError *error; guint callbacks, destroyed; } Import;
static void import_complete(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; Import *output = value; ++output->callbacks;
    if (result) output->result = deep_copy(result);
    if (error) output->error = g_error_copy(error);
}
static void import_destroy(gpointer value) { ++((Import *)value)->destroyed; }
static gboolean import_done(gpointer value) { return ((Import *)value)->destroyed != 0; }
static JsonNode *media_import(Window *window, const gchar *title, guint frames, const gchar *kind)
{
    const gdouble frequency = 900, amplitude = 0.2; guint bytes = frames * 4; guchar *wav = g_malloc(44 + bytes);
    memcpy(wav, "RIFF", 4); write_u32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8); write_u32(wav + 16, 16);
    write_u16(wav + 20, 1); write_u16(wav + 22, 2); write_u32(wav + 24, 48000); write_u32(wav + 28, 48000 * 4);
    write_u16(wav + 32, 4); write_u16(wav + 34, 16); memcpy(wav + 36, "data", 4); write_u32(wav + 40, bytes);
    for (guint i = 0; i < frames; ++i) {
        gint16 sample = (gint16)lrint(sin(2 * G_PI * frequency * i / 48000) * amplitude * 32767);
        write_u16(wav + 44 + i * 4, (guint16)sample); write_u16(wav + 46 + i * 4, (guint16)sample);
    }
    gchar *uuid = g_uuid_string_random(), *name = g_strconcat(uuid, ".wav", NULL), *path = g_build_filename(config.project_root, name, NULL);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL)); g_free(wav); g_free(uuid); g_free(name);
    Import output = {0}; GCancellable *handle = radios_media_import_async(window->ctx, window->root, path,
        kind, title, import_complete, &output, import_destroy);
    g_assert_nonnull(handle); wait_until(import_done, &output, "real SHA-bound PCM analysis/import");
    g_assert_no_error(output.error); g_assert_nonnull(output.result); g_assert_cmpuint(output.callbacks, ==, 1);
    /* Each import also refreshes native profile documents. Let these genuine
     * reads settle between operator actions without weakening the HTTP limit. */
    run_for(2000);
    JsonObject *media = json_object_get_object_member(json_node_get_object(output.result), "media");
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "media_id", radios_json_string(media, "_id", ""));
    JsonNode *detail = request("/v1/workflows/desktop-media/get", body); json_node_free(body); json_node_free(output.result);
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    g_assert_true(json_object_get_boolean_member(version, "verified")); g_assert_true(json_object_get_boolean_member(version, "object_available"));
    g_assert_cmpint(radios_json_int(version, "duration_ms", 0), ==, (frames * 1000 + 47999) / 48000);
    g_assert_cmpstr(radios_json_string(version, "status", ""), ==, "draft");
    g_assert_true(g_file_test(radios_json_string(version, "managed_path", ""), G_FILE_TEST_IS_REGULAR));
    g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); g_object_unref(handle); return detail;
}


static void set(GtkWidget *root, const gchar *name, const gchar *value)
{ GtkWidget *w=find(root,name); g_assert_true(GTK_IS_ENTRY(w)); gtk_entry_set_text(GTK_ENTRY(w),value); }
static void spin(GtkWidget *root, const gchar *name, gdouble value)
{ GtkWidget *w=find(root,name); g_assert_true(GTK_IS_SPIN_BUTTON(w)); gtk_spin_button_set_value(GTK_SPIN_BUTTON(w),value); }
static void key(JsonObject *body)
{ gchar *id=g_uuid_string_random();json_object_set_string_member(body,"idempotency_key",id);g_free(id); }
static gchar *today(void)
{ GDateTime *now=g_date_time_new_now_utc();gchar *value=g_date_time_format(now,"%Y-%m-%d");g_date_time_unref(now);return value; }
static JsonNode *overview(void)
{ JsonNode *body=body_new(),*result=request("/v1/workflows/desktop-advert/overview",body);json_node_free(body);return result; }
static JsonNode *named_record(const gchar *collection,const gchar *name)
{
    JsonNode *result=overview(),*found=NULL;JsonArray *rows=json_object_get_array_member(json_node_get_object(result),collection);
    for(guint i=0;i<json_array_get_length(rows);++i)if(!g_strcmp0(radios_json_string(json_array_get_object_element(rows,i),"name",""),name))
        {found=deep_copy(json_array_get_element(rows,i));break;}
    json_node_free(result);return found;
}
typedef struct {GtkWidget *root;const gchar *id,*uuid;} Row;
static gboolean row_select(gpointer data)
{
    Row *row=data;GtkWidget *view=find(row->root,row->id);if(!GTK_IS_TREE_VIEW(view))return FALSE;
    GtkTreeModel *model=gtk_tree_view_get_model(GTK_TREE_VIEW(view));GtkTreeIter iter;
    for(gboolean valid=gtk_tree_model_get_iter_first(model,&iter);valid;valid=gtk_tree_model_iter_next(model,&iter)){
        gchar *id=NULL;gtk_tree_model_get(model,&iter,4,&id,-1);gboolean match=!g_strcmp0(id,row->uuid);g_free(id);
        if(match){gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)),&iter);return TRUE;}
    }return FALSE;
}
static void select_row(GtkWidget *root,const gchar *table,const gchar *uuid)
{
    Row row={root,table,uuid};gint64 deadline=g_get_monotonic_time()+15*G_TIME_SPAN_SECOND;while(!row_select(&row)&&g_get_monotonic_time()<deadline)pump();
    if(!row_select(&row)){GtkWidget *view=find(root,table),*label=find(root,"media-status");g_error("Actual UUID %s missing from %s (%d rows): %s",uuid,table,GTK_IS_TREE_VIEW(view)?gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(view)),NULL):-1,GTK_IS_LABEL(label)?gtk_label_get_text(GTK_LABEL(label)):"no status");}
}
static gboolean dialog_absent(gpointer name){return dialog_find(name)==NULL;}
static gboolean dialog_present(gpointer name){return dialog_find(name)!=NULL;}
static void accept(const gchar *name)
{
    wait_until(dialog_present,(gpointer)name,"fresh native confirmation becomes available");GtkWidget *dialog=dialog_find(name);g_assert_true(GTK_IS_DIALOG(dialog));gtk_dialog_response(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT);
    gint64 deadline=g_get_monotonic_time()+15*G_TIME_SPAN_SECOND;while(dialog_find(name)&&g_get_monotonic_time()<deadline)pump();
    dialog=dialog_find(name);if(dialog){GtkWidget *label=g_object_get_data(G_OBJECT(dialog),"advert-form-status");
        if(!label){GtkWidget *form=g_object_get_data(G_OBJECT(dialog),"advert-form");label=form?g_object_get_data(G_OBJECT(form),"advert-status"):NULL;}
        g_error("Native form %s did not commit: %s",name,GTK_IS_LABEL(label)?gtk_label_get_text(GTK_LABEL(label)):"no form status");}
}
static gboolean output_ready(gpointer root)
{GtkWidget *w=find(root,"advert-output");return GTK_IS_COMBO_BOX(w)&&gtk_combo_box_set_active_id(GTK_COMBO_BOX(w),"LOCAL_MAIN");}
static void advert_open(Window *window)
{
    click(window->root,"profile-advert-tab");GtkWidget *tools=find(window->root,"advert-tools");g_assert_true(GTK_IS_EXPANDER(tools));gtk_expander_set_expanded(GTK_EXPANDER(tools),TRUE);
    JsonNode *result=overview();JsonArray *outputs=json_object_get_array_member(json_node_get_object(result),"outputs");
    if(!json_array_get_length(outputs))click(window->root,"advert-output-create");
    json_node_free(result);
    wait_until(output_ready,window->root,"explicit configured local logical output");
}
static JsonNode *campaign_create(Window *window,const gchar *name)
{
    advert_open(window);click(window->root,"advert-campaign-add");GtkWidget *dialog=dialog_find("advert-campaign-dialog");g_assert_nonnull(dialog);
    gchar *date=today();set(dialog,"advert-campaign-name",name);set(dialog,"advert-campaign-client",name);set(dialog,"advert-campaign-start",date);set(dialog,"advert-campaign-end",date);
    set(dialog,"advert-campaign-spot","Measured advert");set(dialog,"advert-campaign-version","Native v1");spin(dialog,"advert-campaign-count",3);g_free(date);
    accept("advert-campaign-dialog");JsonNode *record=named_record("campaigns",name);g_assert_nonnull(record);
    select_row(window->root,"advert-campaign-table",radios_json_string(json_node_get_object(record),"_id",""));return record;
}
static void media_pick(Window *window,const gchar *action,JsonNode *detail)
{
    click(window->root,action);wait_until(dialog_present,"media-library-dialog","actual library picker");GtkWidget *dialog=dialog_find("media-library-dialog");
    JsonObject *media=json_object_get_object_member(json_node_get_object(detail),"media");select_row(dialog,"media-table",radios_json_string(media,"_id",""));click(dialog,"media-pick-accept");gtk_dialog_response(GTK_DIALOG(dialog),GTK_RESPONSE_CANCEL);wait_until(dialog_absent,"media-library-dialog","assigned library selection closes explicitly");
}
typedef struct {const gchar *name;gboolean bound,approved;guint spots;} CampaignCondition;
static gboolean campaign_matches(gpointer data)
{
    CampaignCondition *c=data;JsonNode *node=named_record("campaigns",c->name);if(!node)return FALSE;JsonObject *o=json_node_get_object(node);
    JsonArray *spots=json_object_get_array_member(o,"spots");JsonNode *binding=json_object_get_member(o,"binding");gboolean match=(!c->bound||(binding&&JSON_NODE_HOLDS_OBJECT(binding)))&&
        (!c->approved||!g_strcmp0(radios_json_string(o,"status",""),"approved"))&&json_array_get_length(spots)>=c->spots;json_node_free(node);return match;
}
typedef struct {const gchar *collection,*name,*state;} RecordState;
static gboolean record_state(gpointer data)
{
    RecordState *state=data;JsonNode *record=named_record(state->collection,state->name);if(!record)return FALSE;
    JsonObject *object=json_node_get_object(record);gboolean match=!g_strcmp0(radios_json_string(object,g_str_equal(state->collection,"breaks")?"reservation_state":"status",""),state->state);
    json_node_free(record);return match;
}
static void record_wait(const gchar *collection,const gchar *name,const gchar *state)
{RecordState value={collection,name,state};wait_until(record_state,&value,"persisted advertising workflow state");}
static void external_campaign(JsonNode *campaign,const gchar *state)
{
    JsonNode *body=body_new();JsonObject *object=json_node_get_object(body),*record=json_node_get_object(campaign);
    json_object_set_string_member(object,"campaign_id",radios_json_string(record,"_id",""));json_object_set_int_member(object,"expected_revision",radios_json_int(record,"_rev",0));json_object_set_string_member(object,"state",state);key(object);
    JsonNode *result=request("/v1/workflows/desktop-advert/campaign-state",body);json_node_free(result);json_node_free(body);
}
static JsonNode *campaign_bind_approve(Window *window,const gchar *name,JsonNode *detail)
{
    JsonNode *record=campaign_create(window,name);json_node_free(record);media_pick(window,"advert-spot-bind",detail);
    CampaignCondition state={name,TRUE,FALSE,0};wait_until(campaign_matches,&state,"real campaign media binding");
    click(window->root,"advert-spot-approve");wait_until(dialog_present,"advert-spot-approve-dialog","explicit spot and usage-rights review");
    GtkWidget *dialog=dialog_find("advert-spot-approve-dialog"),*gate=find(dialog,"advert-spot-rights-enable");g_assert_true(GTK_IS_TOGGLE_BUTTON(gate));gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(gate),TRUE);
    set(dialog,"advert-spot-rights-territory","LOCAL");combo(dialog,"advert-spot-rights-output","LOCAL_MAIN");set(dialog,"advert-spot-rights-reference","Private acceptance fixture: operator confirms test-owned WAV usage");
    accept("advert-spot-approve-dialog");state.spots=1;wait_until(campaign_matches,&state,"actual formal approved spot and obligation");
    click(window->root,"advert-campaign-approve");accept("advert-campaign-action-dialog");state.approved=TRUE;wait_until(campaign_matches,&state,"separate approved campaign contract");
    return named_record("campaigns",name);
}
static void test_campaign_roundtrip(void)
{
    Window window=window_open();JsonNode *detail=media_import(&window,"Native sales advert",48000+31,"advertisement");
    JsonNode *campaign=campaign_bind_approve(&window,"Native campaign roundtrip",detail);JsonObject *row=json_node_get_object(campaign);
    const gchar *id=radios_json_string(row,"_id","");gchar *saved_id=g_strdup(id);g_assert_cmpint(radios_json_int(row,"_rev",0),>,1);
    JsonArray *spots=json_object_get_array_member(row,"spots"),*obligations=json_object_get_array_member(row,"obligations");g_assert_cmpuint(json_array_get_length(spots),==,1);g_assert_cmpuint(json_array_get_length(obligations),==,1);
    JsonObject *binding=json_object_get_object_member(row,"binding"),*version=json_object_get_object_member(json_node_get_object(detail),"version");
    g_assert_cmpstr(radios_json_string(binding,"version_id",""),==,radios_json_string(version,"_id",""));
    window_close(&window);window=window_open();advert_open(&window);select_row(window.root,"advert-campaign-table",saved_id);
    JsonNode *again=named_record("campaigns","Native campaign roundtrip");g_assert_cmpstr(radios_json_string(json_node_get_object(again),"_id",""),==,saved_id);
    click(window.root,"advert-campaign-edit");wait_until(dialog_present,"advert-campaign-dialog","approved campaign allows safe sales metadata edit");GtkWidget *sales=dialog_find("advert-campaign-dialog");
    const gchar *locked[]={"client","start","end","count","slot","capacity","competition","position"};
    for(guint i=0;i<G_N_ELEMENTS(locked);++i){gchar *id=g_strdup_printf("advert-campaign-%s",locked[i]);g_assert_false(gtk_widget_is_sensitive(find(sales,id)));g_free(id);}
    set(sales,"advert-campaign-owner","Native sales metadata owner");spin(sales,"advert-campaign-price",57.25);accept("advert-campaign-dialog");JsonNode *sales_record=named_record("campaigns","Native campaign roundtrip");JsonObject *sales_row=json_node_get_object(sales_record);
    g_assert_cmpstr(radios_json_string(json_object_get_object_member(sales_row,"desktop_fields"),"owner",""),==,"Native sales metadata owner");g_assert_cmpfloat(json_object_get_double_member(json_object_get_object_member(sales_row,"desktop_fields"),"price"),==,57.25);
    g_assert_cmpstr(radios_json_string(json_object_get_object_member(sales_row,"binding"),"version_id",""),==,radios_json_string(version,"_id",""));g_assert_cmpuint(json_array_get_length(json_object_get_array_member(sales_row,"spots")),==,1);json_node_free(sales_record);
    click(window.root,"advert-campaign-suspend");record_wait("campaigns","Native campaign roundtrip","paused");
    click(window.root,"advert-campaign-resume");record_wait("campaigns","Native campaign roundtrip","approved");
    click(window.root,"advert-campaign-archive");record_wait("campaigns","Native campaign roundtrip","cancelled");
    json_node_free(again);json_node_free(campaign);json_node_free(detail);g_free(saved_id);window_close(&window);
}

typedef struct {GtkWidget *root;const gchar *name,*id;} ComboReady;
static gboolean combo_ready(gpointer data)
{ComboReady *value=data;GtkWidget *widget=find(value->root,value->name);return GTK_IS_COMBO_BOX(widget)&&gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget),value->id);}
static void combo_wait(GtkWidget *root,const gchar *name,const gchar *id)
{ComboReady condition={root,name,id};wait_until(combo_ready,&condition,"fresh server UUID appears in native chooser");}
static gchar *clock_time(gint ahead_seconds)
{
    GDateTime *now=g_date_time_new_now_utc(),*later=g_date_time_add_seconds(now,ahead_seconds);gchar *time=g_date_time_format(later,"%H:%M");g_date_time_unref(later);g_date_time_unref(now);return time;
}
static guint break_serial;
static JsonNode *break_create(Window *window,const gchar *name,gint mode,JsonNode *opener,JsonNode *closer)
{
    advert_open(window);click(window->root,"advert-break-add");GtkWidget *dialog=dialog_find("advert-break-dialog");g_assert_nonnull(dialog);
    gchar *date=today(),*time=clock_time(120+(gint)(++break_serial)*60);
    set(dialog,"advert-k-name",name);set(dialog,"advert-k-date",date);set(dialog,"advert-k-time",time);combo(dialog,"advert-k-output","LOCAL_MAIN");set(dialog,"advert-k-region","LOCAL");
    spin(dialog,"advert-k-capacity",10);GtkWidget *start=find(dialog,"advert-k-start");gtk_combo_box_set_active(GTK_COMBO_BOX(start),mode);g_free(date);g_free(time);
    if(opener||closer)gtk_combo_box_set_active(GTK_COMBO_BOX(find(dialog,"advert-k-empty")),2);
    JsonNode *details[]={opener,closer};const gchar *actions[]={"advert-k-opener-select","advert-k-closer-select"};
    for(guint i=0;i<G_N_ELEMENTS(details);++i)if(details[i]){
        click(dialog,actions[i]);wait_until(dialog_present,"media-library-dialog","actual overhead media picker");GtkWidget *picker=dialog_find("media-library-dialog");
        JsonObject *media=json_object_get_object_member(json_node_get_object(details[i]),"media");select_row(picker,"media-table",radios_json_string(media,"_id",""));click(picker,"media-pick-accept");gtk_dialog_response(GTK_DIALOG(picker),GTK_RESPONSE_CANCEL);wait_until(dialog_absent,"media-library-dialog","verified overhead binding chosen and picker closes explicitly");
    }
    accept("advert-break-dialog");JsonNode *record=named_record("breaks",name);g_assert_nonnull(record);combo_wait(window->root,"advert-break-selector",radios_json_string(json_node_get_object(record),"_id",""));return record;
}
static JsonNode *break_request(const gchar *action,JsonNode *record)
{
    JsonNode *body=body_new();JsonObject *object=json_node_get_object(body),*row=json_node_get_object(record);
    json_object_set_string_member(object,"break_document_id",radios_json_string(row,"_id",""));json_object_set_int_member(object,"expected_revision",radios_json_int(row,"_rev",0));
    gchar *path=g_strdup_printf("/v1/workflows/desktop-advert/%s",action);JsonNode *result=request(path,body);g_free(path);json_node_free(body);return result;
}
static gboolean preview_rows(gpointer root)
{GtkWidget *view=find(root,"advert-break-preview-table");return GTK_IS_TREE_VIEW(view)&&gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(view)),NULL)>0;}
static JsonNode *break_reserve(Window *window,const gchar *name)
{
    JsonNode *record=named_record("breaks",name);g_assert_nonnull(record);combo_wait(window->root,"advert-break-selector",radios_json_string(json_node_get_object(record),"_id",""));json_node_free(record);
    click(window->root,"advert-break-preview");wait_until(preview_rows,window->root,"actual selected/rejected placement preview");click(window->root,"advert-break-reserve");record_wait("breaks",name,"reserved");
    record=named_record("breaks",name);combo_wait(window->root,"advert-break-selector",radios_json_string(json_node_get_object(record),"_id",""));return record;
}
static void campaign_cancel(const gchar *name)
{JsonNode *campaign=named_record("campaigns",name);g_assert_nonnull(campaign);external_campaign(campaign,"cancel");json_node_free(campaign);}
static void test_capacity_cas_reservation(void)
{
    Window window=window_open();JsonNode *a=media_import(&window,"Capacity seven second ad",48000*7,"advertisement"),*b=media_import(&window,"Capacity six second ad",48000*6,"advertisement");
    JsonNode *ca=campaign_bind_approve(&window,"Capacity campaign A",a),*cb=campaign_bind_approve(&window,"Capacity campaign B",b);
    JsonNode *opening=media_import(&window,"Measured quarter-second opener",12000,"jingle"),*closing=media_import(&window,"Measured half-second closer",24000,"jingle");
    JsonNode *br=break_create(&window,"Measured capacity break",1,opening,closing),*preview=break_request("preview",br);JsonObject *plan=json_node_get_object(preview);
    g_assert_cmpint(radios_json_int(plan,"overhead_ms",0),==,750);g_assert_cmpint(radios_json_int(plan,"total_capacity_ms",0),==,10000);
    JsonArray *selected=json_object_get_array_member(plan,"selected"),*rejected=json_object_get_array_member(plan,"rejected");g_assert_cmpuint(json_array_get_length(selected),==,1);g_assert_cmpuint(json_array_get_length(rejected),>,0);
    g_assert_cmpint(radios_json_int(plan,"used_ms",0),<=,9250);g_assert_cmpint(radios_json_int(plan,"remaining_capacity_ms",0),==,9250-radios_json_int(plan,"used_ms",0));
    click(window.root,"advert-break-preview");wait_until(preview_rows,window.root,"native measured capacity preview");
    const gchar *picked=radios_json_string(json_array_get_object_element(selected,0),"campaign_id","");
    JsonNode *paused=!g_strcmp0(picked,radios_json_string(json_node_get_object(ca),"_id",""))?ca:cb;external_campaign(paused,"pause");
    GtkWidget *reserve=find(window.root,"advert-break-reserve");click(window.root,"advert-break-reserve");wait_until(widget_enabled,reserve,"CAS conflict keeps native reservation intent reviewable");
    JsonNode *after=named_record("breaks","Measured capacity break");g_assert_cmpint(radios_json_int(json_node_get_object(after),"_rev",0),==,radios_json_int(json_node_get_object(br),"_rev",0));g_assert_cmpstr(radios_json_string(json_node_get_object(after),"reservation_state",""),==,"draft");json_node_free(after);
    JsonNode *fresh=named_record("campaigns",paused==ca?"Capacity campaign A":"Capacity campaign B");external_campaign(fresh,"resume");json_node_free(fresh);radios_data_refresh(window.ctx);run_for(300);
    after=break_reserve(&window,"Measured capacity break");g_assert_cmpint(radios_json_int(json_node_get_object(after),"_rev",0),>,radios_json_int(json_node_get_object(br),"_rev",0));
    click(window.root,"advert-break-cancel");record_wait("breaks","Measured capacity break","cancelled");campaign_cancel("Capacity campaign A");campaign_cancel("Capacity campaign B");
    JsonNode *empty=break_create(&window,"Empty break with measured overhead",1,opening,closing),*empty_preview=break_request("preview",empty);
    JsonObject *empty_plan=json_node_get_object(empty_preview);g_assert_cmpuint(json_array_get_length(json_object_get_array_member(empty_plan,"selected")),==,0);g_assert_cmpstr(radios_json_string(empty_plan,"empty_action",""),==,"overhead");
    json_node_free(empty);empty=break_reserve(&window,"Empty break with measured overhead");JsonNode *prepared=break_request("prepare",empty);
    JsonObject *manifest=json_object_get_object_member(json_node_get_object(prepared),"manifest");g_assert_cmpuint(json_array_get_length(json_object_get_array_member(manifest,"entries")),==,2);
    click(window.root,"advert-break-play");run_wait(&window,RADIOS_PLAYOUT_FINISHED,0);json_node_free(prepared);json_node_free(empty_preview);json_node_free(empty);
    json_node_free(after);json_node_free(preview);json_node_free(br);json_node_free(ca);json_node_free(cb);json_node_free(a);json_node_free(b);json_node_free(opening);json_node_free(closing);window_close(&window);
}
typedef struct { gdouble rms[2],peak[2]; } Capture;
static goffset capture_offset(const gchar *bus)
{
    gchar *key=g_strdup_printf("RADIOS_TEST_%s_CAPTURE",bus);const gchar *path=g_getenv(key);g_assert_nonnull(path);struct stat value;g_assert_cmpint(g_stat(path,&value),==,0);g_free(key);return value.st_size-value.st_size%8;
}
static Capture capture(const gchar *bus,goffset offset)
{
    Capture result={{0,0},{0,0}};long double squares[2]={0,0};guint64 frames=0;
    gchar *key=g_strdup_printf("RADIOS_TEST_%s_CAPTURE",bus);const gchar *path=g_getenv(key);g_assert_nonnull(path);gint fd=open(path,O_RDONLY);g_free(key);g_assert_cmpint(fd,>=,0);
    struct stat value;g_assert_cmpint(fstat(fd,&value),==,0);gfloat buffer[4096];
    while(offset+8<=value.st_size){ssize_t bytes=pread(fd,buffer,(size_t)MIN((goffset)sizeof buffer,value.st_size-offset),offset);g_assert_cmpint(bytes,>,0);g_assert_cmpint(bytes%8,==,0);
        for(guint i=0;i<(guint)bytes/8;++i)for(guint channel=0;channel<2;++channel){gfloat sample=buffer[i*2+channel];g_assert_true(isfinite(sample));squares[channel]+=(long double)sample*sample;result.peak[channel]=MAX(result.peak[channel],fabs(sample));}
        frames+=(guint64)bytes/8;offset+=bytes;
    }
    close(fd);g_assert_cmpuint(frames,>,4800);for(guint i=0;i<2;++i)result.rms[i]=sqrt((gdouble)(squares[i]/frames));return result;
}
typedef struct {Window *window;RadioSPlayoutState state;guint64 position;} RunCondition;
static gboolean run_matches(gpointer value)
{
    RunCondition *condition=value;RadioSPlayoutSnapshot snapshot;
    return radios_playout_ui_get_snapshot(condition->window->ctx,&snapshot)&&snapshot.state==condition->state&&snapshot.position_ms>=condition->position;
}
static RadioSPlayoutSnapshot run_wait(Window *window,RadioSPlayoutState state,guint64 position)
{
    RunCondition condition={window,state,position};wait_until(run_matches,&condition,"actual C queue state and source-consumption cursor");RadioSPlayoutSnapshot snapshot;g_assert_true(radios_playout_ui_get_snapshot(window->ctx,&snapshot));return snapshot;
}
static JsonNode *documents(const gchar *kind)
{
    JsonNode *body=body_new();json_object_set_string_member(json_node_get_object(body),"kind",kind);JsonNode *result=request("/v1/workflows/desktop/list",body);json_node_free(body);return result;
}
static JsonNode *latest_report(const gchar *campaign_id)
{
    JsonNode *records=documents("advert_report"),*chosen=NULL;JsonArray *rows=json_object_get_array_member(json_node_get_object(records),"records");
    for(guint i=0;i<json_array_get_length(rows);++i){JsonObject *record=json_array_get_object_element(rows,i),*cfg=json_object_get_object_member(record,"config"),*filters=json_object_get_object_member(cfg,"filters");
        if(!g_strcmp0(radios_json_string(filters,"campaign_id",""),campaign_id)){if(chosen)json_node_free(chosen);chosen=deep_copy(json_array_get_element(rows,i));}}
    json_node_free(records);return chosen;
}
typedef struct {GtkWidget *root;const gchar *proof;} ReportVisible;
static gboolean report_visible(gpointer data)
{
    ReportVisible *condition=data;GtkWidget *view=find(condition->root,"advert-asrun-table");if(!GTK_IS_TREE_VIEW(view))return FALSE;GtkTreeModel *model=gtk_tree_view_get_model(GTK_TREE_VIEW(view));GtkTreeIter iter;
    for(gboolean valid=gtk_tree_model_get_iter_first(model,&iter);valid;valid=gtk_tree_model_iter_next(model,&iter)){gchar *proof=NULL;gtk_tree_model_get(model,&iter,6,&proof,-1);gboolean match=!g_strcmp0(proof,condition->proof);g_free(proof);if(match)return TRUE;}return FALSE;
}
static JsonNode *report_native(Window *window,const gchar *campaign_id,const gchar *proof)
{
    gchar *date=today();set(window->root,"advert-asrun-from",date);set(window->root,"advert-asrun-to",date);g_free(date);combo_wait(window->root,"advert-asrun-campaign",campaign_id);combo(window->root,"advert-asrun-output","LOCAL_MAIN");
    click(window->root,"advert-asrun-load");ReportVisible condition={window->root,proof};wait_until(report_visible,&condition,"immutable native report renders an explicit evidence class");
    JsonNode *record=latest_report(campaign_id);g_assert_nonnull(record);return record;
}
static guint documents_count(const gchar *kind)
{JsonNode *result=documents(kind);guint count=json_array_get_length(json_object_get_array_member(json_node_get_object(result),"records"));json_node_free(result);return count;}
static void test_pcm_classified_report_csv(void)
{
    Window window=window_open();JsonNode *detail=media_import(&window,"Three seconds of actual local advertising",48000*3+31,"advertisement");
    JsonNode *campaign=campaign_bind_approve(&window,"Actual PCM campaign",detail);gchar *campaign_id=g_strdup(radios_json_string(json_node_get_object(campaign),"_id",""));
    JsonNode *br=break_create(&window,"Actual PCM break",1,NULL,NULL);json_node_free(br);br=break_reserve(&window,"Actual PCM break");
    goffset main_start=capture_offset("MAIN"),cue_start=capture_offset("CUE");click(window.root,"advert-break-play");run_wait(&window,RADIOS_PLAYOUT_PLAYING,200);run_for(600);
    Capture main=capture("MAIN",main_start),cue=capture("CUE",cue_start);g_assert_cmpfloat(main.peak[0],>,0.05);g_assert_cmpfloat(main.rms[0],>,0.02);g_assert_cmpfloat(cue.peak[0],<,1e-5);
    run_wait(&window,RADIOS_PLAYOUT_FINISHED,0);run_for(250);
    JsonNode *report=report_native(&window,campaign_id,"local_source_consumption");JsonObject *record=json_node_get_object(report),*cfg=json_object_get_object_member(record,"config"),*snapshot=json_object_get_object_member(cfg,"snapshot"),*counts=json_object_get_object_member(snapshot,"counters");
    g_assert_cmpint(radios_json_int(counts,"local_complete",0),==,1);g_assert_cmpint(radios_json_int(counts,"formal_rows",0),==,0);g_assert_false(json_object_get_boolean_member(snapshot,"delivery_proven"));
    JsonArray *rows=json_object_get_array_member(snapshot,"rows");gboolean actual=FALSE;
    for(guint i=0;i<json_array_get_length(rows);++i){JsonObject *row=json_array_get_object_element(rows,i);if(g_str_equal(radios_json_string(row,"record_class",""),"local")){actual=TRUE;g_assert_cmpstr(radios_json_string(row,"state",""),==,"local_complete");g_assert_cmpint(radios_json_int(row,"consumed_frames",0),==,radios_json_int(row,"expected_frames",0));g_assert_false(json_object_get_boolean_member(row,"delivery_proven"));}}
    g_assert_true(actual);gchar *report_id=g_strdup(radios_json_string(record,"_id","")),*hash=g_strdup(radios_json_string(cfg,"snapshot_hash",""));
    click(window.root,"advert-asrun-export");wait_until(dialog_present,"advert-export-dialog","real immutable CSV save dialog");GtkWidget *dialog=dialog_find("advert-export-dialog");
    g_assert_true(gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog),config.project_root));gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog),"actual-source-report.csv");gtk_dialog_response(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT);wait_until(dialog_absent,"advert-export-dialog","real CSV committed");
    gchar *path=g_build_filename(config.project_root,"actual-source-report.csv",NULL),*content=NULL;g_assert_true(g_file_get_contents(path,&content,NULL,NULL));g_assert_nonnull(strstr(content,"record_class,proof_class,delivery_proven"));g_assert_nonnull(strstr(content,"local_source_consumption"));g_assert_nonnull(strstr(content,"False"));g_free(content);g_free(path);
    campaign_cancel("Actual PCM campaign");JsonNode *body=body_new();json_object_set_string_member(json_node_get_object(body),"report_id",report_id);JsonNode *again=request("/v1/workflows/desktop-advert/report-get",body);json_node_free(body);
    g_assert_cmpstr(radios_json_string(json_node_get_object(again),"snapshot_hash",""),==,hash);json_node_free(again);
    window_close(&window);window=window_open();advert_open(&window);RadioSPlayoutSnapshot state;g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&state));g_assert_cmpint(state.state,==,RADIOS_PLAYOUT_IDLE);
    select_row(window.root,"advert-campaign-table",campaign_id);json_node_free(report);json_node_free(campaign);json_node_free(detail);json_node_free(br);g_free(report_id);g_free(hash);g_free(campaign_id);window_close(&window);
}
static JsonObject *programme_for(JsonNode *detail,const gchar *title)
{
    JsonObject *media=json_object_get_object_member(json_node_get_object(detail),"media"),*version=json_object_get_object_member(json_node_get_object(detail),"version");
    JsonObject *config_json=json_object_new();gchar *date=today();json_object_set_string_member(config_json,"title",title);json_object_set_string_member(config_json,"date",date);json_object_set_string_member(config_json,"note","");g_free(date);
    JsonArray *events=json_array_new();JsonObject *event=json_object_new();gchar *id=g_uuid_string_random();json_object_set_string_member(event,"id",id);g_free(id);
    const gchar *keys[]={"title","artist","album","kind"};for(guint i=0;i<G_N_ELEMENTS(keys);++i)json_object_set_string_member(event,keys[i],radios_json_string(media,keys[i],""));
    json_object_set_string_member(event,"start_time","");json_object_set_int_member(event,"duration_ms",radios_json_int(version,"duration_ms",0));json_object_set_string_member(event,"version_label",radios_json_string(version,"label",""));
    json_object_set_string_member(event,"primary_file","");json_object_set_string_member(event,"backup_file","");json_object_set_string_member(event,"note","");
    json_object_set_string_member(event,"media_id",radios_json_string(media,"_id",""));json_object_set_string_member(event,"version_id",radios_json_string(version,"_id",""));json_object_set_string_member(event,"media_file_id",radios_json_string(version,"media_file_id",""));json_object_set_string_member(event,"media_sha256",radios_json_string(version,"sha256",""));
    json_array_add_object_element(events,event);json_object_set_array_member(config_json,"events",events);return config_json;
}
static void test_pause_partial_stop_future_cancel(void)
{
    Window window=window_open();JsonNode *detail=media_import(&window,"Long partial advertisement",48000*6,"advertisement"),*campaign=campaign_bind_approve(&window,"Partial and calendar campaign",detail);
    gchar *campaign_id=g_strdup(radios_json_string(json_node_get_object(campaign),"_id",""));JsonNode *br=break_create(&window,"Partial and calendar break",1,NULL,NULL);json_node_free(br);br=break_reserve(&window,"Partial and calendar break");
    click(window.root,"advert-break-play");run_wait(&window,RADIOS_PLAYOUT_PLAYING,300);click(window.root,"advert-break-pause");run_wait(&window,RADIOS_PLAYOUT_PAUSED,0);run_for(250);
    RadioSPlayoutSnapshot a,b;g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&a));run_for(250);g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&b));g_assert_cmpuint(b.position_ms,<=,a.position_ms+20);
    click(window.root,"advert-break-resume");run_wait(&window,RADIOS_PLAYOUT_PLAYING,a.position_ms+100);click(window.root,"advert-break-stop");run_for(250);
    JsonNode *report=report_native(&window,campaign_id,"local_source_consumption");JsonObject *snapshot=json_object_get_object_member(json_object_get_object_member(json_node_get_object(report),"config"),"snapshot"),*counts=json_object_get_object_member(snapshot,"counters");
    g_assert_cmpint(radios_json_int(counts,"local_interrupted",0),==,1);g_assert_cmpint(radios_json_int(counts,"local_complete",0),==,0);json_node_free(report);
    JsonNode *music=media_import(&window,"Previous actual PCM remains until calendar takeover",48000*20,"music");JsonObject *programme=programme_for(music,"Existing PCM before future advertising");GError *error=NULL;
    g_assert_true(radios_playout_ui_start_programme(window.ctx,window.root,programme,0,0,0,&error));g_assert_no_error(error);json_object_unref(programme);run_wait(&window,RADIOS_PLAYOUT_PLAYING,200);
    g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&a));
    flag_file(".acceptance-advert-prepare-ready",FALSE);flag_file(".acceptance-advert-prepare-hold",TRUE);click(window.root,"advert-break-schedule");flag_wait(".acceptance-advert-prepare-ready");click(window.root,"advert-break-stop");flag_file(".acceptance-advert-prepare-hold",FALSE);run_for(350);
    g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&b));g_assert_cmpint(b.state,==,RADIOS_PLAYOUT_PLAYING);g_assert_cmpstr(b.title,==,"Previous actual PCM remains until calendar takeover");g_assert_cmpuint(b.position_ms,>,a.position_ms+100);
    g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&a));goffset before=capture_offset("MAIN");click(window.root,"advert-break-schedule");run_for(600);g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&b));
    g_assert_cmpstr(b.title,==,"Previous actual PCM remains until calendar takeover");g_assert_cmpuint(b.position_ms,>,a.position_ms+100);Capture sound=capture("MAIN",before);g_assert_cmpfloat(sound.peak[0],>,0.05);
    guint64 before_cancel=b.position_ms;click(window.root,"advert-break-stop");run_for(350);g_assert_false(radios_advert_ui_pending(window.ctx));g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&b));g_assert_cmpint(b.state,==,RADIOS_PLAYOUT_PLAYING);g_assert_cmpstr(b.title,==,"Previous actual PCM remains until calendar takeover");g_assert_cmpuint(b.position_ms,>,before_cancel+100);
    goffset after_cancel=capture_offset("MAIN");run_for(350);sound=capture("MAIN",after_cancel);g_assert_cmpfloat(sound.peak[0],>,0.05);
    g_assert_true(radios_playout_ui_action(window.ctx,RADIOS_PLAYOUT_STOP,&error));g_assert_no_error(error);run_for(250);
    campaign_cancel("Partial and calendar campaign");json_node_free(br);json_node_free(detail);json_node_free(campaign);json_node_free(music);g_free(campaign_id);window_close(&window);
}
static gboolean file_exists(gpointer path){return g_file_test(path,G_FILE_TEST_EXISTS);}
static void flag_file(const gchar *name,gboolean enabled)
{gchar *directory=g_path_get_dirname(config.database_file),*path=g_build_filename(directory,name,NULL);g_free(directory);if(enabled)g_assert_true(g_file_set_contents(path,"1",1,NULL));else if(g_file_test(path,G_FILE_TEST_EXISTS))g_assert_cmpint(g_unlink(path),==,0);g_free(path);}
static void flag_wait(const gchar *name)
{gchar *directory=g_path_get_dirname(config.database_file),*path=g_build_filename(directory,name,NULL);g_free(directory);wait_until(file_exists,path,"isolated backend has computed then held immutable prepare response");g_free(path);}
static gboolean notexecuted_run(gpointer data)
{
    const gchar *break_id=data;JsonNode *result=documents("advert_local_run");JsonArray *rows=json_object_get_array_member(json_node_get_object(result),"records");gboolean found=FALSE;
    for(guint i=0;i<json_array_get_length(rows);++i){JsonObject *cfg=json_object_get_object_member(json_array_get_object_element(rows,i),"config"),*manifest=json_object_get_object_member(cfg,"manifest");
        found|=!g_strcmp0(radios_json_string(manifest,"break_document_id",""),break_id)&&!g_strcmp0(radios_json_string(cfg,"state",""),"notexecuted");
    }
    json_node_free(result);return found;
}
static gboolean failed_zero_run(gpointer data)
{
    const gchar *break_id=data;JsonNode *result=documents("advert_local_run");JsonArray *rows=json_object_get_array_member(json_node_get_object(result),"records");gboolean found=FALSE;
    for(guint i=0;i<json_array_get_length(rows);++i){JsonObject *cfg=json_object_get_object_member(json_array_get_object_element(rows,i),"config"),*manifest=json_object_get_object_member(cfg,"manifest");
        if(g_strcmp0(radios_json_string(manifest,"break_document_id",""),break_id)||g_strcmp0(radios_json_string(cfg,"state",""),"failed"))continue;
        JsonArray *events=json_object_get_array_member(cfg,"events");g_assert_cmpuint(json_array_get_length(events),==,1);JsonObject *event=json_array_get_object_element(events,0);
        g_assert_cmpstr(radios_json_string(event,"state",""),==,"failed");g_assert_cmpint(radios_json_int(event,"consumed_frames",-1),==,0);g_assert_false(json_object_get_boolean_member(cfg,"delivery_proven"));found=TRUE;
    }
    json_node_free(result);return found;
}
static void test_unbound_stale_delay_cas_reopen(void)
{
    Window window=window_open();JsonNode *unbound=campaign_create(&window,"Unbound and CAS-retained campaign");JsonObject *row=json_node_get_object(unbound);JsonNode *binding=json_object_get_member(row,"binding");g_assert_true(JSON_NODE_HOLDS_NULL(binding));g_assert_cmpuint(json_array_get_length(json_object_get_array_member(row,"spots")),==,0);
    g_assert_false(gtk_widget_is_sensitive(find(window.root,"advert-spot-approve")));click(window.root,"advert-campaign-edit");wait_until(dialog_present,"advert-campaign-dialog","fresh campaign sales edit");GtkWidget *dialog=dialog_find("advert-campaign-dialog");g_assert_nonnull(dialog);set(dialog,"advert-campaign-name","Local unsaved fields must survive CAS conflict");
    external_campaign(unbound,"approve");gtk_dialog_response(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT);GtkWidget *save=gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT);wait_until(widget_enabled,save,"CAS error re-enables save without discarding native fields");g_assert_nonnull(dialog_find("advert-campaign-dialog"));g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog,"advert-campaign-name"))),==,"Local unsaved fields must survive CAS conflict");gtk_dialog_response(GTK_DIALOG(dialog),GTK_RESPONSE_CANCEL);
    JsonNode *fresh=named_record("campaigns","Unbound and CAS-retained campaign");external_campaign(fresh,"cancel");json_node_free(fresh);
    JsonNode *detail=media_import(&window,"Stale and delayed verified advertisement",48000*2,"advertisement"),*campaign=campaign_bind_approve(&window,"Stale and delayed campaign",detail),*br=break_create(&window,"Stale and delayed break",1,NULL,NULL);json_node_free(br);br=break_reserve(&window,"Stale and delayed break");
    JsonObject *version=json_object_get_object_member(json_node_get_object(detail),"version");const gchar *path=radios_json_string(version,"managed_path","");gchar *original=NULL;gsize length=0;g_assert_true(g_file_get_contents(path,&original,&length,NULL));gchar *corrupt=g_memdup2(original,length);corrupt[length-1]^=0x01;g_assert_true(g_file_set_contents(path,corrupt,length,NULL));g_free(corrupt);
    guint runs=documents_count("advert_local_run");click(window.root,"advert-break-play");GtkWidget *play=find(window.root,"advert-break-play");wait_until(widget_enabled,play,"stale SHA returns a visible preparation error");RadioSPlayoutSnapshot state;g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&state));g_assert_cmpint(state.state,==,RADIOS_PLAYOUT_IDLE);g_assert_cmpuint(documents_count("advert_local_run"),==,runs);g_assert_true(g_file_set_contents(path,original,length,NULL));g_free(original);
    flag_file(".acceptance-advert-prepare-ready",FALSE);flag_file(".acceptance-advert-prepare-hold",TRUE);click(window.root,"advert-break-play");flag_wait(".acceptance-advert-prepare-ready");click(window.root,"advert-break-stop");flag_file(".acceptance-advert-prepare-hold",FALSE);run_for(450);
    g_assert_false(radios_advert_ui_pending(window.ctx));g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&state));g_assert_cmpint(state.state,==,RADIOS_PLAYOUT_IDLE);g_assert_cmpuint(documents_count("advert_local_run"),==,runs);
    flag_file(".acceptance-advert-prepare-fail",TRUE);click(window.root,"advert-break-play");wait_until(widget_enabled,play,"transport/backend failure leaves retryable UI inputs");g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&state));g_assert_cmpint(state.state,==,RADIOS_PLAYOUT_IDLE);flag_file(".acceptance-advert-prepare-fail",FALSE);
    flag_file(".acceptance-advert-prepare-ready",FALSE);flag_file(".acceptance-advert-prepare-hold",TRUE);click(window.root,"advert-break-play");flag_wait(".acceptance-advert-prepare-ready");window_close(&window);flag_file(".acceptance-advert-prepare-hold",FALSE);flag_file(".acceptance-advert-prepare-ready",FALSE);
    window=window_open();advert_open(&window);g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&state));g_assert_cmpint(state.state,==,RADIOS_PLAYOUT_IDLE);g_assert_false(radios_advert_ui_pending(window.ctx));select_row(window.root,"advert-campaign-table",radios_json_string(json_node_get_object(campaign),"_id",""));
    JsonNode *persisted=named_record("breaks","Stale and delayed break");const gchar *break_id=radios_json_string(json_node_get_object(br),"_id","");g_assert_cmpstr(radios_json_string(json_node_get_object(persisted),"_id",""),==,break_id);json_node_free(persisted);combo_wait(window.root,"advert-break-selector",break_id);
    gchar *ad_bytes=NULL;gsize ad_length=0;g_assert_true(g_file_get_contents(path,&ad_bytes,&ad_length,NULL));flag_file(".acceptance-advert-run-prepare-ready",FALSE);flag_file(".acceptance-advert-run-prepare-hold",TRUE);click(window.root,"advert-break-play");flag_wait(".acceptance-advert-run-prepare-ready");
    gchar *bad_ad=g_memdup2(ad_bytes,ad_length);bad_ad[ad_length-1]^=1;g_assert_true(g_file_set_contents(path,bad_ad,ad_length,NULL));g_free(bad_ad);flag_file(".acceptance-advert-run-prepare-hold",FALSE);run_wait(&window,RADIOS_PLAYOUT_ERROR,0);wait_until(failed_zero_run,(gpointer)break_id,"actual asynchronous ad decoder failure records failed zero frames without fake started/completion");g_assert_false(radios_advert_ui_pending(window.ctx));g_assert_true(g_file_set_contents(path,ad_bytes,ad_length,NULL));g_free(ad_bytes);click(window.root,"advert-break-stop");run_wait(&window,RADIOS_PLAYOUT_IDLE,0);
    JsonNode *broken_music=media_import(&window,"Decoder error must not deadlock advertising handover",48000*2,"music");JsonObject *broken_version=json_object_get_object_member(json_node_get_object(broken_music),"version");const gchar *broken_path=radios_json_string(broken_version,"managed_path","");gchar *good_bytes=NULL;gsize good_length=0;g_assert_true(g_file_get_contents(broken_path,&good_bytes,&good_length,NULL));
    flag_file(".acceptance-playout-prepare-ready",FALSE);flag_file(".acceptance-playout-prepare-hold",TRUE);JsonObject *broken_programme=programme_for(broken_music,"Actual decoder failure before advertising");GError *error=NULL;
    g_assert_true(radios_playout_ui_start_programme(window.ctx,window.root,broken_programme,0,0,0,&error));g_assert_no_error(error);json_object_unref(broken_programme);flag_wait(".acceptance-playout-prepare-ready");
    gchar *changed=g_memdup2(good_bytes,good_length);changed[good_length-1]^=1;g_assert_true(g_file_set_contents(broken_path,changed,good_length,NULL));g_free(changed);flag_file(".acceptance-playout-prepare-hold",FALSE);run_wait(&window,RADIOS_PLAYOUT_ERROR,0);g_assert_true(g_file_set_contents(broken_path,good_bytes,good_length,NULL));g_free(good_bytes);
    click(window.root,"advert-break-play");wait_until(notexecuted_run,(gpointer)break_id,"old C decoder ERROR cancels advertising handover with durable notexecuted receipts");g_assert_false(radios_advert_ui_pending(window.ctx));g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&state));g_assert_cmpint(state.state,==,RADIOS_PLAYOUT_ERROR);
    campaign_cancel("Stale and delayed campaign");json_node_free(broken_music);json_node_free(unbound);json_node_free(detail);json_node_free(campaign);json_node_free(br);window_close(&window);
}

/* Kılavuz 18.10 R10.1: desktop/campaign-version from Reklam › Sürüm geçmişi.
 * A draft campaign gets a new spot draft version (time window, file
 * reference); a bad time keeps the dialog and its inputs; an approved
 * campaign no longer offers the action. */
static gboolean widget_disabled(gpointer widget) { return !gtk_widget_is_sensitive(widget); }
static void test_draft_spot_version(void)
{
    Window window=window_open();JsonNode *campaign=campaign_create(&window,"Native spot version draft");JsonObject *row=json_node_get_object(campaign);
    gchar *id=g_strdup(radios_json_string(row,"_id",""));GtkWidget *add=find(window.root,"advert-version-add");
    wait_until(widget_enabled,add,"a draft campaign offers a new spot version");click(window.root,"advert-version-add");
    wait_until(dialog_present,"advert-version-dialog","native spot version dialog");GtkWidget *dialog=dialog_find("advert-version-dialog");
    set(dialog,"advert-v-name","Native v2");spin(dialog,"advert-v-duration",20);set(dialog,"advert-v-from","25:00");
    gtk_dialog_response(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT);pump();
    g_assert_true(dialog_find("advert-version-dialog")==dialog);g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog,"advert-v-name"))),==,"Native v2");
    set(dialog,"advert-v-from","06:00");set(dialog,"advert-v-to","22:30");set(dialog,"advert-v-file","spots/native-v2.wav");
    accept("advert-version-dialog");
    JsonNode *after=named_record("campaigns","Native spot version draft");JsonObject *saved=json_node_get_object(after);
    g_assert_cmpint(radios_json_int(saved,"_rev",0),==,radios_json_int(row,"_rev",0)+1);
    const gchar *version_id=radios_json_string(saved,"desktop_version_id","");
    g_assert_cmpstr(version_id,!=,radios_json_string(row,"desktop_version_id",""));
    JsonObject *fields=json_object_get_object_member(saved,"desktop_fields");
    g_assert_cmpstr(radios_json_string(fields,"version_label",""),==,"Native v2");g_assert_cmpint(radios_json_int(fields,"duration_seconds",0),==,20);
    JsonNode *body=body_new();json_object_set_string_member(json_node_get_object(body),"collection","versions");json_object_set_string_member(json_node_get_object(body),"record_id",version_id);
    JsonNode *versions=request("/v1/workflows/desktop-workflows/records",body);json_node_free(body);
    JsonArray *records=json_object_get_array_member(json_node_get_object(versions),"records");g_assert_cmpuint(json_array_get_length(records),==,1);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(records,0),"label",""),==,"Native v2");json_node_free(versions);
    JsonNode *state=body_new();JsonObject *request_body=json_node_get_object(state);json_object_set_string_member(request_body,"campaign_id",id);
    json_object_set_int_member(request_body,"expected_revision",radios_json_int(saved,"_rev",0));json_object_set_string_member(request_body,"state","approve");key(request_body);
    json_node_free(request("/v1/workflows/desktop-advert/campaign-state",state));json_node_free(state);
    radios_data_refresh(window.ctx);select_row(window.root,"advert-campaign-table",id);
    wait_until(widget_disabled,add,"an approved campaign keeps its spot version");
    json_node_free(after);json_node_free(campaign);g_free(id);window_close(&window);
}

int main(int argc,char **argv)
{
    g_assert_cmpint(argc,>=,2);gchar *root=g_strdup(argv[1]);for(gint i=1;i<argc;++i)argv[i]=argv[i+1];--argc;
    g_test_init(&argc,&argv,NULL);gtk_init(&argc,&argv);main_sink=g_getenv("RADIOS_TEST_MAIN_SINK");cue_sink=g_getenv("RADIOS_TEST_CUE_SINK");monitor_sink=g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink);g_assert_nonnull(cue_sink);g_assert_nonnull(monitor_sink);g_assert_true(g_str_has_prefix(main_sink,"radios_gui_"));
    GError *error=NULL;g_assert_true(radios_config_load(root,NULL,&config,&error));g_assert_no_error(error);g_free(root);
    application=gtk_application_new("org.radios.NativeAdvertAcceptance",G_APPLICATION_NON_UNIQUE);g_assert_true(g_application_register(G_APPLICATION(application),NULL,&error));g_assert_no_error(error);
    reader=radios_data_client_new(&config,&error);g_assert_no_error(error);g_assert_nonnull(reader);
    g_test_add_func("/native-advert/campaign-verified-bind-explicit-approval-reopen",test_campaign_roundtrip);
    g_test_add_func("/native-advert/measured-capacity-reservation-cas-cancel",test_capacity_cas_reservation);
    g_test_add_func("/native-advert/actual-pcm-immutable-report-csv-reopen",test_pcm_classified_report_csv);
    g_test_add_func("/native-advert/pause-partial-stop-calendar-old-pcm-cancel",test_pause_partial_stop_future_cancel);
    g_test_add_func("/native-advert/unbound-stale-delay-cas-inputs-close-reopen",test_unbound_stale_delay_cas_reopen);
    g_test_add_func("/native-advert/draft-campaign-new-spot-version",test_draft_spot_version);
    gint result=g_test_run();radios_data_client_unref(reader);g_object_unref(application);radios_config_clear(&config);return result;
}
