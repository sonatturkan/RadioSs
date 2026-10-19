#define _POSIX_C_SOURCE 200809L
#include "radios/window.h"
#include "radios/audio_ui.h"
#include "radios/audio.h"
#include "radios/audio_runner.h"
#include "radios/media_jobs.h"
#include "radios/media_ui.h"
#include "radios/editors.h"
#include "radios/playout_ui.h"
#include "radios/broadcast_ui.h"
#include "radios/broadcast_output_ui.h"
#include "radios/media.h"
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
    if (!condition(data)) g_error("Native broadcast acceptance timed out: %s", description);
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
    window.ctx = radios_window_view_context(window.window); g_assert_nonnull(window.ctx); g_assert_nonnull(window.ctx->audio); g_assert_nonnull(window.ctx->playout); g_assert_nonnull(window.ctx->broadcast);
    window.root = GTK_WIDGET(window.ctx->parent); radios_window_present(window.window);
    g_assert_null(find(window.root, "broadcast-output-panel"));
    /* Output regression controls are a test fixture; the broadcast page no
     * longer includes this panel. Keep its lifetime tied to the test window. */
    GtkWidget *layout = gtk_bin_get_child(GTK_BIN(window.root));
    g_assert_true(GTK_IS_BOX(layout));
    GtkWidget *output_panel = radios_broadcast_output_panel_new(window.ctx);
    gtk_box_pack_start(GTK_BOX(layout), output_panel, FALSE, FALSE, 0);
    gtk_widget_show_all(output_panel);
    gtk_expander_set_expanded(GTK_EXPANDER(output_panel), TRUE);
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

static gchar *today(void)
{ GDateTime *now=g_date_time_new_now_utc();gchar *value=g_date_time_format(now,"%Y-%m-%d");g_date_time_unref(now);return value; }
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

static void set(GtkWidget *root, const gchar *name, const gchar *value)
{ GtkWidget *widget=find(root,name); g_assert_true(GTK_IS_ENTRY(widget)); gtk_entry_set_text(GTK_ENTRY(widget),value); }
static RadioSDocument *document_in(GtkWidget *owner)
{
    GPtrArray *documents=g_object_get_data(G_OBJECT(owner),"radios-data-documents");
    if(documents){g_assert_cmpuint(documents->len,==,1);return g_ptr_array_index(documents,0);}
    if(!GTK_IS_CONTAINER(owner))return NULL;
    GList *children=gtk_container_get_children(GTK_CONTAINER(owner));RadioSDocument *result=NULL;
    for(GList *child=children;child&&!result;child=child->next)result=document_in(child->data);
    g_list_free(children);return result;
}
static RadioSDocument *document(GtkWidget *root, const gchar *name)
{
    GtkWidget *owner=find(root,name); g_assert_nonnull(owner);
    RadioSDocument *result=document_in(owner);g_assert_nonnull(result);return result;
}
static gboolean saved(gpointer data) { return radios_data_document_revision(data)>0; }
static void profile_save_status(RadioSDocument *doc,const gchar *status,gpointer data)
{
    (void)doc;(void)data;
    if(g_str_has_prefix(status,"Kaydedilemedi:"))g_error("Broadcast profile save failed: %s",status);
}
static void save_profile(Window *window, const gchar *kind)
{
    gchar *button=g_strdup_printf("broadcast-%s-save",kind),*owner=g_strdup_printf("broadcast-profile-%s",kind);
    RadioSDocument *doc=document(window->root,owner);
    radios_data_document_set_status_callback(doc,profile_save_status,NULL);
    click(window->root,button); wait_until(saved,doc,"native profile receives clean durable acknowledgement");
    radios_data_document_set_status_callback(doc,NULL,NULL);
    g_free(button);g_free(owner);
}
typedef struct { Window *window;const gchar *kind;RadioSAudioOutputState state;guint64 frames; } OutputWait;
static gboolean output_matches(gpointer data)
{
    OutputWait *value=data;RadioSAudioOutputSnapshot snapshot;
    return radios_broadcast_ui_output_snapshot(value->window->ctx,value->kind,&snapshot)&&snapshot.state==value->state&&snapshot.frames_processed>=value->frames;
}
static RadioSAudioOutputSnapshot output_wait(Window *window,const gchar *kind,RadioSAudioOutputState state,guint64 frames)
{
    OutputWait condition={window,kind,state,frames};wait_until(output_matches,&condition,"actual MAIN output state and processed PCM");
    RadioSAudioOutputSnapshot snapshot;g_assert_true(radios_broadcast_ui_output_snapshot(window->ctx,kind,&snapshot));return snapshot;
}
typedef struct {Window *window;const gchar *title;guint64 position;} RunWait;
static gboolean run_matches(gpointer data)
{
    RunWait *value=data;RadioSPlayoutSnapshot snapshot;
    return radios_playout_ui_get_snapshot(value->window->ctx,&snapshot)&&snapshot.state==RADIOS_PLAYOUT_PLAYING&&
        !g_strcmp0(snapshot.title,value->title)&&snapshot.position_ms>=value->position;
}
static void run_wait(Window *window,const gchar *title,guint64 position)
{RunWait condition={window,title,position};wait_until(run_matches,&condition,"actual runner consumes expected source PCM");}
static void programme_start(Window *window,JsonNode *detail)
{
    JsonObject *media=json_object_get_object_member(json_node_get_object(detail),"media");
    JsonObject *programme=programme_for(detail,"Native broadcast verification");GError *error=NULL;
    g_assert_true(radios_playout_ui_start_programme(window->ctx,window->root,programme,0,0,0,&error));g_assert_no_error(error);json_object_unref(programme);
    run_wait(window,radios_json_string(media,"title",""),200);
}
static void programme_stop(Window *window)
{
    GError *error=NULL;g_assert_true(radios_playout_ui_action(window->ctx,RADIOS_PLAYOUT_STOP,&error));g_assert_no_error(error);run_for(150);
}
static JsonNode *documents(const gchar *kind)
{
    JsonNode *body=body_new();json_object_set_string_member(json_node_get_object(body),"kind",kind);
    JsonNode *result=request("/v1/workflows/desktop/list",body);json_node_free(body);return result;
}
typedef struct {const gchar *kind,*state;} Terminal;
static gboolean terminal_record(gpointer data)
{
    Terminal *want=data;JsonNode *result=documents("broadcast_local_run");JsonArray *rows=json_object_get_array_member(json_node_get_object(result),"records");gboolean found=FALSE;
    for(guint i=0;i<json_array_get_length(rows);++i){JsonObject *cfg=json_object_get_object_member(json_array_get_object_element(rows,i),"config"),*manifest=json_object_get_object_member(cfg,"manifest");
        if(!g_strcmp0(radios_json_string(manifest,"kind",""),want->kind)&&!g_strcmp0(radios_json_string(cfg,"state",""),want->state)){
            g_assert_false(json_object_get_boolean_member(cfg,"delivery_proven"));
            JsonArray *events=json_object_get_array_member(cfg,"events");g_assert_cmpuint(json_array_get_length(events),>,0);
            gint64 frames=radios_json_int(json_array_get_object_element(events,json_array_get_length(events)-1),"frames",0);
            if(g_str_equal(want->state,"completed"))g_assert_cmpint(frames,>,0);
            found|=!g_str_equal(want->kind,"emergency")||!g_str_equal(want->state,"interrupted")||frames>0;
        }
    }json_node_free(result);return found;
}
static gchar *file_digest(const gchar *path)
{
    gchar *content=NULL;gsize size=0;g_assert_true(g_file_get_contents(path,&content,&size,NULL));
    gchar *hash=g_compute_checksum_for_data(G_CHECKSUM_SHA256,(const guchar *)content,size);g_free(content);return hash;
}
static void test_record_main_pcm_no_overwrite_reopen(void)
{
    Window window=window_open();JsonNode *detail=media_import(&window,"Actual recorded MAIN music",48000*8,"music");
    set(window.root,"broadcast-record-filename","native-main-proof.wav");save_profile(&window,"record");click(window.root,"broadcast-record-start");
    output_wait(&window,"record",RADIOS_AUDIO_OUTPUT_ACTIVE,0);programme_start(&window,detail);run_wait(&window,"Actual recorded MAIN music",1200);
    click(window.root,"broadcast-record-stop");RadioSAudioOutputSnapshot snapshot=output_wait(&window,"record",RADIOS_AUDIO_OUTPUT_STOPPED,48000);
    g_assert_true(snapshot.recording_finalized);g_assert_cmpuint(snapshot.bytes_written,>,44);
    RadioSMediaAnalysis *analysis=radios_media_analyze(snapshot.target,NULL,NULL,NULL);g_assert_nonnull(analysis);
    g_assert_cmpuint(analysis->sample_rate,==,48000);g_assert_cmpuint(analysis->channels,==,2);g_assert_cmpfloat(analysis->peak,>,0.05);
    radios_media_analysis_free(analysis);gchar *hash=file_digest(snapshot.target);Terminal completed={"record","completed"};wait_until(terminal_record,&completed,"durable completed local record with frame counts");
    click(window.root,"broadcast-record-start");output_wait(&window,"record",RADIOS_AUDIO_OUTPUT_FAILED,0);gchar *again=file_digest(snapshot.target);g_assert_cmpstr(hash,==,again);g_free(hash);g_free(again);
    Terminal failed={"record","failed"};wait_until(terminal_record,&failed,"exclusive create failure remains durable without overwriting WAV");programme_stop(&window);
    window_close(&window);window=window_open();g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(window.root,"broadcast-record-filename"))),==,"native-main-proof.wav");
    RadioSAudioOutputSnapshot reopened;g_assert_true(radios_broadcast_ui_output_snapshot(window.ctx,"record",&reopened));g_assert_cmpint(reopened.state,==,RADIOS_AUDIO_OUTPUT_IDLE);
    json_node_free(detail);window_close(&window);
}
static gboolean metadata_success(gpointer data)
{
    Window *window=data;RadioSAudioOutputSnapshot snapshot;
    return radios_broadcast_ui_output_snapshot(window->ctx,"stream",&snapshot)&&snapshot.metadata_state==RADIOS_AUDIO_METADATA_SUCCESS;
}
static void test_stream_profile_refresh_keeps_draft_and_saved_password(void)
{
    Window window=window_open();
    const gchar *url=g_getenv("RADIOS_TEST_STREAM_URL"),*password=g_getenv("RADIOS_TEST_STREAM_PASSWORD");
    g_assert_nonnull(url);g_assert_nonnull(password);
    set(window.root,"broadcast-stream-url",url);
    set(window.root,"broadcast-stream-username","source");
    set(window.root,"broadcast-stream-password",password);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(window.root,"broadcast-stream-password-save")),TRUE);
    save_profile(&window,"stream");
    RadioSDocument *doc=document(window.root,"broadcast-profile-stream");
    radios_data_document_refresh(doc);run_for(500);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(window.root,"broadcast-stream-password"))),==,password);
    set(window.root,"broadcast-stream-username","draft-source");
    radios_data_refresh(window.ctx);run_for(800);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(window.root,"broadcast-stream-username"))),==,"draft-source");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(window.root,"broadcast-stream-password"))),==,password);
    g_assert_cmpint(radios_data_document_revision(doc),==,0);
    set(window.root,"broadcast-stream-username","source");save_profile(&window,"stream");
    window_close(&window);window=window_open();
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(window.root,"broadcast-stream-url"))),==,url);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(window.root,"broadcast-stream-password"))),==,password);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(window.root,"broadcast-stream-password-save"))));
    window_close(&window);
}
static void test_stream_encoded_metadata_secret_ephemeral(void)
{
    Window window=window_open();JsonNode *detail=media_import(&window,"Encoded stream actual MAIN",48000*8,"music");
    const gchar *url=g_getenv("RADIOS_TEST_STREAM_URL"),*password=g_getenv("RADIOS_TEST_STREAM_PASSWORD");g_assert_nonnull(url);g_assert_nonnull(password);
    set(window.root,"broadcast-stream-url",url);set(window.root,"broadcast-stream-username","source");save_profile(&window,"stream");
    set(window.root,"broadcast-stream-password",password);click(window.root,"broadcast-stream-start");output_wait(&window,"stream",RADIOS_AUDIO_OUTPUT_ACTIVE,0);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(window.root,"broadcast-stream-password"))),==,password);programme_start(&window,detail);
    RadioSAudioOutputSnapshot before_refresh=output_wait(&window,"stream",RADIOS_AUDIO_OUTPUT_ACTIVE,4800);
    radios_data_refresh(window.ctx);run_for(700);
    RadioSAudioOutputSnapshot after_refresh=output_wait(&window,"stream",RADIOS_AUDIO_OUTPUT_ACTIVE,before_refresh.frames_processed+4800);
    g_assert_cmpuint(after_refresh.generation,==,before_refresh.generation);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(window.root,"broadcast-stream-url"))),==,url);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(window.root,"broadcast-stream-password"))),==,password);
    const gchar *title="RadioS · Türkçe gerçek metadata";set(window.root,"broadcast-metadata-title",title);save_profile(&window,"metadata");click(window.root,"broadcast-metadata-start");wait_until(metadata_success,&window,"actual authenticated Icecast metadata HTTP acknowledgement");
    gchar *capture=NULL;g_assert_true(g_file_get_contents(g_getenv("RADIOS_TEST_METADATA_CAPTURE"),&capture,NULL,NULL));JsonParser *parser=json_parser_new();g_assert_true(json_parser_load_from_data(parser,capture,-1,NULL));
    JsonObject *metadata=json_node_get_object(json_parser_get_root(parser));g_assert_cmpstr(radios_json_string(metadata,"title",""),==,title);g_assert_true(json_object_get_boolean_member(metadata,"authorization_seen"));g_free(capture);g_object_unref(parser);
    run_wait(&window,"Encoded stream actual MAIN",1400);click(window.root,"broadcast-stream-stop");RadioSAudioOutputSnapshot snapshot=output_wait(&window,"stream",RADIOS_AUDIO_OUTPUT_STOPPED,48000);
    g_assert_cmpuint(snapshot.bytes_written,>,1000);g_assert_true(snapshot.response_confirmed);g_assert_cmpint(snapshot.http_status,==,200);
    RadioSMediaAnalysis *analysis=radios_media_analyze(g_getenv("RADIOS_TEST_STREAM_CAPTURE"),NULL,NULL,NULL);g_assert_nonnull(analysis);g_assert_cmpstr(analysis->codec,==,"mp3");g_assert_cmpfloat(analysis->peak,>,0.05);radios_media_analysis_free(analysis);
    JsonNode *body=body_new(),*result=request("/v1/workflows/desktop-broadcast/overview",body);json_node_free(body);gchar *json=json_to_string(result,FALSE);g_assert_null(strstr(json,password));g_free(json);json_node_free(result);
    Terminal complete={"stream","completed"};wait_until(terminal_record,&complete,"durable stream HTTP terminal receipt with explicit local evidence");programme_stop(&window);json_node_free(detail);window_close(&window);
}
typedef struct {GtkWidget *tree;const gchar *uuid;} MediaRow;
static gboolean select_media_row(gpointer data)
{
    MediaRow *row=data;GtkTreeModel *model=gtk_tree_view_get_model(GTK_TREE_VIEW(row->tree));GtkTreeIter iter;
    for(gboolean valid=gtk_tree_model_get_iter_first(model,&iter);valid;valid=gtk_tree_model_iter_next(model,&iter)){
        gchar *id=NULL;gtk_tree_model_get(model,&iter,4,&id,-1);gboolean match=!g_strcmp0(id,row->uuid);g_free(id);
        if(match){gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(row->tree)),&iter);return TRUE;}
    }return FALSE;
}
static void emergency_pick(Window *window,JsonNode *detail)
{
    click(window->root,"profile-emergency-tab");click(window->root,"em-list-new");click(window->root,"em-editor-file");
    GtkWidget *dialog=dialog_find("media-library-dialog");g_assert_nonnull(dialog);
    JsonObject *media=json_object_get_object_member(json_node_get_object(detail),"media");MediaRow row={find(dialog,"media-table"),radios_json_string(media,"_id","")};
    wait_until(select_media_row,&row,"real imported emergency UUID selectable in native library");click(dialog,"media-pick-accept");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(window->root,"em-editor-title"))),==,radios_json_string(media,"title",""));
    g_assert_false(gtk_widget_is_sensitive(find(window->root,"em-editor-duration")));click(window->root,"em-editor-save");
    wait_until(saved,document(window->root,"emergency-task-view"),"SHA-bound emergency list receives durable acknowledgement");
    dialog=dialog_find("media-library-dialog");if(dialog)gtk_widget_destroy(dialog);
}
static void emergency_start_native(Window *window)
{
    click(window->root,"em-takeover-open");GtkWidget *dialog=dialog_find("emergency-dialog");g_assert_nonnull(dialog);
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(dialog,"emergency-strategy")),0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(dialog,"emergency-confirmed")),TRUE);gtk_dialog_response(GTK_DIALOG(dialog),GTK_RESPONSE_APPLY);
}
static void flag_file(const gchar *name,gboolean enabled)
{
    gchar *directory=g_path_get_dirname(config.database_file),*path=g_build_filename(directory,name,NULL);g_free(directory);
    if(enabled)g_assert_true(g_file_set_contents(path,"1",1,NULL));else if(g_file_test(path,G_FILE_TEST_EXISTS))g_assert_cmpint(g_unlink(path),==,0);g_free(path);
}
static gboolean file_exists(gpointer path) { return g_file_test(path,G_FILE_TEST_EXISTS); }
static void flag_wait(const gchar *name)
{
    gchar *directory=g_path_get_dirname(config.database_file),*path=g_build_filename(directory,name,NULL);g_free(directory);
    wait_until(file_exists,path,"private backend created immutable durable run before holding its response");g_free(path);
}
static gboolean cue_playing(gpointer data)
{
    Window *window=data;RadioSAudioEngine *engine=radios_audio_ui_engine_acquire(window->ctx);g_assert_nonnull(engine);
    RadioSAudioSnapshot snapshot;radios_audio_engine_snapshot(engine,&snapshot);radios_audio_engine_unref(engine);
    return snapshot.decks[32].state==RADIOS_AUDIO_PLAYING&&snapshot.decks[32].cursor_frames>=4800&&snapshot.decks[32].peak[0]>0.05;
}
static void test_emergency_verified_takeover_return_pending_cancel(void)
{
    Window window=window_open();JsonNode *normal=media_import(&window,"Normal programme survives cancelled takeover",48000*20,"music"),*emergency=media_import(&window,"Verified emergency announcement",48000*8,"announcement");
    programme_start(&window,normal);emergency_pick(&window,emergency);
    flag_file(".acceptance-media-get-hold",TRUE);click(window.root,"em-editor-cue");click(window.root,"em-editor-cue");run_for(250);
    flag_file(".acceptance-media-get-hold",FALSE);run_for(350);g_assert_false(cue_playing(&window));
    click(window.root,"em-editor-cue");wait_until(cue_playing,&window,"actual emergency CUE consumes verified source PCM independently of MAIN");
    RadioSPlayoutSnapshot main;g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&main));g_assert_cmpstr(main.title,==,"Normal programme survives cancelled takeover");
    click(window.root,"em-editor-cue");run_for(150);g_assert_false(cue_playing(&window));
    flag_file(".acceptance-broadcast-start-ready",FALSE);flag_file(".acceptance-broadcast-start-hold",TRUE);emergency_start_native(&window);flag_wait(".acceptance-broadcast-start-ready");
    RadioSPlayoutSnapshot before,after;g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&before));click(window.root,"em-takeover-abort");flag_file(".acceptance-broadcast-start-hold",FALSE);run_for(600);
    g_assert_false(radios_broadcast_ui_emergency_pending(window.ctx));g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&after));g_assert_cmpint(after.state,==,RADIOS_PLAYOUT_PLAYING);g_assert_cmpstr(after.title,==,"Normal programme survives cancelled takeover");g_assert_cmpuint(after.position_ms,>,before.position_ms+100);
    emergency_start_native(&window);run_wait(&window,"Verified emergency announcement",500);click(window.root,"em-takeover-return");run_wait(&window,"Normal programme survives cancelled takeover",100);
    Terminal interrupted={"emergency","interrupted"};wait_until(terminal_record,&interrupted,"actual emergency source consumption remains durable after operator return");
    programme_stop(&window);json_node_free(normal);json_node_free(emergency);window_close(&window);
}

static RadioSAudioRunSnapshot runner_snapshot(Window *window)
{
    RadioSAudioEngine *engine=radios_audio_ui_engine_acquire(window->ctx);g_assert_nonnull(engine);
    RadioSAudioRunSnapshot snapshot;radios_audio_engine_runner_snapshot(engine,&snapshot);radios_audio_engine_unref(engine);return snapshot;
}
static gboolean runner_retired(gpointer data)
{
    RadioSAudioRunSnapshot snapshot=runner_snapshot(data);
    if(snapshot.state!=RADIOS_AUDIO_RUN_FINISHED)return FALSE;
    for(guint i=0;i<RADIOS_AUDIO_NORMAL_DECKS;++i)if(snapshot.owned[i])return FALSE;
    return TRUE;
}
static void exact_receipt(Window *window,guint64 epoch,RadioSAudioRunTerminal terminal,guint64 expected_frames,gboolean exact)
{
    RadioSAudioEngine *engine=radios_audio_ui_engine_acquire(window->ctx);g_assert_nonnull(engine);
    RadioSAudioRunReceipt receipts[RADIOS_AUDIO_RUN_MAX_RECEIPTS];
    guint count=radios_audio_engine_runner_receipts(engine,receipts,G_N_ELEMENTS(receipts));radios_audio_engine_unref(engine);
    guint matched=0;
    for(guint i=0;i<count;++i)if(receipts[i].epoch==epoch){++matched;g_assert_cmpuint(receipts[i].index,==,0);g_assert_cmpint(receipts[i].terminal,==,terminal);
        g_assert_cmpint(receipts[i].first_utc_us,>,0);g_assert_cmpint(receipts[i].last_observed_utc_us,>=,receipts[i].first_utc_us);
        if(exact)g_assert_cmpuint(receipts[i].position_frames,==,expected_frames);else{g_assert_cmpuint(receipts[i].position_frames,>,0);g_assert_cmpuint(receipts[i].position_frames,<,expected_frames);}
    }
    g_assert_cmpuint(matched,==,1);
}
static GHashTable *run_ids(void)
{
    GHashTable *ids=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);JsonNode *result=documents("broadcast_local_run");
    JsonArray *rows=json_object_get_array_member(json_node_get_object(result),"records");
    for(guint i=0;i<json_array_get_length(rows);++i)g_hash_table_add(ids,g_strdup(radios_json_string(json_array_get_object_element(rows,i),"_id","")));
    json_node_free(result);return ids;
}
typedef struct {GHashTable *old;gchar *id;const gchar *kind;} NewRun;
static gboolean new_run_created(gpointer data)
{
    NewRun *want=data;if(want->id)return TRUE;JsonNode *result=documents("broadcast_local_run");JsonArray *rows=json_object_get_array_member(json_node_get_object(result),"records");
    for(guint i=0;i<json_array_get_length(rows);++i){JsonObject *row=json_array_get_object_element(rows,i),*cfg=json_object_get_object_member(row,"config"),*manifest=json_object_get_object_member(cfg,"manifest");
        const gchar *id=radios_json_string(row,"_id","");if(!g_hash_table_contains(want->old,id)&&g_str_equal(radios_json_string(manifest,"kind",""),want->kind)){want->id=g_strdup(id);break;}
    }json_node_free(result);return want->id!=NULL;
}
static gchar *new_run_wait(GHashTable *old)
{
    NewRun want={old,NULL,"emergency"};wait_until(new_run_created,&want,"new durable emergency run identity");g_hash_table_unref(old);return want.id;
}
typedef struct {const gchar *id,*state;guint64 frames;gboolean exact;} ExactRun;
static gboolean exact_terminal(gpointer data)
{
    ExactRun *want=data;JsonNode *result=documents("broadcast_local_run");JsonArray *rows=json_object_get_array_member(json_node_get_object(result),"records");gboolean found=FALSE;
    for(guint i=0;i<json_array_get_length(rows);++i){JsonObject *row=json_array_get_object_element(rows,i);if(g_strcmp0(radios_json_string(row,"_id",""),want->id))continue;
        JsonObject *cfg=json_object_get_object_member(row,"config");const gchar *state=radios_json_string(cfg,"state","");
        if(!g_str_equal(state,"completed")&&!g_str_equal(state,"interrupted")&&!g_str_equal(state,"failed"))break;
        g_assert_cmpstr(state,==,want->state);g_assert_false(json_object_get_boolean_member(cfg,"delivery_proven"));
        JsonArray *events=json_object_get_array_member(cfg,"events");g_assert_cmpuint(json_array_get_length(events),>,0);
        JsonObject *last=json_array_get_object_element(events,json_array_get_length(events)-1);g_assert_cmpstr(radios_json_string(last,"state",""),==,want->state);
        gint64 frames=radios_json_int(last,"frames",-1);if(want->exact)g_assert_cmpuint((guint64)frames,==,want->frames);else{g_assert_cmpint(frames,>,0);g_assert_cmpuint((guint64)frames,<,want->frames);}found=TRUE;
    }json_node_free(result);return found;
}
static void emergency_clear(Window *window)
{
    click(window->root,"profile-emergency-tab");GtkWidget *list=find(window->root,"em-list-items");g_assert_true(GTK_IS_LIST_BOX(list));
    for(GtkListBoxRow *row=gtk_list_box_get_row_at_index(GTK_LIST_BOX(list),0);row;row=gtk_list_box_get_row_at_index(GTK_LIST_BOX(list),0)){
        gtk_list_box_select_row(GTK_LIST_BOX(list),row);click(window->root,"em-list-remove");
    }
    RadioSDocument *doc=document(window->root,"emergency-task-view");if(*radios_data_document_id(doc))wait_until(saved,doc,"empty emergency queue change acknowledges before replacement");
}
static void emergency_return_native(Window *window)
{
    GError *error=NULL;g_assert_true(radios_broadcast_ui_emergency_return(window->ctx,&error));g_assert_no_error(error);run_for(250);
}
typedef struct {Window *window;guint deck;const gchar *media_id;guint64 frames;} ManualWait;
static gboolean manual_pcm(gpointer data)
{
    ManualWait *want=data;RadioSAudioEngine *engine=radios_audio_ui_engine_acquire(want->window->ctx);g_assert_nonnull(engine);RadioSAudioSnapshot snapshot;
    radios_audio_engine_snapshot(engine,&snapshot);radios_audio_engine_unref(engine);
    return snapshot.decks[want->deck].state==RADIOS_AUDIO_PLAYING&&!g_strcmp0(snapshot.decks[want->deck].media_id,want->media_id)&&snapshot.decks[want->deck].cursor_frames>=want->frames&&snapshot.decks[want->deck].peak[0]>0.05;
}
static gboolean emergency_not_pending(gpointer data)
{return !radios_broadcast_ui_emergency_pending(((Window *)data)->ctx);}
typedef struct {Window *window;guint64 incoming;} OverlapWait;
static gboolean overlapping(gpointer data)
{
    OverlapWait *want=data;RadioSAudioRunSnapshot snapshot=runner_snapshot(want->window);
    return snapshot.state==RADIOS_AUDIO_RUN_OVERLAP&&snapshot.index==0&&snapshot.next_index==1&&snapshot.actual_next_frame>=want->incoming;
}
typedef struct { Window *window; guint64 first_epoch; } RepeatWait;
typedef struct { RadioSDocument *document; gint64 revision; } EmergencySaved;
static gboolean emergency_preference_saved(gpointer data)
{
    EmergencySaved *want = data;
    return radios_data_document_revision(want->document) > want->revision;
}
static gboolean emergency_repeat_terminal(gpointer data)
{
    ExactRun *want = data; JsonNode *result = documents("broadcast_local_run");
    JsonArray *rows = json_object_get_array_member(json_node_get_object(result), "records");
    gboolean found = FALSE;
    for (guint i = 0; i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (g_strcmp0(radios_json_string(row, "_id", ""), want->id)) continue;
        JsonObject *cfg = json_object_get_object_member(row, "config");
        if (g_strcmp0(radios_json_string(cfg, "state", ""), want->state)) break;
        JsonArray *events = json_object_get_array_member(cfg, "events");
        g_assert_cmpuint(json_array_get_length(events), >, 0);
        JsonObject *last = json_array_get_object_element(events, json_array_get_length(events) - 1);
        g_assert_cmpint(radios_json_int(last, "frames", 0), >, (gint64)want->frames);
        found = TRUE;
    }
    json_node_free(result); return found;
}
static gboolean emergency_repeated(gpointer data)
{
    RepeatWait *want = data; RadioSAudioRunSnapshot actual = runner_snapshot(want->window);
    return actual.epoch >= want->first_epoch + 2 && actual.actual_current_frame >= 4800;
}
static void test_emergency_continuous_repeat_return(void)
{
    Window window = window_open();
    JsonNode *normal = media_import(&window, "Normal source after continuous emergency", 48000 * 20, "music");
    JsonNode *emergency = media_import(&window, "Continuous emergency announcement", 48000, "announcement");
    emergency_clear(&window); emergency_pick(&window, emergency);
    GtkWidget *repeat = find(window.root, "emergency-repeat-policy"); g_assert_nonnull(repeat);
    EmergencySaved ack = {document(window.root, "emergency-task-view"), 0};
    ack.revision = radios_data_document_revision(ack.document);
    gtk_combo_box_set_active(GTK_COMBO_BOX(repeat), 1); click(window.root, "em-log-save");
    wait_until(emergency_preference_saved, &ack, "continuous emergency preference persists");
    programme_start(&window, normal);
    GHashTable *ids = run_ids(); emergency_start_native(&window); gchar *id = new_run_wait(ids);
    run_wait(&window, "Continuous emergency announcement", 100);
    RepeatWait repeated = {&window, runner_snapshot(&window).epoch};
    wait_until(emergency_repeated, &repeated, "emergency restarts through three actual MAIN PCM epochs");
    gchar *status = radios_broadcast_ui_emergency_status(window.ctx);
    g_assert_nonnull(strstr(status, "Sürekli tekrar")); g_free(status);
    emergency_return_native(&window); run_wait(&window, "Normal source after continuous emergency", 100);
    ExactRun interrupted = {id, "interrupted", 48000 * 2, FALSE};
    wait_until(emergency_repeat_terminal, &interrupted, "continuous emergency counts consumption across every repeated epoch");
    programme_stop(&window);
    ack.revision = radios_data_document_revision(ack.document);
    gtk_combo_box_set_active(GTK_COMBO_BOX(repeat), 0); click(window.root, "em-log-save");
    wait_until(emergency_preference_saved, &ack, "single-pass setting restored for other acceptance cases");
    g_free(id); json_node_free(normal); json_node_free(emergency); window_close(&window);
}
static void test_emergency_terminal_epoch_overlap_return(void)
{
    Window window=window_open();JsonNode *emergency=media_import(&window,"Six seconds of exact emergency receipts",48000*6,"announcement");
    emergency_clear(&window);emergency_pick(&window,emergency);
    GHashTable *ids=run_ids();emergency_start_native(&window);gchar *natural_id=new_run_wait(ids);run_wait(&window,"Six seconds of exact emergency receipts",200);
    RadioSAudioRunSnapshot natural=runner_snapshot(&window);g_assert_cmpuint(natural.count,==,1);wait_until(runner_retired,&window,"natural emergency EOF physically retires its MAIN ownership");
    exact_receipt(&window,natural.epoch,RADIOS_AUDIO_RUN_TERMINAL_COMPLETE,48000*6,TRUE);ExactRun complete={natural_id,"completed",48000*6,TRUE};wait_until(exact_terminal,&complete,"natural EOF classified completed from exact per-entry COMPLETE receipt");
    emergency_return_native(&window);
    ids=run_ids();emergency_start_native(&window);gchar *skipped_id=new_run_wait(ids);run_wait(&window,"Six seconds of exact emergency receipts",250);RadioSAudioRunSnapshot skipped=runner_snapshot(&window);
    GError *error=NULL;g_assert_true(radios_playout_ui_action(window.ctx,RADIOS_PLAYOUT_NEXT,&error));g_assert_no_error(error);wait_until(runner_retired,&window,"Next skip retires emergency ownership");
    exact_receipt(&window,skipped.epoch,RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED,48000*6,FALSE);ExactRun interrupted={skipped_id,"interrupted",48000*6,FALSE};wait_until(exact_terminal,&interrupted,"Next skips remain interrupted with measured partial PCM");emergency_return_native(&window);
    JsonNode *normal=media_import(&window,"Normal source before manual deck takeover",48000*20,"music"),*manual=media_import(&window,"Operator manual PCM must survive late emergency",48000*20,"music");programme_start(&window,normal);
    ids=run_ids();flag_file(".acceptance-broadcast-start-ready",FALSE);flag_file(".acceptance-broadcast-start-hold",TRUE);emergency_start_native(&window);flag_wait(".acceptance-broadcast-start-ready");gchar *cancelled_id=new_run_wait(ids);
    RadioSAudioRunSnapshot before=runner_snapshot(&window);g_assert_cmpint(before.deck_current,>=,0);guint manual_deck=(guint)before.deck_current;
    g_assert_true(radios_audio_ui_load_media(window.ctx,window.root,json_node_get_object(manual),manual_deck,TRUE,&error));g_assert_no_error(error);
    const gchar *manual_id=radios_json_string(json_object_get_object_member(json_node_get_object(manual),"media"),"_id","");ManualWait first={&window,manual_deck,manual_id,4800};wait_until(manual_pcm,&first,"manual deck takeover consumes PCM before held emergency callback arrives");
    RadioSAudioRunSnapshot taken=runner_snapshot(&window);g_assert_cmpuint(taken.epoch,>,before.epoch);flag_file(".acceptance-broadcast-start-hold",FALSE);wait_until(emergency_not_pending,&window,"late emergency response has been accounted for after manual epoch change");
    ManualWait later={&window,manual_deck,manual_id,24000};wait_until(manual_pcm,&later,"manual PCM continues after stale emergency prepare without a late cut");ExactRun cancelled={cancelled_id,"interrupted",0,TRUE};wait_until(exact_terminal,&cancelled,"manual epoch fence records zero-frame interrupted emergency");
    RadioSAudioEngine *engine=radios_audio_ui_engine_acquire(window.ctx);g_assert_nonnull(engine);g_assert_true(radios_audio_engine_unload(engine,manual_deck,&error));g_assert_no_error(error);radios_audio_engine_unref(engine);programme_stop(&window);
    JsonNode *outgoing=media_import(&window,"Crossfade outgoing source must not replay",48000*6,"music"),*incoming=media_import(&window,"Crossfade incoming returns at consumed position",48000*12,"music");
    JsonObject *programme=programme_for(outgoing,"Incoming source promotion on emergency return"),*tail=programme_for(incoming,"Incoming source promotion on emergency return");
    JsonArray *events=json_object_get_array_member(programme,"events"),*tail_events=json_object_get_array_member(tail,"events");json_array_add_element(events,json_node_copy(json_array_get_element(tail_events,0)));json_object_unref(tail);
    g_assert_true(radios_playout_ui_start_programme(window.ctx,window.root,programme,0,RADIOS_AUDIO_RUN_CROSSFADE,3,&error));g_assert_no_error(error);json_object_unref(programme);
    OverlapWait overlap={&window,24000};wait_until(overlapping,&overlap,"normal queue crossfades and incoming track actually consumes MAIN PCM");RadioSAudioRunSnapshot overlapped=runner_snapshot(&window);g_assert_cmpuint(overlapped.actual_next_frame,>=,24000);
    ids=run_ids();emergency_start_native(&window);gchar *overlap_run_id=new_run_wait(ids);run_wait(&window,"Six seconds of exact emergency receipts",300);click(window.root,"em-takeover-return");
    run_wait(&window,"Crossfade incoming returns at consumed position",0);RadioSAudioRunSnapshot restored=runner_snapshot(&window);g_assert_cmpuint(restored.actual_current_frame,>=,overlapped.actual_next_frame);
    g_assert_cmpuint(restored.actual_current_frame,>,24000);g_assert_cmpuint(restored.count,==,1);RadioSPlayoutSnapshot restored_ui;g_assert_true(radios_playout_ui_get_snapshot(window.ctx,&restored_ui));g_assert_cmpstr(restored_ui.title,==,"Crossfade incoming returns at consumed position");
    ExactRun overlap_interrupted={overlap_run_id,"interrupted",48000*6,FALSE};wait_until(exact_terminal,&overlap_interrupted,"crossfade emergency partial playback remains interrupted");programme_stop(&window);
    g_free(natural_id);g_free(skipped_id);g_free(cancelled_id);g_free(overlap_run_id);json_node_free(emergency);json_node_free(normal);json_node_free(manual);json_node_free(outgoing);json_node_free(incoming);window_close(&window);
}
static void test_shutdown_tail_durable_reopen(void)
{
    Window window = window_open();
    JsonNode *media = media_import(&window, "Stage13 interrupted record MAIN", 48000 * 10, "music");
    set(window.root, "broadcast-record-filename", "stage13-close-tail.wav"); save_profile(&window, "record");
    programme_start(&window, media);
    flag_file(".acceptance-broadcast-event-ready", FALSE); flag_file(".acceptance-broadcast-event-hold", TRUE);
    GHashTable *ids = run_ids(); click(window.root, "broadcast-record-start");
    output_wait(&window, "record", RADIOS_AUDIO_OUTPUT_ACTIVE, 0);
    NewRun created = {ids, NULL, "record"}; wait_until(new_run_created, &created, "new durable record identity");
    g_hash_table_unref(ids); gchar *id = created.id;
    flag_wait(".acceptance-broadcast-event-ready");
    output_wait(&window, "record", RADIOS_AUDIO_OUTPUT_ACTIVE, 4800);
    /* A committed ACTIVE HTTP acknowledgement is held while the real PCM
     * source and output close. The terminal successor must survive shutdown. */
    window_close(&window); flag_file(".acceptance-broadcast-event-hold", FALSE);
    GError *error = NULL; JsonNode *pending = radios_data_client_recovery_status(reader, &error); g_assert_no_error(error);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(pending), "pending"), >=, 1); json_node_free(pending);
    window = window_open(); ExactRun interrupted = {id, "interrupted", 48000 * 10, FALSE};
    wait_until(exact_terminal, &interrupted, "shutdown successor CAS replays after held ACTIVE receipt without duplicate run");
    RadioSAudioOutputSnapshot output; g_assert_true(radios_broadcast_ui_output_snapshot(window.ctx, "record", &output));
    g_assert_cmpint(output.state, ==, RADIOS_AUDIO_OUTPUT_IDLE); RadioSAudioRunSnapshot queue = runner_snapshot(&window);
    g_assert_cmpint(queue.state, ==, RADIOS_AUDIO_RUN_IDLE);
    pending = radios_data_recovery_status(window.ctx); g_assert_nonnull(pending);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(pending), "pending"), ==, 0); json_node_free(pending);
    g_free(id); json_node_free(media); window_close(&window);
}
int main(int argc,char **argv)
{
    g_assert_cmpint(argc,>=,2);gchar *root=g_strdup(argv[1]);for(gint i=1;i<argc;++i)argv[i]=argv[i+1];--argc;
    g_test_init(&argc,&argv,NULL);gtk_init(&argc,&argv);main_sink=g_getenv("RADIOS_TEST_MAIN_SINK");cue_sink=g_getenv("RADIOS_TEST_CUE_SINK");monitor_sink=g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink);g_assert_nonnull(cue_sink);g_assert_nonnull(monitor_sink);g_assert_true(g_str_has_prefix(main_sink,"radios_gui_"));
    GError *error=NULL;g_assert_true(radios_config_load(root,NULL,&config,&error));g_assert_no_error(error);g_free(root);
    application=gtk_application_new("org.radios.NativeBroadcastAcceptance",G_APPLICATION_NON_UNIQUE);g_assert_true(g_application_register(G_APPLICATION(application),NULL,&error));g_assert_no_error(error);
    reader=radios_data_client_new(&config,&error);g_assert_no_error(error);g_assert_nonnull(reader);
    g_test_add_func("/native-broadcast/record-main-pcm-no-overwrite-reopen",test_record_main_pcm_no_overwrite_reopen);
    g_test_add_func("/native-broadcast/stream-mp3-metadata-secret-ephemeral",test_stream_encoded_metadata_secret_ephemeral);
    g_test_add_func("/native-broadcast/stream-profile-refresh-keeps-draft-and-saved-password",test_stream_profile_refresh_keeps_draft_and_saved_password);
    g_test_add_func("/native-broadcast/emergency-verified-takeover-return-pending-cancel",test_emergency_verified_takeover_return_pending_cancel);
    g_test_add_func("/native-broadcast/emergency-terminals-manual-epoch-crossfade-return",test_emergency_terminal_epoch_overlap_return);
    g_test_add_func("/native-broadcast/emergency-continuous-repeat-return",test_emergency_continuous_repeat_return);
    g_test_add_func("/native-broadcast/shutdown-tail-durable-reopen", test_shutdown_tail_durable_reopen);
    gint result=g_test_run();radios_data_client_unref(reader);g_object_unref(application);radios_config_clear(&config);return result;
}
