#include "radios/player_scope_execution.h"
#include "radios/automation_scope_ui.h"
#include "radios/automation_calendar_plan.h"
#include "radios/weekly_programme_plan.h"
#include "radios/programme_model.h"
#include "radios/audio_ui.h"
#include "radios/audio.h"
#include "radios/audio_runner.h"
#include "radios/header_navigation.h"
#include "radios/event_playout.h"
#include <string.h>

typedef enum { JOB_LOAD, JOB_PROGRAMME, JOB_SCHEMA, JOB_PREPARED } JobKind;
typedef enum { STEP_BASELINE, STEP_STORED, STEP_SAVING, STEP_SAVE_ACK, STEP_FUTURE, STEP_RUNNING, STEP_RETRY } JobStep;
typedef struct ScopeJob ScopeJob;
struct RadioSPlayerScopeExecution {
    guint refs, timer;
    RadioSViewContext *ctx;
    GtkWidget *request_owner;
    ScopeJob *jobs[32], *loads[32];
    RadioSPlayerScopeExecutionSnapshot states[32];
    gchar override_ids[32][37]; /* temporary schema per source, "" without one */
    gboolean closed;
};
struct ScopeJob {
    guint refs, batch;
    RadioSPlayerScopeExecution *execution;
    RadioSPlayerWorkspace *workspace;
    RadioSDataClient *client;
    RadioSAutomationBinding binding, route_binding;
    gboolean has_route_binding, repeat_set, repeat;
    gint64 stop_after_utc_us;
    RadioSAutoDJMode mode;
    JobKind kind;
    JobStep step;
    gboolean pending, paused, pause_seen, callback_delivered;
    guint64 generations[32], run_epoch;
    gint64 deadline, retry_at, paused_at;
    gint64 soft_end_utc_us; /* a planned last item may end after the block end */
    gchar playlist_id[37];
    JsonNode *programme, *record, *selection, *prepared;
    gchar *route_notice;         /* last reported saved-pool adjustment, reported once per change */
    JsonNode *override_record;   /* the temporary schema, fetched with the main record */
    gboolean override_running;   /* the current occurrence belongs to the temporary schema */
    gboolean override_probe;     /* re-read the temporary schema without stopping the slot */
    gint64 preempt_utc_us;       /* the temporary schema's next start inside the main slot */
    RadioSWeeklyProgrammeOccurrence *occurrence;
    RadioSPlayerScopeExecutionDone done;
    RadioSPlayerScopeExecutionGuard guard;
    gpointer user;
    GDestroyNotify destroy;
};
static void execution_unref(gpointer value);
static void job_finish(ScopeJob *job, const GError *error);
static void job_begin(ScopeJob *job);
static RadioSPlayerScopeExecution *execution_ref(RadioSPlayerScopeExecution *e) { ++e->refs; return e; }
static ScopeJob *job_ref(ScopeJob *job) { ++job->refs; return job; }
static JsonObject *object(JsonObject *o, const gchar *name)
{
    JsonNode *n = o ? json_object_get_member(o, name) : NULL;
    return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
}
static JsonArray *array(JsonObject *o, const gchar *name)
{
    JsonNode *n = o ? json_object_get_member(o, name) : NULL;
    return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL;
}
static JsonNode *copy_object(JsonObject *o)
{
    JsonNode *n = json_node_new(JSON_NODE_OBJECT); json_node_take_object(n, radios_programme_clone_object(o)); return n;
}
static gboolean invalid(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message); return FALSE; }
static ScopeJob **job_slot(ScopeJob *job)
{ return job->kind == JOB_LOAD ? &job->execution->loads[job->binding.source] : &job->execution->jobs[job->binding.source]; }
static void job_unref(gpointer value)
{
    ScopeJob *job = value; if (--job->refs) return;
    if (job->destroy) job->destroy(job->user);
    if (job->client) radios_data_client_unref(job->client);
    g_clear_pointer(&job->programme, json_node_free); g_clear_pointer(&job->record, json_node_free);
    g_clear_pointer(&job->selection, json_node_free); g_clear_pointer(&job->prepared, json_node_free);
    g_clear_pointer(&job->override_record, json_node_free); g_free(job->route_notice);
    radios_weekly_programme_occurrence_free(job->occurrence);
    execution_unref(job->execution); g_free(job);
}
static void deliver(ScopeJob *job, const GError *error)
{
    if (job->callback_delivered) return;
    job->callback_delivered = TRUE;
    RadioSPlayerScopeExecutionDone done = job->done; GDestroyNotify destroy = job->destroy;
    gpointer user = job->user; job->done = NULL; job->destroy = NULL; job->user = NULL; job->guard = NULL;
    if (done && !job->execution->closed) done(job->execution->ctx, &job->binding.scope, job->binding.source, error, user);
    if (destroy) destroy(user);
}
static void set_state(ScopeJob *job, RadioSPlayerScopeExecutionState state, const GError *error)
{
    RadioSPlayerScopeExecutionSnapshot *s = &job->execution->states[job->binding.source];
    s->state = state; s->source = job->binding.source; s->schema_active = job->kind == JOB_SCHEMA;
    s->override_active = job->kind == JOB_SCHEMA && job->override_running;
    s->error[0] = 0; if (error) g_strlcpy(s->error, error->message, sizeof s->error);
    if (job->kind == JOB_SCHEMA && job->record) {
        JsonObject *config = object(json_node_get_object(job->record), "config");
        JsonNode *repeat = config ? json_object_get_member(config, "repeat") : NULL;
        s->repeat = job->repeat_set ? job->repeat : repeat && json_node_get_boolean(repeat);
    }
    if (job->occurrence) {
        s->starts_at_utc_us = job->occurrence->start_utc_us; s->ends_at_utc_us = job->occurrence->end_utc_us;
        g_strlcpy(s->event_name, radios_json_string(json_node_get_object(job->occurrence->event), "title",
            radios_json_string(json_node_get_object(job->occurrence->event), "name", "Otomasyon")), sizeof s->event_name);
    }
}
static void job_finish(ScopeJob *job, const GError *error)
{
    if (*job_slot(job) != job) return;
    *job_slot(job) = NULL;
    if (job->kind != JOB_LOAD || !job->execution->jobs[job->binding.source]) {
        set_state(job, error ? RADIOS_SCOPE_EXECUTION_ERROR :
            (job->kind == JOB_PROGRAMME || job->kind == JOB_PREPARED) && job->prepared ? RADIOS_SCOPE_EXECUTION_PLAYING : RADIOS_SCOPE_EXECUTION_IDLE, error);
        job->execution->states[job->binding.source].schema_active = FALSE;
    }
    if (error && job->callback_delivered && !job->execution->closed && job->execution->ctx->notify)
        job->execution->ctx->notify(job->execution->ctx->notify_data, error->message);
    deliver(job, error); job_unref(job);
}
static void job_fail(ScopeJob *job, const GError *failure, const gchar *fallback)
{
    GError *error = failure ? g_error_copy(failure) : g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, fallback);
    job_finish(job, error); g_error_free(error);
}
static void capture_generations(ScopeJob *job)
{
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(job->execution->ctx); RadioSAudioSnapshot s;
    if (!engine) return;
    radios_audio_engine_snapshot(engine, &s); radios_audio_engine_unref(engine);
    guint count = job->mode == RADIOS_AUTODJ_SEMI ? 1 : job->binding.count;
    for (guint i = 0; i < count; ++i) {
        guint deck = job->binding.decks[i]; job->generations[deck] = s.decks[deck].generation;
    }
}
static gboolean job_current(ScopeJob *job, GError **error)
{
    RadioSPlayerScopeExecution *e = job->execution;
    if (e->closed || *job_slot(job) != job) return FALSE;
    if (!e->ctx || e->ctx->players != job->workspace || !radios_player_workspace_ready(job->workspace))
        return invalid(error, "Oynatıcı çalışma oturumu değişti.");
    RadioSDataClient *current = radios_data_client_acquire(e->ctx);
    gboolean same = current && current == job->client && radios_data_connected(e->ctx);
    if (current) radios_data_client_unref(current);
    if (!same) return invalid(error, "Hazırlık sırasında veri oturumu değişti; mevcut ses korundu.");
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(job->workspace);
    if (job->kind == JOB_LOAD) {
        /* Playlist binding is a stable queue recipient, independent of the
         * source's current AutoDJ target, mode and schema assignment. */
        gboolean recipient = job->binding.scope.kind == RADIOS_AUTOMATION_SCOPE_PLAYER ?
            radios_player_topology_player_const(topology, job->binding.scope.deck) != NULL :
            job->binding.scope.kind == RADIOS_AUTOMATION_SCOPE_CLUSTER ?
            radios_player_topology_cluster_const(topology, job->binding.scope.cluster_id) != NULL :
            radios_player_topology_members(topology, "", NULL, 0) > 0;
        return recipient || invalid(error, "Playlistin yayın sırası alıcısı kaldırıldı.");
    }
    if (!radios_automation_scope_binding_current(topology, &job->binding, error)) return FALSE;
    if (job->has_route_binding && !radios_automation_scope_binding_current(
            radios_player_workspace_topology(job->workspace), &job->route_binding, error)) return FALSE;
    if (job->step == STEP_RUNNING) {
        RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(e->ctx); RadioSAudioRunSnapshot run;
        if (!engine) return invalid(error, "Ses motoru hazır değil.");
        radios_audio_engine_player_runner_snapshot(engine, job->binding.source, &run); radios_audio_engine_unref(engine);
        if (run.epoch != job->run_epoch) return invalid(error, "Programın oynatıcı sahipliği değişti; yeni ses korundu.");
    }
    if (job->kind != JOB_LOAD && job->step != STEP_RUNNING && job->step != STEP_FUTURE && job->step != STEP_RETRY) {
        RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(e->ctx); RadioSAudioSnapshot s;
        if (!engine) return invalid(error, "Ses motoru hazır değil.");
        RadioSAudioRunSnapshot general; radios_audio_engine_runner_snapshot(engine, &general);
        radios_audio_engine_snapshot(engine, &s); radios_audio_engine_unref(engine);
        /* The general queue's runner keeps the players it plays or has cued:
         * a scope flow never takes them over (one programme, one flow). */
        if (general.state != RADIOS_AUDIO_RUN_IDLE && general.state != RADIOS_AUDIO_RUN_FINISHED && general.state != RADIOS_AUDIO_RUN_ERROR) {
            gboolean taken = FALSE;
            for (guint i = 0; i < job->binding.count && !taken; ++i)
                taken = job->binding.decks[i] < RADIOS_AUDIO_NORMAL_DECKS && general.owned[job->binding.decks[i]];
            for (guint i = 0; job->has_route_binding && i < job->route_binding.count && !taken; ++i)
                taken = job->route_binding.decks[i] < RADIOS_AUDIO_NORMAL_DECKS && general.owned[job->route_binding.decks[i]];
            if (taken) return invalid(error, "Bu oynatıcılar genel yayın sırasında çalıyor; önce genel sırayı durdurun.");
        }
        /* A SEMI override never acquires the source definition's ordinary
         * transition peer. Unrelated peer loads must not cancel this single
         * player's preparation; full topology/assignment fences stay above. */
        guint count = job->mode == RADIOS_AUTODJ_SEMI ? 1 : job->binding.count;
        for (guint i = 0; i < count; ++i) {
            guint deck = job->binding.decks[i];
            if (s.decks[deck].generation != job->generations[deck])
                return invalid(error, "Hazırlanırken oynatıcı kaynağı değişti; mevcut ses korundu.");
        }
    }
    if (job->guard && !job->guard(e->ctx, &job->binding.scope, job->binding.source, job->user, error)) {
        if (!error || !*error) return invalid(error, "Event başlatma isteği artık geçerli değil.");
        return FALSE;
    }
    return TRUE;
}
static JsonNode *body(ScopeJob *job)
{
    JsonNode *n = json_node_new(JSON_NODE_OBJECT); json_node_take_object(n, json_object_new());
    json_object_set_string_member(json_node_get_object(n), "station_id", radios_data_client_station_id(job->client)); return n;
}
static gboolean request(ScopeJob *job, const gchar *path, JsonNode *input, RadioSDataCallback callback)
{
    job->pending = TRUE;
    gboolean accepted = radios_data_request(job->execution->ctx, job->execution->request_owner,
        "POST", path, input, callback, job_ref(job), job_unref);
    json_node_free(input); if (!accepted) job->pending = FALSE; return accepted;
}
static gboolean check_received(ScopeJob *job, JsonNode *result, const GError *failure)
{
    if (*job_slot(job) != job || job->execution->closed) return FALSE;
    job->pending = FALSE; GError *error = failure ? g_error_copy(failure) : NULL;
    if (!error) job_current(job, &error);
    if (!error && (!result || !JSON_NODE_HOLDS_OBJECT(result))) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Doğrulama yanıtı geçersiz.");
    if (error) { job_fail(job, error, "Hazırlık başarısız."); g_error_free(error); return FALSE; }
    return TRUE;
}
static JsonNode *playlist_programme(JsonObject *prepared, GError **error)
{
    JsonArray *tracks = array(prepared, "tracks");
    if (!tracks || !json_array_get_length(tracks)) { invalid(error, "Playlistte yüklenebilir parça yok."); return NULL; }
    JsonObject *config = json_object_new(); JsonArray *events = json_array_new();
    json_object_set_string_member(config, "title", radios_json_string(prepared, "source_name", "Yayın sırası"));
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d"); g_date_time_unref(now);
    json_object_set_string_member(config, "date", date); g_free(date); json_object_set_string_member(config, "note", "");
    json_object_set_array_member(config, "events", events);
    /* Adjacent music/jingle runs from a loaded playlist are separate
     * formal events. The playable track order and media bindings stay intact. */
    gchar *block_id = NULL; JsonObject *block = NULL; JsonArray *blocks = json_array_new();
    json_object_set_array_member(config, "blocks", blocks);
    for (guint i = 0; i < json_array_get_length(tracks); ++i) {
        JsonObject *row = json_array_get_object_element(tracks, i), *detail = object(row, "detail");
        JsonObject *media = object(detail, "media"), *version = object(detail, "version"), *metadata = object(row, "metadata");
        const gchar *kind = radios_json_string(media, "kind", "");
        if (!media || !version || !radios_programme_kind_valid(kind)) {
            g_free(block_id); json_object_unref(config); invalid(error, "Playlistin doğrulanmış medya bağlantısı eksik."); return NULL;
        }
        if (!block || g_strcmp0(radios_json_string(block, "kind", ""), kind)) {
            g_free(block_id); block_id = g_uuid_string_random(); block = json_object_new();
            json_object_set_string_member(block, "id", block_id); json_object_set_string_member(block, "kind", kind);
            const gchar *playlist_id = radios_json_string(prepared, "playlist_id", "");
            if (g_uuid_string_is_valid(playlist_id)) json_object_set_string_member(block, "playlist_id", playlist_id);
            json_object_set_string_member(block, "title", radios_json_string(prepared, "source_name", "Yayın sırası"));
            json_object_set_string_member(block, "start_time", ""); json_object_set_string_member(block, "note", "");
            json_array_add_object_element(blocks, block);
        }
        JsonObject *event = json_object_new(); gchar *id = g_uuid_string_random();
        json_object_set_string_member(event, "id", id); g_free(id); json_object_set_string_member(event, "kind", kind);
        json_object_set_string_member(event, "block_id", block_id);
        const gchar *fields[] = {"title", "artist", "album"};
        for (guint j = 0; j < G_N_ELEMENTS(fields); ++j)
            json_object_set_string_member(event, fields[j], radios_json_string(metadata, fields[j], radios_json_string(media, fields[j], "")));
        json_object_set_string_member(event, "start_time", "");
        json_object_set_int_member(event, "duration_ms", radios_json_int(version, "duration_ms", 0));
        const gchar *blank[] = {"version_label", "primary_file", "backup_file", "note"};
        for (guint j = 0; j < G_N_ELEMENTS(blank); ++j) json_object_set_string_member(event, blank[j], "");
        json_object_set_string_member(event, "media_id", radios_json_string(media, "_id", ""));
        json_object_set_string_member(event, "version_id", radios_json_string(version, "_id", ""));
        json_object_set_string_member(event, "media_file_id", radios_json_string(version, "media_file_id", ""));
        json_object_set_string_member(event, "media_sha256", radios_json_string(version, "sha256", ""));
        json_array_add_object_element(events, event);
    }
    g_free(block_id);
    JsonNode *n = json_node_new(JSON_NODE_OBJECT); json_node_take_object(n, config); return n;
}
static void schema_fetch(ScopeJob *job, gboolean final);
static void schema_resolve(ScopeJob *job);
static void override_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value);
static void install(ScopeJob *job)
{
    GError *error = NULL;
    guint decks[32], count = job->mode == RADIOS_AUTODJ_SEMI ? 1 : job->binding.count;
    memcpy(decks, job->binding.decks, count * sizeof *decks);
    JsonObject *prepared = json_node_get_object(job->prepared); gboolean routed = FALSE;
    JsonArray *tracks = array(prepared, "tracks");
    job->soft_end_utc_us = 0;
    for (guint i = 0; tracks && i < json_array_get_length(tracks); ++i) {
        JsonObject *row = json_array_get_object_element(tracks, i);
        JsonObject *constraints = object(row, "calendar_constraints");
        if (constraints) job->soft_end_utc_us = MAX(job->soft_end_utc_us, radios_json_int(constraints, "latest_end_utc_us", 0));
        /* Scope validation already checked the saved distributions. SEMI's
         * explicit runtime override keeps this same queue on its source. */
        if (job->mode == RADIOS_AUTODJ_SEMI) json_object_remove_member(row, "calendar_routing");
        routed |= object(row, "calendar_routing") != NULL;
    }
    if (job->kind != JOB_SCHEMA && !routed && job->mode == RADIOS_AUTODJ_TRANSITIONS &&
        job->binding.scope.kind == RADIOS_AUTOMATION_SCOPE_UNCLUSTERED) count = 2;
    /* An assigned future schema is not the source of an ordinary programme.
     * Only the exact freshly fetched/rechecked schema publishes that origin. */
    JsonNode *schema_record = job->override_running && job->override_record ? job->override_record : job->record;
    const gchar *origin = job->kind == JOB_SCHEMA && schema_record ?
        radios_json_string(json_node_get_object(schema_record), "_id", "") : job->kind == JOB_PREPARED ?
        radios_json_string(prepared, "loudness_automation_document_id", "") : NULL;
    gboolean assigned = job->kind == JOB_SCHEMA;
    if (!job_current(job, &error) || !radios_player_workspace_start_prepared_pool(job->workspace, &job->binding,
            prepared, decks, count, job->mode, origin, assigned, &error)) {
        if (job->kind == JOB_SCHEMA && job->occurrence && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            /* The selection no longer fits after the prepare round trip:
             * select again shortly instead of ending the whole schedule. */
            job->step = STEP_RETRY; job->retry_at = g_get_real_time() + 2 * G_USEC_PER_SEC;
            set_state(job, RADIOS_SCOPE_EXECUTION_WAITING, NULL); if (!job->guard) deliver(job, NULL); g_clear_error(&error); return;
        }
        job_fail(job, error, "Seçili yayın sırası başlatılamadı."); g_clear_error(&error); return;
    }
    set_state(job, RADIOS_SCOPE_EXECUTION_PLAYING, NULL);
    if (job->kind == JOB_SCHEMA) {
        RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(job->execution->ctx); RadioSAudioRunSnapshot run;
        radios_audio_engine_player_runner_snapshot(engine, job->binding.source, &run); radios_audio_engine_unref(engine);
        job->run_epoch = run.epoch; job->step = STEP_RUNNING; ++job->batch; deliver(job, NULL);
    } else { deliver(job, NULL); job_finish(job, NULL); }
}
static void programme_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; ScopeJob *job = value; if (!check_received(job, result, error)) return;
    GError *failure = NULL; JsonNode *prepared = job->selection ?
        radios_automation_calendar_merge_selection(json_node_get_object(result), json_node_get_object(job->selection), &failure) : copy_object(json_node_get_object(result));
    if (!prepared) { job_fail(job, failure, "Medya yeniden doğrulanamadı."); g_clear_error(&failure); return; }
    g_clear_pointer(&job->prepared, json_node_free); job->prepared = prepared;
    if (job->kind == JOB_SCHEMA) schema_fetch(job, TRUE); else install(job);
}
static void prepare_programme(ScopeJob *job)
{
    JsonNode *input = body(job); JsonObject *o = json_node_get_object(input);
    json_object_set_string_member(o, "kind", "programme"); json_object_set_object_member(o, "config", radios_programme_clone_object(json_node_get_object(job->programme)));
    if (!request(job, "/v1/workflows/desktop-playout/prepare", input, programme_received)) job_fail(job, NULL, "Yayın sırası hazırlığı gönderilemedi.");
}
static void automation_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; ScopeJob *job = value; if (!check_received(job, result, error)) return;
    gint64 now = g_get_real_time();
    if (!job->occurrence || now >= job->occurrence->end_utc_us) { job->step = STEP_RETRY; job->retry_at = now; return; }
    JsonNode *sources = copy_object(json_node_get_object(result));
    RadioSViewContext *run_ctx = radios_player_workspace_source_context(job->workspace, job->binding.source);
    JsonNode *turns = run_ctx ? radios_playout_ui_calendar_turns(run_ctx) : NULL;
    if (turns) json_object_set_member(json_node_get_object(sources), "calendar_turns", turns);
    JsonNode *history = run_ctx ? radios_playout_ui_history(run_ctx) : NULL;
    if (history) {
        JsonObject *record = json_object_new(); json_object_set_member(record, "config", history);
        json_object_set_object_member(json_node_get_object(sources), "history", record);
    }
    /* Within its last seconds the block cannot take a selection made for
     * "now + lead": wait for the next occurrence instead of failing. */
    if (MAX(now + 5 * G_USEC_PER_SEC, job->occurrence->start_utc_us) >= job->occurrence->end_utc_us) {
        job->step = STEP_RETRY; job->retry_at = job->occurrence->end_utc_us;
        set_state(job, RADIOS_SCOPE_EXECUTION_WAITING, NULL); if (!job->guard) deliver(job, NULL); return;
    }
    GError *failure = NULL; gchar *seed = g_strdup_printf("%s:%u", job->occurrence->occurrence_key, job->batch);
    g_clear_pointer(&job->selection, json_node_free);
    job->selection = radios_automation_calendar_prepared_programme_at_mode(json_node_get_object(job->occurrence->event),
        json_node_get_object(sources), radios_data_client_station_id(job->client), seed, 200,
        /* Select for the moment playback can really begin: playback starts one
         * prepare round trip later, so the first item still fits its window. */
        MAX(now + 5 * G_USEC_PER_SEC, job->occurrence->start_utc_us), job->mode == RADIOS_AUTODJ_SEMI, &failure);
    g_free(seed); json_node_free(sources);
    if (!job->selection) {
        if (g_error_matches(failure, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            job->step = STEP_RETRY; job->retry_at = MIN(job->occurrence->end_utc_us, (now / (60 * G_USEC_PER_SEC) + 1) * (60 * G_USEC_PER_SEC));
            set_state(job, RADIOS_SCOPE_EXECUTION_WAITING, NULL); if (!job->guard) deliver(job, NULL);
        } else job_fail(job, failure, "Program kurallarına uygun medya yok.");
        g_clear_error(&failure); return;
    }
    g_clear_pointer(&job->programme, json_node_free);
    job->programme = copy_object(object(json_node_get_object(job->selection), "config")); prepare_programme(job);
}
static gboolean schema_bounds(JsonObject *config, gint64 *anchor, gint64 *period)
{
    gchar *iso = g_strconcat(radios_json_string(config, "week_anchor", ""), "T00:00:00Z", NULL);
    GDateTime *date = g_date_time_new_from_iso8601(iso, NULL); g_free(iso);
    gint64 weeks = radios_json_int(config, "week_count", 0);
    if (!date || weeks < 1 || weeks > 52) { if (date) g_date_time_unref(date); return FALSE; }
    *anchor = g_date_time_to_unix(date) * G_USEC_PER_SEC; *period = weeks * 7 * G_TIME_SPAN_DAY;
    g_date_time_unref(date); return TRUE;
}
static void repeat_bounds(ScopeJob *job)
{
    if (!job->record) return;
    gint64 anchor = 0, period = 0;
    if (!schema_bounds(object(json_node_get_object(job->record), "config"), &anchor, &period)) return;
    gint64 reference = job->occurrence && job->step == STEP_RUNNING ? job->occurrence->start_utc_us : g_get_real_time();
    gint64 cycle = reference > anchor ? (reference - anchor) / period : 0;
    job->stop_after_utc_us = anchor + (cycle + 1) * period;
}
static void schema_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; ScopeJob *job = value; if (!check_received(job, result, error)) return;
    JsonObject *record = json_node_get_object(result);
    if (job->prepared) {
        if (g_get_real_time() >= job->occurrence->end_utc_us) { job->step = STEP_RETRY; job->retry_at = g_get_real_time(); return; }
        install(job); return;
    }
    g_clear_pointer(&job->record, json_node_free); job->record = copy_object(record);
    const gchar *override_id = job->execution->override_ids[job->binding.source];
    g_clear_pointer(&job->override_record, json_node_free);
    if (*override_id) {
        /* The temporary schema is read next to the main record every time, so
         * its edits and its removal reach the running flow. */
        job->pending = TRUE;
        if (radios_automation_scope_fetch(job->execution->ctx, job->execution->request_owner, override_id, 0,
                override_received, job_ref(job), job_unref)) return;
        job->pending = FALSE;
    }
    schema_resolve(job);
}
static void override_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; ScopeJob *job = value;
    if (*job_slot(job) != job || job->execution->closed) return;
    job->pending = FALSE;
    g_clear_pointer(&job->override_record, json_node_free);
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        /* A vanished temporary schema must not end the main flow. */
        job->execution->override_ids[job->binding.source][0] = '\0';
        if (job->execution->ctx->notify)
            job->execution->ctx->notify(job->execution->ctx->notify_data, "Geçici şema okunamadı; ana program akışı sürüyor.");
    } else job->override_record = copy_object(json_node_get_object(result));
    if (job->override_probe) {
        /* A temporary schema set while a main slot plays: interrupt that slot
         * only when the temporary schema has a slot right now. */
        job->override_probe = FALSE;
        JsonObject *main_config = job->record ? object(json_node_get_object(job->record), "config") : NULL;
        JsonObject *override_config = job->override_record ? object(json_node_get_object(job->override_record), "config") : NULL;
        gboolean use_override = FALSE; gint64 preempt = 0;
        RadioSWeeklyProgrammeOccurrence *probe = radios_weekly_programme_override_next(main_config, override_config,
            g_get_real_time(), &use_override, &preempt, NULL);
        radios_weekly_programme_occurrence_free(probe);
        if (use_override) job->preempt_utc_us = g_get_real_time(); else job->preempt_utc_us = preempt;
        return;
    }
    schema_resolve(job);
}
static void schema_resolve(ScopeJob *job)
{
    JsonObject *record = json_node_get_object(job->record);
    radios_weekly_programme_occurrence_free(job->occurrence); job->occurrence = NULL;
    GError *failure = NULL;
    JsonNode *runtime_config = copy_object(object(record, "config"));
    if (job->repeat_set) {
        if (!job->stop_after_utc_us) repeat_bounds(job);
        gint64 anchor = 0, period = 0; schema_bounds(json_node_get_object(runtime_config), &anchor, &period);
        json_object_set_boolean_member(json_node_get_object(runtime_config), "repeat", job->repeat || job->stop_after_utc_us > anchor + period);
    }
    JsonObject *override_config = job->override_record ? object(json_node_get_object(job->override_record), "config") : NULL;
    gboolean use_override = FALSE; gint64 preempt = 0;
    job->occurrence = radios_weekly_programme_override_next(json_node_get_object(runtime_config), override_config,
        g_get_real_time(), &use_override, &preempt, &failure);
    job->override_running = use_override; job->preempt_utc_us = preempt;
    json_node_free(runtime_config);
    if (job->occurrence && !use_override && job->repeat_set && !job->repeat && job->occurrence->start_utc_us >= job->stop_after_utc_us) {
        radios_weekly_programme_occurrence_free(job->occurrence); job->occurrence = NULL;
    }
    if (!job->occurrence) { if (failure) job_fail(job, failure, "Program zamanı geçersiz."); else job_finish(job, NULL); g_clear_error(&failure); return; }
    if (!job->occurrence->active) {
        job->step = STEP_FUTURE; set_state(job, RADIOS_SCOPE_EXECUTION_WAITING, NULL); if (!job->guard) deliver(job, NULL); return;
    }
    gchar *notice = NULL;
    JsonNode *event = radios_automation_scope_route_event_notice(job->has_route_binding ? &job->route_binding : &job->binding,
        json_node_get_object(job->occurrence->event), &notice, &failure);
    if (!event) { g_free(notice); job_fail(job, failure, "Program dağılımı kapsam dışında."); g_clear_error(&failure); return; }
    /* Saved pools naming players outside this group keep playing on the
     * group's own players; the operator is told once per differing event. */
    if (notice && g_strcmp0(notice, job->route_notice) && job->execution->ctx->notify)
        job->execution->ctx->notify(job->execution->ctx->notify_data, notice);
    g_free(job->route_notice); job->route_notice = notice;
    json_node_free(job->occurrence->event); job->occurrence->event = event;
    JsonNode *config = radios_automation_calendar_prepare_config(json_node_get_object(event), NULL, &failure);
    if (!config) { job_fail(job, failure, "Program rotasyonları geçersiz."); g_clear_error(&failure); return; }
    JsonNode *input = body(job); json_object_set_string_member(json_node_get_object(input), "kind", "automation");
    json_object_set_member(json_node_get_object(input), "config", config);
    if (!request(job, "/v1/workflows/desktop-playout/prepare", input, automation_received)) job_fail(job, NULL, "Program kaynakları hazırlanamadı.");
}
static void schema_fetch(ScopeJob *job, gboolean final)
{
    job->pending = TRUE;
    /* The final re-check pins the record whose slot actually plays. */
    gboolean override = final && job->override_running && job->override_record;
    JsonNode *pinned = override ? job->override_record : job->record;
    gint64 revision = final && pinned ? radios_json_int(json_node_get_object(pinned), "_rev", 0) : 0;
    const gchar *document = override ? radios_json_string(json_node_get_object(job->override_record), "_id", "") : job->binding.automation_id;
    gboolean accepted = radios_automation_scope_fetch(job->execution->ctx, job->execution->request_owner,
        document, revision, schema_received, job_ref(job), job_unref);
    if (!accepted) { job->pending = FALSE; job_fail(job, NULL, "Atanan otomasyon yeniden doğrulanamadı."); }
}
static void playlist_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; ScopeJob *job = value; if (!check_received(job, result, error)) return;
    GError *failure = NULL; job->programme = playlist_programme(json_node_get_object(result), &failure);
    if (!job->programme) { job_fail(job, failure, "Playlist yüklenemedi."); g_clear_error(&failure); return; }
    job->step = STEP_SAVING;
}
static void job_begin(ScopeJob *job)
{
    capture_generations(job); job->deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
    job->step = STEP_STORED; set_state(job, RADIOS_SCOPE_EXECUTION_PREPARING, NULL);
    if (job->kind != JOB_PREPARED) g_clear_pointer(&job->prepared, json_node_free);
    g_clear_pointer(&job->selection, json_node_free);
    if (job->kind == JOB_PREPARED) install(job);
    else if (job->kind == JOB_LOAD) {
        JsonNode *input = body(job); json_object_set_string_member(json_node_get_object(input), "kind", "playlist");
        json_object_set_string_member(json_node_get_object(input), "playlist_id", job->playlist_id);
        if (!request(job, "/v1/workflows/desktop-playout/prepare", input, playlist_received)) job_fail(job, NULL, "Playlist kaynağı hazırlanamadı.");
    } else if (job->kind == JOB_SCHEMA) schema_fetch(job, FALSE);
    else if (job->programme) prepare_programme(job);
}
static gboolean execution_tick(gpointer value)
{
    RadioSPlayerScopeExecution *e = value; if (e->closed) return G_SOURCE_REMOVE;
    execution_ref(e);
    for (guint source = 0; source < 32 && !e->closed; ++source) for (guint lane = 0; lane < 2 && !e->closed; ++lane) {
        ScopeJob *job = lane ? e->loads[source] : e->jobs[source]; if (!job) continue;
        job_ref(job); GError *error = NULL;
        if (!job_current(job, &error)) { job_fail(job, error, "Hazırlık artık geçerli değil."); g_clear_error(&error); job_unref(job); continue; }
        if (job->step != STEP_FUTURE && job->step != STEP_RUNNING && job->step != STEP_RETRY && g_get_monotonic_time() > job->deadline) {
            GError *timeout = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                "Yayın sırası hazırlığı zaman aşımına uğradı; yeniden deneyebilirsiniz.");
            job_fail(job, timeout, timeout->message); g_error_free(timeout); job_unref(job); continue;
        }
        /* The engine is the truth for a running schema: a resume the runner
         * applied after refusing the command, or Play on the player card,
         * would leave the schema paused here, showing "Duraklatıldı" while it
         * plays and never scheduling its next batch. Once the engine has
         * shown the pause, leaving it resumes the schema. A pause the ticks
         * never saw (resumed within one tick) or that never reached the engine
         * gives way once the engine is not paused after a grace period, even
         * if the short item already finished meanwhile. */
        if (job->paused && job->step == STEP_RUNNING) {
            RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(e->ctx); RadioSAudioRunSnapshot run;
            radios_audio_engine_player_runner_snapshot(engine, source, &run); radios_audio_engine_unref(engine);
            if (run.epoch == job->run_epoch && run.state == RADIOS_AUDIO_RUN_PAUSED) job->pause_seen = TRUE;
            else if (run.epoch == job->run_epoch && (job->pause_seen ||
                    g_get_monotonic_time() - job->paused_at > 1500 * G_TIME_SPAN_MILLISECOND)) {
                job->paused = FALSE; set_state(job, RADIOS_SCOPE_EXECUTION_PLAYING, NULL);
            }
        }
        if (job->pending || job->paused) { job_unref(job); continue; }
        if (job->step == STEP_BASELINE) {
            if (job->kind == JOB_LOAD || radios_player_workspace_prepare_scope(job->workspace, source, &error)) job_begin(job);
            else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PENDING)) job_fail(job, error, "Kapsam hazır değil.");
        } else if (job->step == STEP_STORED && !job->programme && job->kind == JOB_PROGRAMME) {
            JsonObject *programme = NULL;
            if (radios_player_workspace_get_programme(job->workspace, &job->binding.scope, &programme, &error)) {
                job->programme = copy_object(programme); json_object_unref(programme); prepare_programme(job);
            } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PENDING)) job_fail(job, error, "Yayın sırası yüklenemedi.");
        } else if (job->step == STEP_SAVING) {
            if (radios_player_workspace_set_programme(job->workspace, &job->binding.scope, json_node_get_object(job->programme), &error)) {
                job->step = STEP_SAVE_ACK;
            } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PENDING)) job_fail(job, error, "Playlist sırası kaydedilemedi.");
        } else if (job->step == STEP_SAVE_ACK) {
            if (radios_player_workspace_programme_saved(job->workspace, &job->binding.scope, &error)) {
                deliver(job, NULL); job_finish(job, NULL);
            } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PENDING)) job_fail(job, error, "Playlist sırası doğrulanamadı.");
        } else if (job->step == STEP_FUTURE && job->occurrence && g_get_real_time() >= job->occurrence->start_utc_us) job_begin(job);
        else if (job->step == STEP_RETRY && g_get_real_time() >= job->retry_at) job_begin(job);
        else if (job->step == STEP_RUNNING) {
            RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(e->ctx); RadioSAudioRunSnapshot run;
            radios_audio_engine_player_runner_snapshot(engine, source, &run); radios_audio_engine_unref(engine);
            if (run.epoch != job->run_epoch) job_fail(job, NULL, "Programın oynatıcı sahipliği değişti; yeni ses korundu.");
            else if (run.state == RADIOS_AUDIO_RUN_ERROR) job_fail(job, NULL, run.error);
            else if (job->kind == JOB_SCHEMA && job->override_probe) {
                const gchar *override_id = e->override_ids[source]; job->pending = TRUE;
                if (!*override_id || !radios_automation_scope_fetch(e->ctx, e->request_owner, override_id, 0,
                        override_received, job_ref(job), job_unref)) { job->pending = FALSE; job->override_probe = FALSE; }
            }
            /* The temporary schema's slot begins inside the main slot (or it
             * was removed while its slot played): switch now. */
            else if (job->kind == JOB_SCHEMA && job->preempt_utc_us && g_get_real_time() >= job->preempt_utc_us) {
                job->preempt_utc_us = 0;
                radios_player_workspace_action(job->workspace, source, RADIOS_PLAYOUT_STOP, NULL); job_begin(job);
            }
            /* 30 s grace: a live start lags its selection by the prepare
             * round trip (the planner accepts that much on the first item). */
            else if (job->occurrence && g_get_real_time() >= MAX(job->occurrence->end_utc_us, job->soft_end_utc_us) + 30 * G_USEC_PER_SEC) {
                radios_player_workspace_action(job->workspace, source, RADIOS_PLAYOUT_STOP, NULL); job_begin(job);
            } else if (run.state == RADIOS_AUDIO_RUN_FINISHED) {
                gboolean owned = FALSE; for (guint i = 0; i < 32; ++i) owned |= run.owned[i];
                if (!owned) job_begin(job);
            }
        }
        g_clear_error(&error); job_unref(job);
    }
    gboolean keep = !e->closed; execution_unref(e);
    return keep ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
static void window_destroyed(GtkWidget *widget, gpointer value)
{ (void)widget; radios_player_scope_execution_close(value); }
void radios_player_scope_execution_close(RadioSPlayerScopeExecution *e)
{
    if (!e || e->closed) return;
    e->closed = TRUE;
    if (e->timer) { g_source_remove(e->timer); e->timer = 0; }
    for (guint source = 0; source < 32; ++source) {
        if (e->jobs[source]) job_finish(e->jobs[source], NULL);
        if (e->loads[source]) job_finish(e->loads[source], NULL);
    }
    if (e->request_owner) { gtk_widget_destroy(e->request_owner); g_object_unref(e->request_owner); e->request_owner = NULL; }
    e->ctx = NULL;
}
static void execution_unref(gpointer value)
{
    RadioSPlayerScopeExecution *e = value; if (--e->refs) return;
    radios_player_scope_execution_close(e); g_free(e);
}
RadioSPlayerScopeExecution *radios_player_scope_execution_get(RadioSViewContext *ctx)
{
    if (!ctx || !ctx->parent || !ctx->players) return NULL;
    RadioSPlayerScopeExecution *e = g_object_get_data(G_OBJECT(ctx->parent), "radios-player-scope-execution");
    if (e) return e->closed ? NULL : e;
    e = g_new0(RadioSPlayerScopeExecution, 1); e->refs = 1; e->ctx = ctx;
    e->request_owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(e->request_owner);
    for (guint i = 0; i < 32; ++i) e->states[i].source = i;
    e->timer = g_timeout_add(100, execution_tick, e);
    g_signal_connect(ctx->parent, "destroy", G_CALLBACK(window_destroyed), e);
    g_object_set_data_full(G_OBJECT(ctx->parent), "radios-player-scope-execution", e, execution_unref); return e;
}
static ScopeJob *job_new(RadioSPlayerScopeExecution *e, const RadioSAutomationScope *scope,
    JobKind kind, RadioSAutoDJMode mode, JsonObject *prepared,
    RadioSPlayerScopeExecutionDone done, gpointer user, GDestroyNotify destroy, GError **error)
{
    RadioSAutomationBinding binding;
    if (!e || e->closed || !scope || !radios_player_workspace_ready(e->ctx->players) || !radios_data_connected(e->ctx)) {
        invalid(error, "Oynatıcı tanımları ve veri bağlantısı hazır olmalı."); return NULL;
    }
    if (!radios_automation_scope_resolve(radios_player_workspace_topology(e->ctx->players), scope, &binding, error)) return NULL;
    if (!binding.count || binding.source >= 32) { invalid(error, "Seçili kapsamda oynatıcı yok."); return NULL; }
    if (kind == JOB_SCHEMA && !binding.automation_id[0]) { invalid(error, "Bu kapsama atanmış otomasyon şeması yok."); return NULL; }
    if (mode == RADIOS_AUTODJ_OFF) {
        /* A player follows its own definition; a cluster plays together. */
        mode = binding.autodj_mode;
        if (mode == RADIOS_AUTODJ_OFF) mode = RADIOS_AUTODJ_SEMI;
    }
    gboolean prepared_routed = FALSE;
    JsonArray *prepared_tracks = array(prepared, "tracks");
    for (guint i = 0; prepared_tracks && i < json_array_get_length(prepared_tracks); ++i) {
        JsonNode *row = json_array_get_element(prepared_tracks, i);
        prepared_routed |= JSON_NODE_HOLDS_OBJECT(row) && json_object_has_member(json_node_get_object(row), "calendar_routing");
    }
    if (kind == JOB_PREPARED && prepared_routed && mode == RADIOS_AUTODJ_TRANSITIONS &&
        scope->kind == RADIOS_AUTOMATION_SCOPE_PLAYER) {
        guint decks[32], count = 0; gboolean routed = FALSE;
        guint target = binding.autodj_target >= 0 ? (guint)binding.autodj_target : G_MAXUINT;
        if (!radios_playout_prepared_rotation_pool(e->ctx, prepared, binding.source, target,
                decks, &count, &routed, error)) return NULL;
        if (routed && !radios_automation_scope_resolve_routed(radios_player_workspace_topology(e->ctx->players),
                scope, decks, count, &binding, error)) return NULL;
    }
    if (kind != JOB_LOAD && (mode < RADIOS_AUTODJ_SEMI || mode > RADIOS_AUTODJ_TRANSITIONS ||
        (mode == RADIOS_AUTODJ_TRANSITIONS && binding.count < 2))) { invalid(error, "AutoDJ geçişi için aynı kapsamda ikinci oynatıcı gerekli."); return NULL; }
    RadioSAutomationBinding route_binding = {0}; gboolean has_route_binding = FALSE;
    const RadioSPlayerDefinition *source_definition = &radios_player_workspace_topology(e->ctx->players)->players[binding.source];
    if (kind == JOB_SCHEMA && mode == RADIOS_AUTODJ_SEMI && scope->kind == RADIOS_AUTOMATION_SCOPE_PLAYER && source_definition->cluster_id[0]) {
        RadioSAutomationScope group = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER};
        g_strlcpy(group.cluster_id, source_definition->cluster_id, sizeof group.cluster_id);
        if (!radios_automation_scope_resolve(radios_player_workspace_topology(e->ctx->players), &group, &route_binding, error)) return NULL;
        has_route_binding = TRUE;
    }
    ScopeJob *old = kind == JOB_LOAD ? e->loads[binding.source] : e->jobs[binding.source];
    if (old && old->step == STEP_RUNNING) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Çalışan otomasyonun yerine yenisini yüklemeden önce Durdur'a basın.");
        return NULL;
    }
    execution_ref(e);
    if (old) { GError *cancel = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Yeni seçim önceki bekleyen hazırlığın yerini aldı."); job_finish(old, cancel); g_error_free(cancel); }
    if (e->closed) { invalid(error, "Oynatıcı çalışma oturumu kapandı."); execution_unref(e); return NULL; }
    /* The cancelled operation's callback may synchronously create its own
     * replacement. Preserve that newly accepted job instead of orphaning it. */
    if (kind == JOB_LOAD ? e->loads[binding.source] != NULL : e->jobs[binding.source] != NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "İptal bildirimi sırasında yeni bir hazırlık başlatıldı.");
        execution_unref(e); return NULL;
    }
    ScopeJob *job = g_new0(ScopeJob, 1); job->refs = 1; job->execution = execution_ref(e); job->workspace = e->ctx->players;
    job->client = radios_data_client_acquire(e->ctx); job->binding = binding; job->kind = kind; job->mode = mode;
    job->route_binding = route_binding; job->has_route_binding = has_route_binding;
    job->done = done; job->user = user; job->destroy = destroy; job->deadline = g_get_monotonic_time() + 30 * G_USEC_PER_SEC;
    *job_slot(job) = job;
    if (kind != JOB_LOAD) {
        e->states[binding.source] = (RadioSPlayerScopeExecutionSnapshot){.source = binding.source};
        set_state(job, RADIOS_SCOPE_EXECUTION_PREPARING, NULL);
    }
    capture_generations(job); execution_unref(e); return job;
}
gboolean radios_player_scope_execution_load_playlist(RadioSPlayerScopeExecution *e,
    const RadioSAutomationScope *scope, const gchar *playlist_id, RadioSPlayerScopeExecutionDone done,
    gpointer user, GDestroyNotify destroy, GError **error)
{
    if (!playlist_id || !g_uuid_string_is_valid(playlist_id)) { if (destroy) destroy(user); return invalid(error, "Playlist kimliği geçersiz."); }
    ScopeJob *job = job_new(e, scope, JOB_LOAD, RADIOS_AUTODJ_SEMI, NULL, done, user, destroy, error);
    if (!job) { if (destroy) destroy(user); return FALSE; }
    g_strlcpy(job->playlist_id, playlist_id, sizeof job->playlist_id); return TRUE;
}
gboolean radios_player_scope_execution_play_programme_guarded(RadioSPlayerScopeExecution *e,
    const RadioSAutomationScope *scope, JsonObject *programme, RadioSAutoDJMode mode,
    RadioSPlayerScopeExecutionGuard guard, RadioSPlayerScopeExecutionDone done,
    gpointer user, GDestroyNotify destroy, GError **error)
{
    ScopeJob *job = job_new(e, scope, JOB_PROGRAMME, mode, NULL, done, user, destroy, error);
    if (!job) { if (destroy) destroy(user); return FALSE; }
    job->guard = guard;
    if (programme) job->programme = copy_object(programme);
    return TRUE;
}
gboolean radios_player_scope_execution_play_programme(RadioSPlayerScopeExecution *e,
    const RadioSAutomationScope *scope, JsonObject *programme, RadioSAutoDJMode mode,
    RadioSPlayerScopeExecutionDone done, gpointer user, GDestroyNotify destroy, GError **error)
{
    return radios_player_scope_execution_play_programme_guarded(e, scope, programme, mode,
        NULL, done, user, destroy, error);
}
gboolean radios_player_scope_execution_play_prepared(RadioSPlayerScopeExecution *e,
    const RadioSAutomationScope *scope, JsonObject *prepared, RadioSAutoDJMode mode,
    RadioSPlayerScopeExecutionDone done, gpointer user, GDestroyNotify destroy, GError **error)
{
    if (!e || e->closed || !prepared || !g_str_equal(radios_json_string(prepared, "kind", ""), "programme") ||
        !g_str_equal(radios_json_string(prepared, "station_id", ""), radios_data_station_id(e->ctx))) {
        if (destroy) destroy(user);
        return invalid(error, "Doğrulanmış yayın sırası oturuma ait değil.");
    }
    ScopeJob *job = job_new(e, scope, JOB_PREPARED, mode, prepared, done, user, destroy, error);
    if (!job) { if (destroy) destroy(user); return FALSE; }
    job->prepared = copy_object(prepared); return TRUE;
}
gboolean radios_player_scope_execution_play_schema_guarded(RadioSPlayerScopeExecution *e,
    const RadioSAutomationScope *scope, RadioSAutoDJMode mode,
    RadioSPlayerScopeExecutionGuard guard, RadioSPlayerScopeExecutionDone done,
    gpointer user, GDestroyNotify destroy, GError **error)
{
    ScopeJob *job = job_new(e, scope, JOB_SCHEMA, mode, NULL, done, user, destroy, error);
    if (!job) { if (destroy) destroy(user); return FALSE; }
    job->guard = guard; return TRUE;
}
gboolean radios_player_scope_execution_play_schema(RadioSPlayerScopeExecution *e,
    const RadioSAutomationScope *scope, RadioSAutoDJMode mode, RadioSPlayerScopeExecutionDone done,
    gpointer user, GDestroyNotify destroy, GError **error)
{
    return radios_player_scope_execution_play_schema_guarded(e, scope, mode, NULL,
        done, user, destroy, error);
}
static guint scope_sources(RadioSPlayerScopeExecution *e, const RadioSAutomationScope *scope, gboolean sources[32], GError **error)
{
    memset(sources, 0, 32 * sizeof *sources); RadioSAutomationBinding binding;
    if (!e || e->closed || !radios_automation_scope_resolve(radios_player_workspace_topology(e->ctx->players), scope, &binding, error)) return 0;
    JsonNode *projection = radios_player_workspace_projection(e->ctx->players, scope, error); if (!projection) return 0;
    JsonArray *players = array(json_node_get_object(projection), "players");
    for (guint i = 0; players && i < json_array_get_length(players); ++i) {
        gint64 source = radios_json_int(json_array_get_object_element(players, i), "runtime_source", 0) - 1;
        if (source >= 0 && source < 32) sources[source] = TRUE;
    }
    json_node_free(projection);
    /* A player's transition target is an execution pool, not ownership of
     * that peer's independent scheduled operation. Installed leases above
     * resolve actual shared owners; pending jobs follow their saved scope. */
    for (guint source = 0; source < 32; ++source) for (guint lane = 0; lane < 2; ++lane) {
        ScopeJob *job = lane ? e->loads[source] : e->jobs[source]; if (!job) continue;
        gboolean selected = FALSE;
        if (scope->kind == RADIOS_AUTOMATION_SCOPE_PLAYER) {
            if (job->binding.scope.kind == RADIOS_AUTOMATION_SCOPE_PLAYER) selected = source == scope->deck;
            else for (guint i = 0; i < job->binding.count; ++i) selected |= job->binding.decks[i] == scope->deck;
        } else for (guint i = 0; i < binding.count; ++i) selected |= binding.decks[i] == source;
        if (selected) sources[source] = TRUE;
    }
    guint count = 0; for (guint i = 0; i < 32; ++i) count += sources[i]; return count;
}
gboolean radios_player_scope_execution_action(RadioSPlayerScopeExecution *e,
    const RadioSAutomationScope *scope, RadioSPlayoutAction action, GError **error)
{
    /* Local event materialization precedes ordinary scope-job admission. Stop
     * must revoke that intent even when no runner or pending scope exists. */
    if (action == RADIOS_PLAYOUT_STOP && e && !e->closed && scope) {
        if (scope->kind == RADIOS_AUTOMATION_SCOPE_PLAYER) radios_event_playout_cancel(e->ctx, scope->deck);
        else {
            RadioSAutomationBinding binding;
            if (radios_automation_scope_resolve(radios_player_workspace_topology(e->ctx->players), scope, &binding, NULL))
                for (guint i = 0; i < binding.count; ++i) radios_event_playout_cancel(e->ctx, binding.decks[i]);
        }
    }
    gboolean sources[32]; guint count = scope_sources(e, scope, sources, error);
    if (!count) {
        if (error && *error) return FALSE;
        return action == RADIOS_PLAYOUT_STOP && e && !e->closed ? TRUE : invalid(error, "Seçili kapsamda çalışan yayın sırası yok.");
    }
    execution_ref(e); gboolean accepted = TRUE;
    for (guint source = 0; source < 32 && !e->closed; ++source) if (sources[source]) {
        ScopeJob *job = e->jobs[source];
        if (action == RADIOS_PLAYOUT_STOP) {
            GError *cancel = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Bekleyen hazırlık durduruldu.");
            for (guint lane = 0; lane < 2 && !e->closed; ++lane) {
                ScopeJob *pending = lane ? e->loads[source] : e->jobs[source];
                if (pending) { job_ref(pending); deliver(pending, cancel); job_finish(pending, NULL); job_unref(pending); }
            }
            g_error_free(cancel); if (e->closed) break;
        } else if (job && job->kind == JOB_SCHEMA && job->step != STEP_RUNNING) {
            if (action != RADIOS_PLAYOUT_PAUSE && action != RADIOS_PLAYOUT_RESUME) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Bekleyen şemada gezilecek etkin bir sıra yok."); accepted = FALSE; break;
            }
            job->paused = action == RADIOS_PLAYOUT_PAUSE; job->paused_at = g_get_monotonic_time(); job->pause_seen = FALSE;
            set_state(job, job->paused ? RADIOS_SCOPE_EXECUTION_PAUSED : RADIOS_SCOPE_EXECUTION_WAITING, NULL); continue;
        }
        RadioSPlayoutSnapshot run = {0}; gboolean owns = radios_player_workspace_snapshot(e->ctx->players, source, &run) &&
            run.state != RADIOS_PLAYOUT_IDLE && run.state != RADIOS_PLAYOUT_FINISHED;
        if (owns && !radios_player_workspace_action(e->ctx->players, source, action, error)) { accepted = FALSE; break; }
        if (job && e->jobs[source] == job && job->kind == JOB_SCHEMA &&
            (action == RADIOS_PLAYOUT_PAUSE || action == RADIOS_PLAYOUT_RESUME)) {
            job->paused = action == RADIOS_PLAYOUT_PAUSE; job->paused_at = g_get_monotonic_time(); job->pause_seen = FALSE;
            set_state(job, job->paused ? RADIOS_SCOPE_EXECUTION_PAUSED : RADIOS_SCOPE_EXECUTION_PLAYING, NULL);
        }
        if (!e->jobs[source]) {
            e->states[source].schema_active = FALSE;
            if (action == RADIOS_PLAYOUT_STOP) e->states[source].state = RADIOS_SCOPE_EXECUTION_IDLE;
            else if (owns && action == RADIOS_PLAYOUT_PAUSE) e->states[source].state = RADIOS_SCOPE_EXECUTION_PAUSED;
            else if (owns && action == RADIOS_PLAYOUT_RESUME) e->states[source].state = RADIOS_SCOPE_EXECUTION_PLAYING;
        }
    }
    execution_unref(e); return accepted;
}
gboolean radios_player_scope_execution_set_repeat(RadioSPlayerScopeExecution *e,
    const RadioSAutomationScope *scope, gboolean repeat, GError **error)
{
    gboolean sources[32]; guint count = scope_sources(e, scope, sources, error); gboolean found = FALSE;
    if (!count) { if (error && *error) return FALSE; return invalid(error, "Seçili kapsamda etkin haftalık şema yok."); }
    for (guint source = 0; source < 32; ++source) if (sources[source] && e->jobs[source] && e->jobs[source]->kind == JOB_SCHEMA) {
        ScopeJob *job = e->jobs[source]; job->repeat_set = TRUE; job->repeat = repeat; repeat_bounds(job);
        e->states[source].repeat = repeat; found = TRUE;
        if (job->step == STEP_FUTURE) { job->step = STEP_RETRY; job->retry_at = g_get_real_time(); }
    }
    return found || invalid(error, "Seçili kapsamda etkin haftalık şema yok.");
}
gboolean radios_player_scope_execution_set_schema_override(RadioSPlayerScopeExecution *e,
    const RadioSAutomationScope *scope, const gchar *record_id, GError **error)
{
    gboolean sources[32]; guint count = scope_sources(e, scope, sources, error);
    if (!count) { if (error && *error) return FALSE; return invalid(error, "Seçili kapsamda oynatıcı yok."); }
    if (record_id && strlen(record_id) != 36) return invalid(error, "Geçici şema kimliği geçersiz.");
    for (guint source = 0; source < 32; ++source) if (sources[source]) {
        g_strlcpy(e->override_ids[source], record_id ? record_id : "", sizeof e->override_ids[source]);
        ScopeJob *job = e->jobs[source];
        if (!job || job->kind != JOB_SCHEMA) continue;
        if (job->step == STEP_RUNNING) {
            /* Removing the playing temporary slot hands back now; a new one is
             * probed first so a playing main slot is not cut needlessly. */
            if (!record_id) { job->preempt_utc_us = job->override_running ? g_get_real_time() : 0; job->override_probe = FALSE; }
            else job->override_probe = TRUE;
        } else if (job->step == STEP_FUTURE || job->step == STEP_RETRY) { job->step = STEP_RETRY; job->retry_at = g_get_real_time(); }
    }
    return TRUE;
}
const gchar *radios_player_scope_execution_schema_override(RadioSPlayerScopeExecution *e,
    const RadioSAutomationScope *scope)
{
    gboolean sources[32]; if (!scope_sources(e, scope, sources, NULL)) return NULL;
    for (guint source = 0; source < 32; ++source) if (sources[source] && e->override_ids[source][0]) return e->override_ids[source];
    return NULL;
}
gboolean radios_player_scope_execution_validate_topology(RadioSPlayerScopeExecution *e,
    RadioSPlayerWorkspace *workspace, const RadioSPlayerTopology *proposed, GError **error)
{
    if (!e || e->closed || !e->ctx || e->ctx->players != workspace) return TRUE;
    for (guint source = 0; source < 32; ++source) {
        ScopeJob *job = e->jobs[source]; if (!job) continue;
        if (!radios_automation_scope_binding_current(proposed, &job->binding, NULL) ||
            (job->has_route_binding && !radios_automation_scope_binding_current(proposed, &job->route_binding, NULL))) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
                "Çalışan veya zamanı bekleyen otomasyonun kapsamını değiştirmeden önce Durdur'a basın.");
            return FALSE;
        }
    }
    return TRUE;
}
gboolean radios_player_scope_execution_busy(RadioSPlayerScopeExecution *e, const RadioSAutomationScope *scope)
{
    gboolean sources[32]; if (!scope_sources(e, scope, sources, NULL)) return FALSE;
    for (guint i = 0; i < 32; ++i) if (sources[i] && (e->loads[i] || (e->jobs[i] && e->jobs[i]->step != STEP_RUNNING))) return TRUE;
    return FALSE;
}
static gboolean workspace_run_snapshot(RadioSPlayerScopeExecution *e, guint source, RadioSPlayoutSnapshot *run)
{
    gboolean valid = radios_player_workspace_snapshot(e->ctx->players, source, run);
    RadioSViewContext *ctx = radios_player_workspace_source_context(e->ctx->players, source);
    RadioSAudioRunSnapshot actual;
    /* The public navigation snapshot reads the engine through the same
     * scoped epoch fence. Transport feedback and EOF must not depend on the
     * controller's next 100 ms widget/history status poll. */
    if (ctx && radios_playout_ui_navigation_snapshot(ctx, &actual)) {
        const RadioSPlayoutState states[] = {RADIOS_PLAYOUT_IDLE, RADIOS_PLAYOUT_WAITING,
            RADIOS_PLAYOUT_SCHEDULED, RADIOS_PLAYOUT_PLAYING, RADIOS_PLAYOUT_OVERLAP,
            RADIOS_PLAYOUT_PAUSED, RADIOS_PLAYOUT_WAITING, RADIOS_PLAYOUT_MANUAL_WAIT,
            RADIOS_PLAYOUT_FINISHED, RADIOS_PLAYOUT_ERROR};
        if ((guint)actual.state < G_N_ELEMENTS(states)) run->state = states[actual.state];
        g_strlcpy(run->error, actual.error, sizeof run->error); valid = TRUE;
    }
    return valid;
}
static RadioSPlayerScopeExecutionState run_state(RadioSPlayoutState state)
{
    switch (state) {
    case RADIOS_PLAYOUT_PREPARING: return RADIOS_SCOPE_EXECUTION_PREPARING;
    case RADIOS_PLAYOUT_PLAYING: case RADIOS_PLAYOUT_OVERLAP: return RADIOS_SCOPE_EXECUTION_PLAYING;
    case RADIOS_PLAYOUT_PAUSED: return RADIOS_SCOPE_EXECUTION_PAUSED;
    case RADIOS_PLAYOUT_SCHEDULED: case RADIOS_PLAYOUT_WAITING: case RADIOS_PLAYOUT_MANUAL_WAIT: return RADIOS_SCOPE_EXECUTION_WAITING;
    case RADIOS_PLAYOUT_ERROR: return RADIOS_SCOPE_EXECUTION_ERROR;
    case RADIOS_PLAYOUT_IDLE: case RADIOS_PLAYOUT_FINISHED: return RADIOS_SCOPE_EXECUTION_IDLE;
    }
    return RADIOS_SCOPE_EXECUTION_IDLE;
}
static void source_snapshot(RadioSPlayerScopeExecution *e, guint source, RadioSPlayerScopeExecutionSnapshot *snapshot)
{
    *snapshot = e->states[source]; snapshot->source = source;
    ScopeJob *job = e->jobs[source]; RadioSPlayoutSnapshot run = {0};
    gboolean has_run = workspace_run_snapshot(e, source, &run);
    snapshot->schema_active = job && job->kind == JOB_SCHEMA;
    snapshot->override_active = snapshot->schema_active && job->override_running;
    snapshot->loading_playlist = e->loads[source] != NULL;
    snapshot->preparing = job && job->step != STEP_RUNNING && job->step != STEP_FUTURE && job->step != STEP_RETRY;
    if (job) {
        if (job->paused) snapshot->state = RADIOS_SCOPE_EXECUTION_PAUSED;
        else if (job->step == STEP_FUTURE || job->step == STEP_RETRY) snapshot->state = RADIOS_SCOPE_EXECUTION_WAITING;
        else if (job->step == STEP_RUNNING) {
            snapshot->state = has_run ? run_state(run.state) : RADIOS_SCOPE_EXECUTION_WAITING;
            if (snapshot->state == RADIOS_SCOPE_EXECUTION_IDLE) snapshot->state = RADIOS_SCOPE_EXECUTION_WAITING;
        } else snapshot->state = RADIOS_SCOPE_EXECUTION_PREPARING;
        if (snapshot->schema_active) {
            if (job->occurrence) {
                snapshot->starts_at_utc_us = job->occurrence->start_utc_us; snapshot->ends_at_utc_us = job->occurrence->end_utc_us;
                g_strlcpy(snapshot->event_name, radios_json_string(json_node_get_object(job->occurrence->event), "title",
                    radios_json_string(json_node_get_object(job->occurrence->event), "name", "Otomasyon")), sizeof snapshot->event_name);
            }
            JsonObject *config = job->record ? object(json_node_get_object(job->record), "config") : NULL;
            JsonNode *repeat = config ? json_object_get_member(config, "repeat") : NULL;
            snapshot->repeat = job->repeat_set ? job->repeat : repeat && json_node_get_boolean(repeat);
        }
    } else {
        RadioSPlayerScopeExecutionState actual = has_run ? run_state(run.state) : RADIOS_SCOPE_EXECUTION_IDLE;
        if (actual != RADIOS_SCOPE_EXECUTION_IDLE || snapshot->state != RADIOS_SCOPE_EXECUTION_ERROR) snapshot->state = actual;
        if (actual == RADIOS_SCOPE_EXECUTION_IDLE && e->loads[source]) snapshot->state = RADIOS_SCOPE_EXECUTION_PREPARING;
        snapshot->repeat = FALSE; snapshot->starts_at_utc_us = 0; snapshot->ends_at_utc_us = 0; snapshot->event_name[0] = 0;
        if (actual == RADIOS_SCOPE_EXECUTION_ERROR) g_strlcpy(snapshot->error, run.error, sizeof snapshot->error);
    }
}
gboolean radios_player_scope_execution_snapshot(RadioSPlayerScopeExecution *e,
    const RadioSAutomationScope *scope, RadioSPlayerScopeExecutionSnapshot *snapshot)
{
    if (!e || e->closed || !snapshot) return FALSE;
    gboolean sources[32];
    scope_sources(e, scope, sources, NULL);
    for (guint i = 0; i < 32; ++i) if (sources[i]) { source_snapshot(e, i, snapshot); return TRUE; }
    RadioSAutomationBinding binding;
    if (!radios_automation_scope_resolve(radios_player_workspace_topology(e->ctx->players), scope, &binding, NULL) || binding.source >= 32) return FALSE;
    source_snapshot(e, binding.source, snapshot); return TRUE;
}
