#include "radios/event_playout.h"
#include "radios/player_scope_execution.h"
#include "radios/local_event_prepare.h"
#include "radios/programme_model.h"
#include "radios/audio_ui.h"
#include "radios/audio_runner.h"
#include "radios/header_navigation.h"
#include <string.h>

typedef struct EventService EventService;
typedef struct EventJob EventJob;
typedef enum { EVENT_MATERIALIZING, EVENT_STARTING, EVENT_RUNNING, EVENT_CONTINUING } EventStep;
struct EventService {
    guint refs, timer;
    RadioSViewContext *ctx;
    GtkWidget *request_owner;
    EventJob *jobs[32];
    JsonArray *completed; /* bounded terminal notifications, consumed by the event GUI */
    RadioSDataClient *completed_client;
    gboolean closed;
};
struct EventJob {
    guint refs;
    EventService *service;
    RadioSPlayerWorkspace *workspace;
    RadioSDataClient *client;
    GWeakRef owner;
    gulong owner_destroy;
    GCancellable *materialization;
    RadioSAutomationBinding source, target;
    JsonObject *programme; /* immutable formal config accepted for this event */
    gchar origin_block_id[37];
    gchar automation_id[37]; /* kılavuz A-2 / 4c live.automation_id, carried and logged only */
    gboolean has_target, scope_pending, completed;
    EventStep step;
    guint64 generations[32], target_intents[32], target_epochs[32];
    guint64 global_intent, source_intent, source_epoch, finished_generation, loss_epoch;
};

static EventService *service_ref(EventService *service) { ++service->refs; return service; }
static EventJob *job_ref(EventJob *job) { ++job->refs; return job; }
static void service_unref(gpointer value);
static void job_finish(EventJob *job, const GError *error);
static gboolean event_tick(gpointer value);
static gboolean fail(GError **error, GIOErrorEnum code, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, code, message); return FALSE; }
static RadioSPlayerScopeExecution *execution(EventJob *job)
{ return radios_player_scope_execution_get(job->service->ctx); }
static void job_unref(gpointer value)
{
    EventJob *job = value; if (--job->refs) return;
    GtkWidget *owner = g_weak_ref_get(&job->owner);
    if (owner && job->owner_destroy && g_signal_handler_is_connected(owner, job->owner_destroy))
        g_signal_handler_disconnect(owner, job->owner_destroy);
    g_clear_object(&owner); g_weak_ref_clear(&job->owner);
    g_clear_object(&job->materialization);
    g_clear_pointer(&job->programme, json_object_unref);
    if (job->client) radios_data_client_unref(job->client);
    service_unref(job->service); g_free(job);
}
static void owner_destroyed(GtkWidget *owner, gpointer value)
{
    (void)owner; EventJob *job = value; job->owner_destroy = 0;
    job_finish(job, NULL);
}
static void job_finish(EventJob *job, const GError *error)
{
    EventService *service = job->service;
    if (service->jobs[job->source.source] != job) return;
    job_ref(job); service->jobs[job->source.source] = NULL;
    if (job->materialization) g_cancellable_cancel(job->materialization);
    /* Pending scope preparation retains its callback reference. Removing this
     * intent makes its synchronous pre-install guard fail; do not recursively
     * call scope Stop or affect another owner's running audio here. */
    if (error && !service->closed && service->ctx->notify)
        service->ctx->notify(service->ctx->notify_data, error->message);
    job_unref(job); /* service slot */
    job_unref(job); /* local reentrancy guard */
}
static void service_close(EventService *service)
{
    if (service->closed) return;
    service->closed = TRUE;
    if (service->timer) { g_source_remove(service->timer); service->timer = 0; }
    for (guint i = 0; i < 32; ++i) if (service->jobs[i]) job_finish(service->jobs[i], NULL);
    if (service->request_owner) {
        gtk_widget_destroy(service->request_owner); g_object_unref(service->request_owner); service->request_owner = NULL;
    }
    g_clear_pointer(&service->completed, json_array_unref);
    if (service->completed_client) { radios_data_client_unref(service->completed_client); service->completed_client = NULL; }
    service->ctx = NULL;
}
static void service_unref(gpointer value)
{
    EventService *service = value; if (--service->refs) return;
    service_close(service); g_free(service);
}
static void window_destroyed(GtkWidget *window, gpointer value)
{ (void)window; service_close(value); }
static EventService *service_get(RadioSViewContext *ctx, gboolean create)
{
    if (!ctx || !GTK_IS_WINDOW(ctx->parent) || gtk_widget_in_destruction(GTK_WIDGET(ctx->parent))) return NULL;
    EventService *service = g_object_get_data(G_OBJECT(ctx->parent), "radios-event-playout");
    if (service) return service->closed ? NULL : service;
    if (!create) return NULL;
    service = g_new0(EventService, 1); service->refs = 1; service->ctx = ctx;
    service->request_owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(service->request_owner);
    service->timer = g_timeout_add(50, event_tick, service);
    g_signal_connect(ctx->parent, "destroy", G_CALLBACK(window_destroyed), service);
    g_object_set_data_full(G_OBJECT(ctx->parent), "radios-event-playout", service, service_unref);
    return service;
}
static gboolean common_current(EventJob *job, GError **error)
{
    EventService *service = job->service;
    if (service->closed || service->jobs[job->source.source] != job)
        return fail(error, G_IO_ERROR_CANCELLED, "Event isteği iptal edildi.");
    GtkWidget *owner = g_weak_ref_get(&job->owner);
    gboolean alive = owner && !gtk_widget_in_destruction(owner);
    g_clear_object(&owner);
    if (!alive) return fail(error, G_IO_ERROR_CANCELLED, "Event penceresi kapandı; devam geçişi iptal edildi.");
    RadioSViewContext *ctx = service->ctx;
    if (ctx->players != job->workspace || !radios_player_workspace_ready(job->workspace))
        return fail(error, G_IO_ERROR_CANCELLED, "Event'in oynatıcı çalışma oturumu değişti.");
    RadioSDataClient *current = radios_data_client_acquire(ctx);
    gboolean same = current && radios_data_connected(ctx) && radios_data_client_same_identity(current, job->client);
    if (current) radios_data_client_unref(current);
    if (!same) return fail(error, G_IO_ERROR_CANCELLED, "Event hazırlanırken veri kimliği değişti; mevcut yayın korundu.");
    if (radios_playout_ui_intent(ctx) != job->global_intent)
        return fail(error, G_IO_ERROR_CANCELLED, "Yeni yayın işlemi Event'in bekleyen devam geçişini iptal etti.");
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(job->workspace);
    if (!radios_automation_scope_binding_current(topology, &job->source, error) ||
        (job->has_target && !radios_automation_scope_binding_current(topology, &job->target, error))) return FALSE;
    return TRUE;
}
static gboolean deck_idle(EventJob *job, RadioSAudioEngine *engine, const RadioSAudioSnapshot *audio,
    guint deck, GError **error)
{
    const RadioSAudioDeckSnapshot *state = &audio->decks[deck]; gint runner_owner = -2;
    if (!state->main_routed || state->output_bus != RADIOS_AUDIO_BUS_MAIN || state->external_bound ||
        state->state != RADIOS_AUDIO_EMPTY || radios_audio_engine_deck_runner_owner(engine, deck, &runner_owner))
        return fail(error, G_IO_ERROR_BUSY, "Event için seçilen oynatıcı meşgul veya MAIN dışında; çalan ve CUE kaynakları korunuyor.");
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = deck};
    RadioSPlayerScopeExecution *scope_execution = execution(job);
    if (scope_execution && radios_player_scope_execution_busy(scope_execution, &scope))
        return fail(error, G_IO_ERROR_BUSY, "Event oynatıcısında bekleyen başka bir hazırlık var.");
    return TRUE;
}
static gboolean target_current(EventJob *job, RadioSAudioEngine *engine,
    const RadioSAudioSnapshot *audio, gboolean before_install, GError **error)
{
    if (!job->has_target) return TRUE;
    for (guint i = 0; i < job->target.count; ++i) {
        guint deck = job->target.decks[i]; RadioSAudioRunSnapshot run;
        RadioSViewContext *ctx = radios_player_workspace_source_context(job->workspace, deck);
        radios_audio_engine_player_runner_snapshot(engine, deck, &run);
        if (audio->decks[deck].generation != job->generations[deck] || run.epoch != job->target_epochs[deck] ||
            radios_playout_ui_intent(ctx) != job->target_intents[deck])
            return fail(error, G_IO_ERROR_CANCELLED, "Devam kümesinin oynatıcısı veya yayın isteği değişti; mevcut ses korundu.");
        /* Our own target preparation occupies the scope only in CONTINUING.
         * Before that point, no pending foreign scope may be replaced. */
        if (before_install && job->step != EVENT_CONTINUING && !deck_idle(job, engine, audio, deck, error)) return FALSE;
        if (before_install && job->step == EVENT_CONTINUING) {
            const RadioSAudioDeckSnapshot *state = &audio->decks[deck]; gint runner_owner = -2;
            if (!state->main_routed || state->output_bus != RADIOS_AUDIO_BUS_MAIN || state->external_bound ||
                state->state != RADIOS_AUDIO_EMPTY || radios_audio_engine_deck_runner_owner(engine, deck, &runner_owner))
                return fail(error, G_IO_ERROR_BUSY, "Devam kümesi meşgul; çalışan kaynaklar korundu.");
        }
    }
    return TRUE;
}
static gboolean job_current(EventJob *job, gboolean preparing, GError **error)
{
    if (!common_current(job, error)) return FALSE;
    RadioSViewContext *ctx = job->service->ctx;
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx);
    if (!engine) return fail(error, G_IO_ERROR_NOT_INITIALIZED, "Event ses motoru hazır değil.");
    RadioSAudioSnapshot audio; RadioSAudioRunSnapshot run;
    radios_audio_engine_snapshot(engine, &audio);
    radios_audio_engine_player_runner_snapshot(engine, job->source.source, &run);
    RadioSViewContext *source_ctx = radios_player_workspace_source_context(job->workspace, job->source.source);
    gboolean valid = radios_playout_ui_intent(source_ctx) == job->source_intent && run.epoch == job->source_epoch;
    if (!valid) fail(error, G_IO_ERROR_CANCELLED, "Event'in oynatıcı sahipliği veya yayın isteği değişti; yeni ses korundu.");
    if (valid && audio.loss_epoch != job->loss_epoch)
        valid = fail(error, G_IO_ERROR_CANCELLED, "Ses çıkışı bağlantısı değişti; bekleyen Event veya devam geçişini yeniden başlatın.");
    if (valid && preparing) {
        guint source = job->source.source; const RadioSAudioDeckSnapshot *state = &audio.decks[source]; gint owner = -2;
        if (state->generation != job->generations[source] || state->state != RADIOS_AUDIO_EMPTY ||
            !state->main_routed || state->output_bus != RADIOS_AUDIO_BUS_MAIN || state->external_bound ||
            radios_audio_engine_deck_runner_owner(engine, source, &owner))
            valid = fail(error, G_IO_ERROR_CANCELLED, "Event hazırlanırken kaynak oynatıcı veya MAIN bağlantısı değişti; mevcut ses korundu.");
    }
    if (valid && preparing && !job->scope_pending &&
        radios_player_scope_execution_busy(execution(job), &job->source.scope))
        valid = fail(error, G_IO_ERROR_BUSY, "Event hazırlanırken kaynakta başka bir hazırlık başlatıldı; o istek korundu.");
    if (valid && job->step == EVENT_CONTINUING &&
        (run.state != RADIOS_AUDIO_RUN_FINISHED || audio.decks[job->source.source].generation != job->finished_generation ||
         audio.decks[job->source.source].state != RADIOS_AUDIO_EMPTY))
        valid = fail(error, G_IO_ERROR_CANCELLED, "Event tamamlandıktan sonra kaynak değişti; devam geçişi iptal edildi.");
    if (valid) valid = target_current(job, engine, &audio, TRUE, error);
    radios_audio_engine_unref(engine); return valid;
}
static gboolean scope_guard(RadioSViewContext *ctx, const RadioSAutomationScope *scope,
    guint source, gpointer value, GError **error)
{
    (void)ctx; (void)scope; (void)source;
    EventJob *job = value; return job_current(job, job->step == EVENT_STARTING, error);
}
static void continued(RadioSViewContext *ctx, const RadioSAutomationScope *scope,
    guint source, const GError *error, gpointer value)
{
    (void)ctx; (void)scope; (void)source; EventJob *job = value; job->scope_pending = FALSE;
    if (job->service->closed || job->service->jobs[job->source.source] != job) return;
    job_finish(job, error);
}
static void started(RadioSViewContext *ctx, const RadioSAutomationScope *scope,
    guint source, const GError *error, gpointer value)
{
    (void)scope; (void)source; EventJob *job = value; job->scope_pending = FALSE;
    if (job->service->closed || job->service->jobs[job->source.source] != job) return;
    if (error) { job_finish(job, error); return; }
    GError *failure = NULL;
    if (!common_current(job, &failure)) { job_finish(job, failure); g_clear_error(&failure); return; }
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx); RadioSAudioRunSnapshot run = {0};
    if (engine) { radios_audio_engine_player_runner_snapshot(engine, job->source.source, &run); radios_audio_engine_unref(engine); }
    if (!run.epoch || !run.count) {
        failure = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Event'in kabul edilen ses sırası bulunamadı.");
        job_finish(job, failure); g_error_free(failure); return;
    }
    job->source_epoch = run.epoch;
    job->source_intent = radios_playout_ui_intent(radios_player_workspace_source_context(job->workspace, job->source.source));
    job->step = EVENT_RUNNING;
}
static gboolean start_programme(EventJob *job, JsonObject *programme, GError **error)
{
    if (!job_current(job, TRUE, error)) return FALSE;
    g_clear_pointer(&job->programme, json_object_unref);
    job->programme = radios_programme_clone_object(programme);
    job->step = EVENT_STARTING; job->scope_pending = TRUE;
    return radios_player_scope_execution_play_programme_guarded(execution(job), &job->source.scope,
        programme, RADIOS_AUTODJ_SEMI, scope_guard, started, job_ref(job), job_unref, error);
}
static void materialized(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *programme,
    const GError *error, gpointer value)
{
    (void)ctx; (void)owner; EventJob *job = value;
    if (job->service->closed || job->service->jobs[job->source.source] != job) return;
    GError *failure = error ? g_error_copy(error) : NULL;
    if (!failure && !start_programme(job, programme, &failure) && !failure)
        fail(&failure, G_IO_ERROR_FAILED, "Yerel Event başlatılamadı.");
    if (failure) { job->scope_pending = FALSE; job_finish(job, failure); g_error_free(failure); }
}
static RadioSAudioRunTerminal final_terminal(RadioSAudioEngine *engine, EventJob *job, const RadioSAudioRunSnapshot *run)
{
    RadioSAudioRunReceipt receipts[RADIOS_AUDIO_RUN_MAX_RECEIPTS];
    guint count = radios_audio_engine_player_runner_receipts(engine, job->source.source, receipts, G_N_ELEMENTS(receipts));
    for (guint i = count; i > 0; --i) {
        const RadioSAudioRunReceipt *receipt = &receipts[i - 1];
        if (receipt->epoch == job->source_epoch && receipt->index + 1 == run->count)
            return receipt->terminal;
    }
    return RADIOS_AUDIO_RUN_TERMINAL_NONE;
}
static void completed_origin_forget(EventService *service, const gchar *origin)
{
    if (!service->completed || !origin || !*origin) return;
    for (guint i = json_array_get_length(service->completed); i > 0; --i) {
        JsonObject *row = json_array_get_object_element(service->completed, i - 1);
        if (!g_strcmp0(radios_json_string(row, "origin_block_id", ""), origin))
            json_array_remove_element(service->completed, i - 1);
    }
}
static void record_complete(EventJob *job)
{
    if (job->completed) return;
    job->completed = TRUE;
    if (!*job->origin_block_id) return;
    EventService *service = job->service;
    if (!radios_data_client_same_identity(service->completed_client, job->client)) {
        g_clear_pointer(&service->completed, json_array_unref);
        if (service->completed_client) radios_data_client_unref(service->completed_client);
        service->completed_client = radios_data_client_ref(job->client);
    }
    completed_origin_forget(service, job->origin_block_id);
    if (!service->completed) service->completed = json_array_new();
    if (json_array_get_length(service->completed) >= 64) json_array_remove_element(service->completed, 0);
    JsonObject *row = json_object_new();
    json_object_set_string_member(row, "origin_block_id", job->origin_block_id);
    json_object_set_int_member(row, "source", job->source.source);
    json_array_add_object_element(service->completed, row);
}
static void continue_cluster(EventJob *job)
{
    GError *error = NULL;
    if (!job_current(job, FALSE, &error)) { job_finish(job, error); g_clear_error(&error); return; }
    RadioSAutoDJMode mode = job->target.count >= 2 ? RADIOS_AUTODJ_TRANSITIONS : RADIOS_AUTODJ_SEMI;
    job->scope_pending = TRUE;
    gboolean accepted = job->target.automation_id[0] ?
        radios_player_scope_execution_play_schema_guarded(execution(job), &job->target.scope, mode,
            scope_guard, continued, job_ref(job), job_unref, &error) :
        radios_player_scope_execution_play_programme_guarded(execution(job), &job->target.scope, NULL, mode,
            scope_guard, continued, job_ref(job), job_unref, &error);
    if (!accepted) { job->scope_pending = FALSE; job_finish(job, error); }
    g_clear_error(&error);
}
static gboolean event_tick(gpointer value)
{
    EventService *service = value;
    if (service->closed) { service->timer = 0; return G_SOURCE_REMOVE; }
    service_ref(service); /* notifications may close/finalize the app owner */
    for (guint i = 0; i < 32 && !service->closed; ++i) {
        EventJob *job = service->jobs[i]; if (!job) continue;
        job_ref(job); GError *error = NULL;
        if (!job_current(job, job->step == EVENT_MATERIALIZING || job->step == EVENT_STARTING, &error)) {
            job_finish(job, error); g_clear_error(&error); job_unref(job); continue;
        }
        if (job->step == EVENT_RUNNING) {
            RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(service->ctx); RadioSAudioRunSnapshot run;
            RadioSAudioSnapshot audio;
            radios_audio_engine_player_runner_snapshot(engine, i, &run); radios_audio_engine_snapshot(engine, &audio);
            if (run.state == RADIOS_AUDIO_RUN_FINISHED) {
                gboolean owned = FALSE;
                for (guint deck = 0; deck < 32; ++deck) owned |= run.owned[deck];
                RadioSAudioRunTerminal terminal = final_terminal(engine, job, &run);
                radios_audio_engine_unref(engine);
                if (terminal == RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED || terminal == RADIOS_AUDIO_RUN_TERMINAL_FAILED)
                    job_finish(job, NULL);
                else if (!owned && audio.decks[i].state == RADIOS_AUDIO_EMPTY && terminal == RADIOS_AUDIO_RUN_TERMINAL_COMPLETE) {
                    job->finished_generation = audio.decks[i].generation;
                    record_complete(job);
                    if (job->has_target) { job->step = EVENT_CONTINUING; continue_cluster(job); }
                    else job_finish(job, NULL);
                } else if (!owned && audio.decks[i].state != RADIOS_AUDIO_EMPTY) {
                    /* Lease release empties this source atomically. A newly
                     * occupied source after release is a manual takeover. */
                    job_finish(job, NULL);
                }
                /* FINISHED may become audible before owned deck cleanup or
                 * its COMPLETE receipt. Keep waiting for both observations. */
            } else {
                radios_audio_engine_unref(engine);
                if (run.state == RADIOS_AUDIO_RUN_IDLE || run.state == RADIOS_AUDIO_RUN_ERROR) job_finish(job, NULL);
            }
        }
        job_unref(job);
    }
    gboolean keep = !service->closed;
    service_unref(service); return keep ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
static EventJob *job_new(RadioSViewContext *ctx, GtkWidget *owner, guint requested,
    const gchar *next_cluster_id, GError **error)
{
    if (!ctx || !GTK_IS_WIDGET(owner) || gtk_widget_in_destruction(owner) || !ctx->players ||
        !radios_player_workspace_ready(ctx->players) || !radios_data_connected(ctx)) {
        fail(error, G_IO_ERROR_NOT_INITIALIZED, "Event için veritabanı ve canlı oynatıcılar hazır olmalı."); return NULL;
    }
    EventService *service = service_get(ctx, TRUE);
    if (!service) { fail(error, G_IO_ERROR_CLOSED, "Event çalışma penceresi kapandı."); return NULL; }
    EventJob *job = g_new0(EventJob, 1); job->refs = 1; job->service = service_ref(service); job->workspace = ctx->players;
    job->client = radios_data_client_acquire(ctx); g_weak_ref_init(&job->owner, owner);
    job->global_intent = radios_playout_ui_intent(ctx);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx); RadioSAudioSnapshot audio;
    if (!engine || !job->client) {
        if (engine) radios_audio_engine_unref(engine);
        fail(error, G_IO_ERROR_NOT_INITIALIZED, "Event ses motoru veya veri oturumu hazır değil."); job_unref(job); return NULL;
    }
    radios_audio_engine_snapshot(engine, &audio); job->loss_epoch = audio.loss_epoch;
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(ctx->players);
    guint selected = G_MAXUINT;
    for (guint deck = 0; deck < 32; ++deck) {
        if (requested != G_MAXUINT && requested != deck) continue;
        const RadioSPlayerDefinition *player = radios_player_topology_player_const(topology, deck);
        if (!player || player->cluster_id[0] || service->jobs[deck]) continue;
        if (deck_idle(job, engine, &audio, deck, NULL)) { selected = deck; break; }
    }
    if (selected == G_MAXUINT) {
        fail(error, G_IO_ERROR_BUSY, "Event için boş, kümesiz ve MAIN'e atanmış oynatıcı bulunamadı.");
        radios_audio_engine_unref(engine); job_unref(job); return NULL;
    }
    RadioSAutomationScope source = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = selected};
    gboolean valid = radios_automation_scope_resolve(topology, &source, &job->source, error);
    if (valid && next_cluster_id && *next_cluster_id) {
        RadioSAutomationScope target = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER};
        if (!g_uuid_string_is_valid(next_cluster_id)) valid = fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Devam kümesi kimliği geçersiz.");
        else {
            g_strlcpy(target.cluster_id, next_cluster_id, sizeof target.cluster_id);
            valid = radios_automation_scope_resolve(topology, &target, &job->target, error);
            if (valid && !job->target.count) valid = fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Devam kümesinde oynatıcı yok.");
            job->has_target = valid;
        }
        for (guint i = 0; valid && i < job->target.count; ++i)
            valid = deck_idle(job, engine, &audio, job->target.decks[i], error);
    }
    if (valid) {
        RadioSAudioRunSnapshot run; radios_audio_engine_player_runner_snapshot(engine, selected, &run);
        job->source_epoch = run.epoch;
        job->source_intent = radios_playout_ui_intent(radios_player_workspace_source_context(job->workspace, selected));
        job->generations[selected] = audio.decks[selected].generation;
        for (guint i = 0; i < job->target.count; ++i) {
            guint deck = job->target.decks[i]; radios_audio_engine_player_runner_snapshot(engine, deck, &run);
            job->generations[deck] = audio.decks[deck].generation; job->target_epochs[deck] = run.epoch;
            job->target_intents[deck] = radios_playout_ui_intent(radios_player_workspace_source_context(job->workspace, deck));
        }
        service->jobs[selected] = job;
        job->owner_destroy = g_signal_connect(owner, "destroy", G_CALLBACK(owner_destroyed), job);
    }
    radios_audio_engine_unref(engine);
    if (!valid) { job_unref(job); return NULL; }
    return job;
}
static gboolean valid_origin(const gchar *origin, GError **error)
{
    return !origin || !*origin || g_uuid_string_is_valid(origin) ||
        fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Event'in yayın sırası kimliği geçersiz.");
}
static void origin_set(EventJob *job, const gchar *origin, JsonObject *programme)
{
    if (origin && *origin) g_strlcpy(job->origin_block_id, origin, sizeof job->origin_block_id);
    else if (programme) {
        RadioSProgrammeTree *tree = radios_programme_tree_new(programme);
        if (tree->blocks->len) {
            RadioSProgrammeBlock *block = g_ptr_array_index(tree->blocks, 0);
            const gchar *id = radios_json_string(block->block, "id", "");
            if (g_uuid_string_is_valid(id)) g_strlcpy(job->origin_block_id, id, sizeof job->origin_block_id);
        }
        radios_programme_tree_free(tree);
    }
    completed_origin_forget(job->service, job->origin_block_id);
}
gboolean radios_event_playout_programme_for_event(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *programme, guint deck, const gchar *next_cluster_id,
    const gchar *origin_block_id, GError **error)
{
    if (!programme) return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Event'in yayın sırası eksik.");
    if (!valid_origin(origin_block_id, error)) return FALSE;
    EventJob *job = job_new(ctx, owner, deck, next_cluster_id, error); if (!job) return FALSE;
    origin_set(job, origin_block_id, programme);
    /* Kılavuz A-2 (4c schema contract): the event's live.automation_id arrives
     * as a transient top-level member. It is recorded for the run and logged;
     * the formal programme handed to preparation never carries it. */
    const gchar *automation = radios_json_string(programme, "automation_id", "");
    JsonObject *formal = programme;
    if (*automation) {
        g_strlcpy(job->automation_id, automation, sizeof job->automation_id);
        g_debug("event playout: event %s starts with automation %s", job->origin_block_id, automation);
        formal = radios_programme_clone_object(programme); json_object_remove_member(formal, "automation_id");
    }
    gboolean accepted = start_programme(job, formal, error);
    if (formal != programme) json_object_unref(formal);
    if (!accepted) { job->scope_pending = FALSE; job_finish(job, NULL); }
    return accepted;
}
gboolean radios_event_playout_programme(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *programme, guint deck, const gchar *next_cluster_id, GError **error)
{
    return radios_event_playout_programme_for_event(ctx, owner, programme, deck, next_cluster_id, NULL, error);
}
gboolean radios_event_playout_local_for_event(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *local_config, guint deck, const gchar *next_cluster_id,
    const gchar *origin_block_id, GError **error)
{
    if (!local_config) return fail(error, G_IO_ERROR_INVALID_ARGUMENT, "Yerel Event tarifi eksik.");
    if (!valid_origin(origin_block_id, error)) return FALSE;
    EventJob *job = job_new(ctx, owner, deck, next_cluster_id, error); if (!job) return FALSE;
    origin_set(job, origin_block_id, NULL);
    job->step = EVENT_MATERIALIZING;
    job->materialization = radios_local_event_prepare_async(ctx, job->service->request_owner, local_config,
        materialized, job_ref(job), job_unref);
    if (!job->materialization) {
        job_finish(job, NULL); return fail(error, G_IO_ERROR_FAILED, "Yerel Event hazırlığı başlatılamadı.");
    }
    return TRUE;
}
gboolean radios_event_playout_local(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *local_config, guint deck, const gchar *next_cluster_id, GError **error)
{
    return radios_event_playout_local_for_event(ctx, owner, local_config, deck, next_cluster_id, NULL, error);
}
static RadioSPlayoutState event_state(RadioSAudioRunState state)
{
    switch (state) {
    case RADIOS_AUDIO_RUN_IDLE: return RADIOS_PLAYOUT_IDLE;
    case RADIOS_AUDIO_RUN_PREROLL: return RADIOS_PLAYOUT_PREPARING;
    case RADIOS_AUDIO_RUN_SCHEDULED: return RADIOS_PLAYOUT_SCHEDULED;
    case RADIOS_AUDIO_RUN_PLAYING: return RADIOS_PLAYOUT_PLAYING;
    case RADIOS_AUDIO_RUN_OVERLAP: return RADIOS_PLAYOUT_OVERLAP;
    case RADIOS_AUDIO_RUN_PAUSED: return RADIOS_PLAYOUT_PAUSED;
    case RADIOS_AUDIO_RUN_WAITING_MEDIA: return RADIOS_PLAYOUT_WAITING;
    case RADIOS_AUDIO_RUN_MANUAL_WAIT: return RADIOS_PLAYOUT_MANUAL_WAIT;
    case RADIOS_AUDIO_RUN_FINISHED: return RADIOS_PLAYOUT_FINISHED;
    case RADIOS_AUDIO_RUN_ERROR: return RADIOS_PLAYOUT_ERROR;
    }
    return RADIOS_PLAYOUT_ERROR;
}
static JsonNode *event_snapshot(RadioSViewContext *ctx, gboolean include_programme)
{
    JsonObject *result = json_object_new(); JsonArray *runs = json_array_new();
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, result);
    json_object_set_array_member(result, "runs", runs);
    EventService *service = service_get(ctx, FALSE); if (!service) return node;
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx); if (!engine) return node;
    for (guint source = 0; source < 32; ++source) {
        EventJob *job = service->jobs[source]; if (!job) continue;
        gboolean preparing = job->step == EVENT_MATERIALIZING || job->step == EVENT_STARTING;
        if (!job_current(job, preparing, NULL)) continue;
        RadioSAudioRunSnapshot run;
        radios_audio_engine_player_runner_snapshot(engine, source, &run);
        if (run.epoch != job->source_epoch) continue;
        RadioSPlayoutSnapshot actual = {0};
        RadioSViewContext *source_ctx = radios_player_workspace_source_context(job->workspace, source);
        gboolean observed = !preparing && radios_playout_ui_get_snapshot(source_ctx, &actual) &&
            !g_strcmp0(actual.entry_id, run.entry_id);
        JsonObject *row = json_object_new();
        json_object_set_string_member(row, "origin_block_id", job->origin_block_id);
        json_object_set_string_member(row, "automation_id", job->automation_id);
        json_object_set_int_member(row, "source", source);
        const RadioSPlayerTopology *topology = radios_player_workspace_topology(job->workspace);
        const RadioSPlayerDefinition *player = radios_player_topology_player_const(topology, source);
        const RadioSPlayerCluster *cluster = job->has_target ?
            radios_player_topology_cluster_const(topology, job->target.scope.cluster_id) : NULL;
        gchar default_name[64]; g_snprintf(default_name, sizeof default_name, "Oynatıcı %u", source + 1);
        json_object_set_string_member(row, "source_name", player && *player->name ? player->name : default_name);
        json_object_set_string_member(row, "next_cluster_id", job->has_target ? job->target.scope.cluster_id : "");
        json_object_set_string_member(row, "next_cluster_name", cluster ? cluster->name : "");
        json_object_set_int_member(row, "state", preparing ? RADIOS_PLAYOUT_PREPARING : event_state(run.state));
        json_object_set_string_member(row, "entry_id", preparing ? "" : run.entry_id);
        json_object_set_string_member(row, "next_entry_id", observed ? actual.next_entry_id : "");
        json_object_set_int_member(row, "position_ms", preparing ? 0 : (gint64)(run.actual_current_frame / 48));
        json_object_set_int_member(row, "duration_ms", observed ? (gint64)actual.duration_ms : 0);
        json_object_set_int_member(row, "cue_in_ms", observed ? (gint64)actual.cue_in_ms : 0);
        json_object_set_boolean_member(row, "completed", job->completed);
        json_object_set_boolean_member(row, "continuing", job->step == EVENT_CONTINUING);
        if (include_programme && job->programme)
            json_object_set_object_member(row, "programme", radios_programme_clone_object(job->programme));
        json_array_add_object_element(runs, row);
    }
    radios_audio_engine_unref(engine); return node;
}
JsonNode *radios_event_playout_snapshot(RadioSViewContext *ctx)
{ return event_snapshot(ctx, TRUE); }
JsonNode *radios_event_playout_display_snapshot(RadioSViewContext *ctx)
{ return event_snapshot(ctx, FALSE); }
JsonArray *radios_event_playout_take_completed(RadioSViewContext *ctx)
{
    EventService *service = service_get(ctx, FALSE);
    if (!service || !service->completed) return json_array_new();
    JsonArray *completed = service->completed; service->completed = NULL;
    RadioSDataClient *current = radios_data_client_acquire(ctx);
    gboolean same = current && radios_data_connected(ctx) &&
        radios_data_client_same_identity(current, service->completed_client);
    if (current) radios_data_client_unref(current);
    if (service->completed_client) { radios_data_client_unref(service->completed_client); service->completed_client = NULL; }
    if (!same) { json_array_unref(completed); return json_array_new(); }
    return completed;
}
void radios_event_playout_cancel(RadioSViewContext *ctx, guint deck)
{
    EventService *service = service_get(ctx, FALSE); if (!service) return;
    for (guint i = 0; i < 32; ++i) {
        EventJob *job = service->jobs[i]; if (!job) continue;
        gboolean matches = deck == G_MAXUINT || deck == i;
        for (guint member = 0; !matches && job->has_target && member < job->target.count; ++member)
            matches = deck == job->target.decks[member];
        if (matches) job_finish(job, NULL);
    }
}
gboolean radios_event_playout_busy(RadioSViewContext *ctx, guint deck)
{
    EventService *service = service_get(ctx, FALSE); if (!service) return FALSE;
    if (deck < 32) return service->jobs[deck] != NULL;
    if (deck == G_MAXUINT) for (guint i = 0; i < 32; ++i) if (service->jobs[i]) return TRUE;
    return FALSE;
}
