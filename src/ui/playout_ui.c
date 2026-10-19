#include "radios/playout_ui.h"
#include "radios/header_navigation.h"
#include "radios/playout.h"
#include "radios/player_binding.h"
#include "radios/audio_ui.h"
#include "radios/advert_ui.h"
#include "radios/broadcast_ui.h"
#include "radios/broadcast_header.h"
#include "radios/player_workspace.h"
#include "radios/copyright_policy.h"
#include "radios/programme_model.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct RadioSPlayoutController {
    gint references;
    RadioSViewContext *ctx;
    gint player_deck;
    gboolean scoped;
    guint64 scoped_epoch;
    GHashTable *scoped_epochs;
    RadioSAudioEngine *engine;
    RadioSPlayoutPlan *plan;
    RadioSPlayoutPlan *pending_plan;
    RadioSPlayoutPlan *replacement_plan;
    RadioSPlayoutPlan *window_tail;
    gboolean window_pending;
    guint64 replacement_epoch;
    RadioSPlayoutHistory *history;
    RadioSDocument *history_document;
    GWeakRef owner;
    gulong owner_handler;
    guint timer;
    guint64 intent;
    GHashTable *observed;
    JsonObject *calendar_turns;
    JsonNode *probability_checkpoint, *policy_selection_checkpoint;
    JsonNode *automation_config;
    RadioSPlayoutSnapshot snapshot;
    gboolean closed, preparing, automatic, history_loaded, failed, prefetching;
    gchar extension_error[256];
    guint extension_failures;
    gint64 extension_retry_at;
    /* Kılavuz 2026-10-18 §06/§08 çekme adedi: the window settings, and the
     * programme whose remaining windows this run still appends. */
    guint window_event_fetch, window_content_fetch;
    JsonNode *programme_config;
    guint programme_next_index, programme_window_size, programme_mode;
    gdouble programme_overlap;
    /* Kılavuz §04/§07: the duration wait of a live step, keyed by the run
     * epoch and the index of the row before the step. */
    guint64 live_wait_epoch;
    guint live_wait_index;
    gint64 live_wait_until_utc_us;
};
struct RadioSPlayoutResume {
    RadioSPlayoutPlan *plan;
    JsonNode *automation_config;
    gboolean automatic, paused;
    gchar loudness_automation_id[37];
    gboolean loudness_assigned;
    JsonNode *programme_config; /* remaining windows of an interrupted windowed programme */
    guint programme_next_index, programme_window_size, programme_mode;
    gdouble programme_overlap;
};
typedef struct {
    RadioSPlayoutController *playout;
    guint64 intent;
    gboolean preview, automatic, append;
    gboolean programme_window; /* continuation window of a windowed programme run */
    guint window_next, window_size;
    guint loudness_context;
    gchar loudness_automation_id[37];
    gboolean loudness_assigned;
    guint64 epoch;
    gint64 boundary_start_utc_us;
    gboolean player_autodj;
    guint pair[2];
    guint64 pair_generation[2];
    gint pair_selection;
    gboolean pair_selection_captured;
    JsonNode *config;
    RadioSPlayoutRequest request;
    RadioSPlayoutHistory *history;
    RadioSDataCallback callback;
    gpointer user;
    GDestroyNotify destroy;
} Preparation;
static GList *copyright_controllers;
typedef struct { gchar *station, *id; gint64 revision; } CopyrightRecord;
static void copyright_record_free(gpointer value)
{ CopyrightRecord *record = value; g_free(record->station); g_free(record->id); g_free(record); }

static gboolean prepare_start(RadioSViewContext *, GtkWidget *, const gchar *, const gchar *, JsonArray *, JsonObject *,
    guint, guint, gdouble, gint64, gboolean, RadioSDataCallback, gpointer, GDestroyNotify, GError **);
static gboolean prepare_extension(RadioSPlayoutController *, const RadioSAudioRunSnapshot *, GError **);
static gboolean prepare_programme_window(RadioSPlayoutController *, const RadioSAudioRunSnapshot *, GError **);
static gboolean install_plan(RadioSPlayoutController *, RadioSPlayoutPlan *, GError **);
static void capture_history(RadioSPlayoutController *);
static gboolean replace_remaining_commit(RadioSPlayoutController *, RadioSPlayoutPlan *, const RadioSAudioRunSnapshot *, GError **);
static void extension_clear(RadioSPlayoutController *p)
{
    p->prefetching = FALSE; p->extension_error[0] = 0; p->extension_failures = 0; p->extension_retry_at = 0;
    radios_playout_plan_free(p->pending_plan); p->pending_plan = NULL;
    radios_playout_plan_free(p->replacement_plan); p->replacement_plan = NULL;
    radios_playout_plan_free(p->window_tail); p->window_tail = NULL; p->window_pending = FALSE;
    g_clear_pointer(&p->programme_config, json_node_free); p->programme_next_index = p->programme_window_size = 0;
}
static RadioSPlayoutController *ref(RadioSPlayoutController *p)
{ if (p) g_atomic_int_inc(&p->references); return p; }
static void notice(RadioSPlayoutController *p, const gchar *message)
{ if (!p->closed && p->ctx->notify) p->ctx->notify(p->ctx->notify_data, message); }
static gboolean invalid(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, message); return FALSE; }
/* Publication belongs to the accepted execution, never the selected widget.
 * These bounded options are copied by configure/adopt and by pending plans. */
static void plan_loudness_origin(RadioSPlayoutPlan *plan, guint context, const gchar *document_id, gboolean assigned)
{
    plan->options.loudness_context = context;
    g_strlcpy(plan->options.loudness_automation_id, document_id ? document_id : "", sizeof plan->options.loudness_automation_id);
    plan->options.loudness_assigned = assigned;
}
static gboolean loudness_origin_valid(const gchar *document_id, gboolean assigned, GError **error)
{
    if ((document_id && *document_id && !g_uuid_string_is_valid(document_id)) ||
        (assigned && (!document_id || !*document_id)))
        return invalid(error, "Ses eşitleme otomasyon kaynağı geçersiz.");
    return TRUE;
}
static void preparation_loudness_origin(Preparation *job, RadioSPlayoutPlan *plan)
{ plan_loudness_origin(plan, job->loudness_context, job->loudness_automation_id, job->loudness_assigned); }
static void default_policy_snapshot(RadioSViewContext *ctx, RadioSPlayoutRequest *request)
{
    request->has_default_policy = radios_broadcast_transition_policy(ctx, &request->default_mode,
        &request->default_overlap_seconds, &request->default_beat_sync);
}
static void extension_failed(RadioSPlayoutController *p, const GError *error)
{
    gboolean transient = error && ((error->domain == RADIOS_DATA_CLIENT_ERROR &&
        (error->code == RADIOS_DATA_CLIENT_ERROR_TRANSPORT || error->code == RADIOS_DATA_CLIENT_ERROR_TIMEOUT ||
         error->code == RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED || error->code == RADIOS_DATA_CLIENT_ERROR_UNAVAILABLE ||
         error->code == RADIOS_DATA_CLIENT_ERROR_HTTP)) ||
        (error->domain == G_IO_ERROR && (error->code == G_IO_ERROR_NOT_CONNECTED || error->code == G_IO_ERROR_TIMED_OUT ||
         error->code == G_IO_ERROR_CONNECTION_CLOSED || error->code == G_IO_ERROR_BUSY)));
    g_strlcpy(p->extension_error, error ? error->message : "Sonraki rotasyon hazırlanamadı.", sizeof p->extension_error);
    if (transient) {
        guint delay = MIN(30u, 1u << MIN(p->extension_failures, 5u));
        ++p->extension_failures; p->extension_retry_at = g_get_monotonic_time() + delay * G_USEC_PER_SEC;
    } else p->extension_retry_at = 0;
    notice(p, p->extension_error);
}
static RadioSPlayoutController *controller(RadioSViewContext *ctx)
{ return ctx && ctx->playout && !ctx->playout->closed ? ctx->playout : NULL; }
gboolean radios_playout_ui_ready(RadioSViewContext *ctx)
{ RadioSPlayoutController *p = controller(ctx); return p && radios_data_document_ready(p->history_document); }
static gboolean scoped_owns_run(RadioSPlayoutController *p)
{
    if (!p->scoped || !p->scoped_epoch) return FALSE;
    RadioSAudioRunSnapshot run;
    radios_audio_engine_player_runner_snapshot(p->engine, (guint)p->player_deck, &run);
    return run.epoch == p->scoped_epoch;
}
static void scoped_capture_epoch(RadioSPlayoutController *p)
{
    if (!p->scoped) return;
    RadioSAudioRunSnapshot run;
    radios_audio_engine_player_runner_snapshot(p->engine, (guint)p->player_deck, &run);
    p->scoped_epoch = run.epoch;
    if (!g_hash_table_contains(p->scoped_epochs, &run.epoch)) {
        guint64 *epoch = g_new(guint64, 1); *epoch = run.epoch;
        g_hash_table_add(p->scoped_epochs, epoch);
    }
}
static void checkpoint_broadcast(RadioSPlayoutController *p)
{ if (p->player_deck < 0) radios_broadcast_ui_checkpoint(p->ctx); }
static void checkpoint_advert(RadioSPlayoutController *p)
{ if (p->player_deck < 0) radios_advert_ui_checkpoint(p->ctx); }
static gboolean runner_command_once(RadioSPlayoutController *p, RadioSAudioPlayerRunCommand command, GError **error)
{
    if (p->scoped && !scoped_owns_run(p))
        return command == RADIOS_AUDIO_PLAYER_RUN_STOP ? TRUE : invalid(error, "Bu AutoDJ kapsamı artık bu sıranın sahibi değil.");
    if (p->player_deck >= 0) {
        gboolean result = radios_audio_engine_player_runner_action(p->engine,(guint)p->player_deck,command,error);
        if (result) scoped_capture_epoch(p);
        return result;
    }
    switch (command) {
    case RADIOS_AUDIO_PLAYER_RUN_START: return radios_audio_engine_runner_start(p->engine,error);
    case RADIOS_AUDIO_PLAYER_RUN_PAUSE: return radios_audio_engine_runner_pause(p->engine,error);
    case RADIOS_AUDIO_PLAYER_RUN_RESUME: return radios_audio_engine_runner_resume(p->engine,error);
    case RADIOS_AUDIO_PLAYER_RUN_STOP: return radios_audio_engine_runner_stop(p->engine,error);
    case RADIOS_AUDIO_PLAYER_RUN_NEXT: return radios_audio_engine_runner_next(p->engine,error);
    case RADIOS_AUDIO_PLAYER_RUN_PREVIOUS: return radios_audio_engine_runner_previous(p->engine,error);
    }
    return invalid(error,"Geçersiz oynatıcı sıra komutu.");
}
/* A pause/seek flush reopens the MAIN output within about 0.2 s; meanwhile
 * the engine answers RESUME with UNAVAILABLE. Bridge that short gap instead
 * of leaving the run paused (the output stream has its own thread). */
static gboolean runner_command(RadioSPlayoutController *p, RadioSAudioPlayerRunCommand command, GError **error)
{
    gint64 deadline = g_get_monotonic_time() + 250 * G_TIME_SPAN_MILLISECOND;
    for (;;) {
        GError *local = NULL;
        if (runner_command_once(p, command, &local)) return TRUE;
        if (command != RADIOS_AUDIO_PLAYER_RUN_RESUME || g_get_monotonic_time() >= deadline ||
            !g_error_matches(local, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_UNAVAILABLE)) {
            g_propagate_error(error, local); return FALSE;
        }
        g_error_free(local); g_usleep(10 * 1000);
    }
}
static void runner_snapshot(RadioSPlayoutController *p, RadioSAudioRunSnapshot *snapshot)
{
    if (p->player_deck >= 0) radios_audio_engine_player_runner_snapshot(p->engine,(guint)p->player_deck,snapshot);
    else radios_audio_engine_runner_snapshot(p->engine,snapshot);
    if (p->scoped && (!p->scoped_epoch || snapshot->epoch != p->scoped_epoch))
        *snapshot = (RadioSAudioRunSnapshot){.state = RADIOS_AUDIO_RUN_IDLE, .deck_current = -1, .deck_next = -1};
}
static guint runner_receipts(RadioSPlayoutController *p, RadioSAudioRunReceipt *out, guint capacity)
{
    guint count = p->player_deck >= 0 ? radios_audio_engine_player_runner_receipts(p->engine,(guint)p->player_deck,out,capacity) :
        radios_audio_engine_runner_receipts(p->engine,out,capacity);
    if (!p->scoped) return count;
    guint kept = 0;
    for (guint i = 0; i < count; ++i)
        if (g_hash_table_contains(p->scoped_epochs, &out[i].epoch)) out[kept++] = out[i];
    return kept;
}
/* Deep copy without a text round trip: a 1 MB programme copies in ~5 ms
 * instead of ~13 ms on the GTK thread (kılavuz §08 liste sonu donması). */
static JsonNode *clone(JsonNode *source)
{ return radios_programme_clone(source); }
static JsonNode *clone_object(JsonObject *source)
{ JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, source); JsonNode *result = clone(node); json_node_free(node); return result; }
JsonObject *radios_playout_ui_copyright_policy_get(RadioSViewContext *ctx)
{
    if (!ctx || !ctx->parent) return NULL;
    CopyrightRecord *record = g_object_get_data(G_OBJECT(ctx->parent), "radios-copyright-policy-record");
    const gchar *station = radios_data_station_id(ctx);
    if (record && *station && !g_str_equal(station, record->station)) {
        g_object_set_data(G_OBJECT(ctx->parent), "radios-copyright-policy", NULL);
        g_object_set_data(G_OBJECT(ctx->parent), "radios-copyright-policy-record", NULL);
        return NULL;
    }
    return g_object_get_data(G_OBJECT(ctx->parent), "radios-copyright-policy");
}
static void controller_copyright(RadioSPlayoutController *p, JsonObject *policy)
{
    radios_playout_plan_copyright(p->pending_plan, policy);
    if (!p->plan) return;
    radios_playout_plan_copyright(p->plan, policy);
    if (p->scoped && !scoped_owns_run(p)) return;
    guint count = p->plan->entries->len;
    if (!count || count > RADIOS_AUDIO_RUN_MAX_ITEMS) return;
    RadioSAudioCopyrightWindow windows[RADIOS_AUDIO_RUN_MAX_ITEMS];
    for (guint i = 0; i < count; ++i)
        windows[i] = ((RadioSPlayoutEntry *)g_ptr_array_index(p->plan->entries, i))->run.copyright;
    radios_audio_engine_runner_copyright(p->engine, p->player_deck, windows, count, NULL);
    RadioSAudioCopyrightWindow adjunct[RADIOS_AUDIO_RUN_MAX_ITEMS];
    for (guint i = 0; i < count; ++i)
        adjunct[i] = ((RadioSPlayoutEntry *)g_ptr_array_index(p->plan->entries, i))->run.adjunct.copyright;
    radios_audio_engine_runner_adjunct_copyright(p->engine, p->player_deck, adjunct, count, NULL);
}
void radios_playout_ui_copyright_policy(RadioSViewContext *ctx, JsonObject *policy)
{
    if (!ctx || !ctx->parent || !radios_copyright_policy_validate(policy, NULL)) return;
    JsonNode *copy = policy ? clone_object(policy) : NULL;
    JsonObject *owned = copy ? json_object_ref(json_node_get_object(copy)) : NULL;
    if (copy) json_node_free(copy);
    g_object_set_data_full(G_OBJECT(ctx->parent), "radios-copyright-policy", owned,
        owned ? (GDestroyNotify)json_object_unref : NULL);
    for (GList *item = copyright_controllers; item; item = item->next) {
        RadioSPlayoutController *p = item->data;
        if (!p->closed && p->ctx->parent == ctx->parent) controller_copyright(p, owned);
    }
}
gboolean radios_playout_ui_copyright_policy_record(RadioSViewContext *ctx,
    JsonObject *policy, const gchar *document_id, gint64 revision)
{
    if (!ctx || !ctx->parent || revision < 0 || (revision && !g_uuid_string_is_valid(document_id)) ||
        !radios_copyright_policy_validate(policy, NULL)) return FALSE;
    radios_playout_ui_copyright_policy_get(ctx); /* clears a different station */
    CopyrightRecord *before = g_object_get_data(G_OBJECT(ctx->parent), "radios-copyright-policy-record");
    const gchar *id = document_id ? document_id : "", *station = radios_data_station_id(ctx);
    if (before && ((before->revision > 0 && !revision) ||
        (g_str_equal(before->id, id) && revision <= before->revision))) return FALSE;
    CopyrightRecord *record = g_new0(CopyrightRecord, 1);
    record->station = g_strdup(station); record->id = g_strdup(id); record->revision = revision;
    radios_playout_ui_copyright_policy(ctx, policy);
    g_object_set_data_full(G_OBJECT(ctx->parent), "radios-copyright-policy-record", record, copyright_record_free);
    return TRUE;
}
static RadioSPlayoutPlan *build_plan(RadioSViewContext *ctx, JsonObject *prepared,
    const RadioSPlayoutRequest *request, GError **error)
{
    JsonNode *policy = json_object_get_member(prepared, "copyright_policy");
    if (policy && JSON_NODE_HOLDS_OBJECT(policy))
        radios_playout_ui_copyright_policy_record(ctx, json_node_get_object(policy),
            radios_json_string(prepared, "copyright_policy_document_id", ""),
            radios_json_int(prepared, "copyright_policy_revision", 0));
    RadioSPlayoutRequest current = *request;
    current.copyright_policy = radios_playout_ui_copyright_policy_get(ctx);
    JsonNode *local = clone_object(prepared); JsonObject *input = json_node_get_object(local);
    RadioSPlayoutController *playout = controller(ctx);
    if (playout && playout->probability_checkpoint) json_object_set_member(input, "probability_checkpoint", json_node_copy(playout->probability_checkpoint));
    if (playout && playout->policy_selection_checkpoint) json_object_set_member(input, "policy_selection_checkpoint", json_node_copy(playout->policy_selection_checkpoint));
    RadioSPlayoutPlan *plan = radios_playout_plan_build(input, &current, error); json_node_free(local);
    if (plan && !plan->entries->len) {
        radios_playout_plan_free(plan);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK,
            "Geçerli telif ve otomasyon kuralları içinde çalınabilir parça kalmadı.");
        return NULL;
    }
    return plan;
}
/* Native timing re-validates a calendar selection made before the prepare
 * round trip. Only these native guards identify a genuine remaining-window,
 * repeat-gap or playlist-turn failure of that selection: the scope runner
 * selects again instead of ending the schedule. Other INVALID_DATA errors
 * stay fatal and are never retried. */
static void calendar_timing_retry(GError **error)
{
    static const gchar *const timing[] = {
        "Tempo ayarlı rotasyon parçası playlist veya program zaman aralığını aşıyor.",
        "Tempo ayarlı gerçek çalma sırası parça, sanatçı veya albüm tekrar süresini karşılamıyor.",
        "Tempo ayarlı playlist sırası belirlenen playlist dönüş sıklığını karşılamıyor."};
    if (!error || !*error || !g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA)) return;
    for (guint i = 0; i < G_N_ELEMENTS(timing); ++i) {
        if (!g_str_equal((*error)->message, timing[i])) continue;
        gchar *message = g_strdup((*error)->message); g_clear_error(error);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK, message); g_free(message); return;
    }
}
static void owner_clear(RadioSPlayoutController *p)
{
    GtkWidget *owner = g_weak_ref_get(&p->owner); gulong handler = p->owner_handler; p->owner_handler = 0;
    if (owner && handler && g_signal_handler_is_connected(owner, handler)) g_signal_handler_disconnect(owner, handler);
    g_clear_object(&owner); g_weak_ref_set(&p->owner, NULL);
}
static void owner_destroyed(GtkWidget *owner, gpointer value)
{
    (void)owner; RadioSPlayoutController *p = value;
    if (p->closed) return;
    runner_command(p,RADIOS_AUDIO_PLAYER_RUN_STOP, NULL); checkpoint_broadcast(p); checkpoint_advert(p); capture_history(p); extension_clear(p);
    ++p->intent; p->preparing = p->automatic = p->failed = FALSE; p->owner_handler = 0;
    p->snapshot = (RadioSPlayoutSnapshot){.state = RADIOS_PLAYOUT_IDLE, .deck = -1, .next_deck = -1};
    g_weak_ref_set(&p->owner, NULL);
}
static void history_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer value)
{
    (void)ctx; (void)owner; RadioSPlayoutController *p = value;
    if (p->closed || p->history_loaded) return;
    radios_playout_history_import(p->history, json_object_get_array_member(config, "entries"));
    radios_playout_history_set_rotation(p->history, json_object_has_member(config, "rotation") ? json_object_get_object_member(config, "rotation") : NULL);
    p->history_loaded = TRUE;
}
static void preparation_free(gpointer value)
{
    Preparation *job = value;
    if (job->destroy) job->destroy(job->user);
    if (job->config) json_node_free(job->config);
    radios_playout_history_free(job->history); radios_playout_ui_unref(job->playout); g_free(job);
}
static RadioSPlayoutState map_state(RadioSAudioRunState state)
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
static void observe_calendar_turn(RadioSPlayoutController *p, RadioSPlayoutEntry *entry, gint64 actual_utc_us)
{
    JsonNode *start = entry->calendar_constraints ? json_object_get_member(entry->calendar_constraints, "source_turn_start") : NULL;
    if (!start || !JSON_NODE_HOLDS_VALUE(start) || json_node_get_value_type(start) != G_TYPE_BOOLEAN ||
        !json_node_get_boolean(start) || !g_uuid_string_is_valid(entry->source_id) || actual_utc_us <= 0) return;
    gchar *id = g_ascii_strdown(entry->source_id, -1);
    gint64 previous = radios_json_int(p->calendar_turns, id, 0);
    json_object_set_int_member(p->calendar_turns, id, MAX(previous, actual_utc_us)); g_free(id);
    if (json_object_get_size(p->calendar_turns) > 2000) {
        GList *keys = json_object_get_members(p->calendar_turns); const gchar *oldest = NULL; gint64 stamp = G_MAXINT64;
        for (GList *item = keys; item; item = item->next) {
            gint64 time = radios_json_int(p->calendar_turns, item->data, 0);
            if (time < stamp) { stamp = time; oldest = item->data; }
        }
        if (oldest) json_object_remove_member(p->calendar_turns, oldest);
        g_list_free(keys);
    }
}
static gboolean observe(RadioSPlayoutController *p, const RadioSAudioRunReceipt *receipt)
{
    if (!p->plan || receipt->index >= p->plan->entries->len) return FALSE;
    RadioSPlayoutEntry *e = g_ptr_array_index(p->plan->entries, receipt->index);
    if (receipt->position_frames <= e->run.media.cue_in_frame) return FALSE;
    gchar key[64]; g_snprintf(key, sizeof key, "%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT, receipt->epoch, receipt->occurrence);
    if (g_hash_table_contains(p->observed, key)) return FALSE;
    g_hash_table_add(p->observed, g_strdup(key));
    g_snprintf(key, sizeof key, "entry:%" G_GUINT64_FORMAT ":%u", receipt->epoch, receipt->index);
    g_hash_table_add(p->observed, g_strdup(key));
    radios_playout_history_observe(p->history, e->run.media.media_id, e->run.media.version_id, e->artist, e->album,
        receipt->first_utc_us, receipt->position_frames - e->run.media.cue_in_frame);
    observe_calendar_turn(p, e, receipt->first_utc_us);
    JsonObject *display = json_node_get_object(p->plan->display);
    JsonArray *rows = json_object_get_array_member(display, "entries");
    JsonObject *row = rows && receipt->index < json_array_get_length(rows) ? json_array_get_object_element(rows, receipt->index) : NULL;
    const gchar *fields[] = {"probability_checkpoint", "policy_selection_checkpoint"};
    JsonNode **slots[] = {&p->probability_checkpoint, &p->policy_selection_checkpoint};
    for (guint i = 0; row && i < G_N_ELEMENTS(fields); ++i) {
        JsonNode *checkpoint = json_object_get_member(row, fields[i]);
        if (checkpoint && JSON_NODE_HOLDS_OBJECT(checkpoint)) { g_clear_pointer(slots[i], json_node_free); *slots[i] = clone_object(json_node_get_object(checkpoint)); }
    }
    if (e->rotation_after) radios_playout_history_set_rotation(p->history, e->rotation_after);
    p->history_loaded = TRUE;
    return TRUE;
}
static gint receipt_compare(gconstpointer a, gconstpointer b)
{
    const RadioSAudioRunReceipt *left = a, *right = b;
    if (left->first_utc_us != right->first_utc_us) return left->first_utc_us < right->first_utc_us ? -1 : 1;
    return left->occurrence < right->occurrence ? -1 : left->occurrence != right->occurrence;
}
static void capture_history(RadioSPlayoutController *p)
{
    RadioSAudioRunReceipt receipts[RADIOS_AUDIO_RUN_MAX_RECEIPTS];
    guint count = runner_receipts(p, receipts, G_N_ELEMENTS(receipts));
    qsort(receipts, count, sizeof receipts[0], receipt_compare);
    gboolean changed = FALSE;
    for (guint i = 0; i < count; ++i) changed |= observe(p, &receipts[i]);
    if (!changed) return;
    if (p->history_document && radios_data_document_ready(p->history_document)) {
        JsonNode *body = radios_playout_history_json(p->history);
        radios_data_document_dirty(p->history_document, TRUE);
        if (!radios_data_document_save(p->history_document, body)) notice(p, "Yerel çalma geçmişi kaydedilemedi; tekrar koruması bu oturumda korunuyor.");
        json_node_free(body);
    }
}
/* Repeating one accepted plan can run for days. Keep deduplication keys for
 * the bounded engine receipt window, so old epochs neither grow forever nor
 * get observed again after a restart. */
static void prune_observed(RadioSPlayoutController *p)
{
    if (g_hash_table_size(p->observed) <= RADIOS_AUDIO_RUN_MAX_RECEIPTS * 3u) return;
    RadioSAudioRunReceipt receipts[RADIOS_AUDIO_RUN_MAX_RECEIPTS];
    guint count = runner_receipts(p, receipts, G_N_ELEMENTS(receipts));
    GHashTable *keep = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < count; ++i) {
        const RadioSAudioRunReceipt *r = &receipts[i];
        g_hash_table_add(keep, g_strdup_printf("%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT, r->epoch, r->occurrence));
        g_hash_table_add(keep, g_strdup_printf("entry:%" G_GUINT64_FORMAT ":%u", r->epoch, r->index));
        g_hash_table_add(keep, g_strdup_printf("complete:%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT, r->epoch, r->occurrence));
    }
    GHashTableIter iter; gpointer key;
    g_hash_table_iter_init(&iter, p->observed);
    while (g_hash_table_iter_next(&iter, &key, NULL))
        if (!g_hash_table_contains(keep, key)) g_hash_table_iter_remove(&iter);
    g_hash_table_unref(keep);
}
static guint events_total(JsonObject *config)
{
    JsonNode *events = config ? json_object_get_member(config, "events") : NULL;
    return events && JSON_NODE_HOLDS_ARRAY(events) ? json_array_get_length(json_node_get_array(events)) : 0;
}
/* Kılavuz §06/§08 windowed programme continuation. The rotation refill above
 * stays as it is; this runs only for a non-automatic programme start that
 * still has windows left, one request in flight at a time (prefetching) and
 * with the same transient retry backoff as the rotation path. */
static void programme_poll(RadioSPlayoutController *p, const RadioSAudioRunSnapshot *actual)
{
    if (p->automatic || !p->programme_config || !p->plan) return;
    if (actual->state == RADIOS_AUDIO_RUN_FINISHED && p->pending_plan) {
        RadioSPlayoutPlan *ready = p->pending_plan; p->pending_plan = NULL; GError *error = NULL;
        runner_command(p, RADIOS_AUDIO_PLAYER_RUN_STOP, NULL);
        if (!install_plan(p, ready, &error)) {
            radios_playout_plan_free(ready); p->failed = TRUE; p->snapshot.state = RADIOS_PLAYOUT_ERROR;
            g_strlcpy(p->snapshot.error, error ? error->message : "Program devam ettirilemedi.", sizeof p->snapshot.error);
            notice(p, p->snapshot.error); g_clear_pointer(&p->programme_config, json_node_free);
        }
        g_clear_error(&error); return;
    }
    if (actual->state == RADIOS_AUDIO_RUN_FINISHED && p->extension_error[0] && !p->extension_retry_at) {
        /* The verified queue completed; the window that failed stays visible. */
        p->failed = TRUE; p->snapshot.state = RADIOS_PLAYOUT_ERROR;
        g_strlcpy(p->snapshot.error, p->extension_error, sizeof p->snapshot.error); notice(p, p->snapshot.error);
        g_clear_pointer(&p->programme_config, json_node_free); return;
    }
    if (p->prefetching || p->pending_plan || p->programme_next_index >= events_total(json_node_get_object(p->programme_config))) return;
    if (p->extension_error[0] && (!p->extension_retry_at || g_get_monotonic_time() < p->extension_retry_at)) return;
    if (actual->state != RADIOS_AUDIO_RUN_PLAYING && actual->state != RADIOS_AUDIO_RUN_OVERLAP && actual->state != RADIOS_AUDIO_RUN_PAUSED &&
        actual->state != RADIOS_AUDIO_RUN_MANUAL_WAIT && actual->state != RADIOS_AUDIO_RUN_WAITING_MEDIA && actual->state != RADIOS_AUDIO_RUN_FINISHED) return;
    if (actual->count - MIN(actual->count, actual->index) > MAX(3u, MIN(100u, p->programme_window_size / 2))) return;
    GError *error = NULL;
    if (!prepare_programme_window(p, actual, &error)) extension_failed(p, error);
    g_clear_error(&error);
}
static guint accepted_window_size(RadioSPlayoutController *p, RadioSPlayoutPlan *plan)
{
    JsonArray *rows = json_object_get_array_member(json_node_get_object(plan->display), "entries");
    const gchar *previous = NULL; guint events = 0, contents = 0, count = 0;
    for (guint i = 0; i < plan->entries->len; ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i);
        JsonObject *row = rows && i < json_array_get_length(rows) ? json_array_get_object_element(rows, i) : NULL;
        const gchar *block = radios_json_string(row, "block_id", "accepted-programme");
        gboolean attached = radios_programme_kind_attaches(entry->kind);
        if (!previous || (!attached && g_strcmp0(previous, block))) {
            if (p->window_event_fetch && events >= p->window_event_fetch) break;
            ++events; contents = 0; previous = block;
        }
        guint cost = 1 + (entry->run.adjunct.enabled ? 1u : 0u);
        if (count && p->window_content_fetch && contents + cost > p->window_content_fetch) break;
        contents += cost; ++count;
        if (count == RADIOS_AUDIO_RUN_MAX_ITEMS) break;
    }
    return MAX(count, 1u);
}
/* All starts (weekly, private and AutoDJ included) use the same bounded native
 * preparation window. Its immutable metadata tail never owns decoder PCM. */
static gboolean native_window_poll(RadioSPlayoutController *p, const RadioSAudioRunSnapshot *actual)
{
    if (p->window_pending && actual->state == RADIOS_AUDIO_RUN_FINISHED && p->pending_plan) {
        RadioSPlayoutPlan *ready = p->pending_plan; p->pending_plan = NULL; p->window_pending = FALSE;
        GError *error = NULL; runner_command(p, RADIOS_AUDIO_PLAYER_RUN_STOP, NULL);
        if (!install_plan(p, ready, &error)) { radios_playout_plan_free(ready); notice(p, error ? error->message : "Yayın penceresi sürdürülemedi."); }
        g_clear_error(&error); return TRUE;
    }
    if (!p->window_tail || !p->window_tail->entries->len) return p->window_pending;
    if (p->pending_plan || actual->count - MIN(actual->count, actual->index) > 2) return TRUE;
    RadioSPlayoutPlan *next = radios_playout_plan_take_prefix(p->window_tail, accepted_window_size(p, p->window_tail));
    if (!p->window_tail->entries->len) { radios_playout_plan_free(p->window_tail); p->window_tail = NULL; }
    RadioSAudioRunItem *items = radios_playout_plan_items(next); GError *error = NULL;
    gboolean appended = radios_audio_engine_runner_append_source(p->engine, p->player_deck, items, next->entries->len, &error); g_free(items);
    if (appended) {
        JsonArray *rows = json_object_get_array_member(json_node_get_object(p->plan->display), "entries");
        JsonArray *added = json_object_get_array_member(json_node_get_object(next->display), "entries");
        for (guint i = 0; added && i < json_array_get_length(added); ++i) json_array_add_element(rows, radios_programme_clone(json_array_get_element(added, i)));
        while (next->entries->len) g_ptr_array_add(p->plan->entries, g_ptr_array_steal_index(next->entries, 0));
        radios_playout_plan_refresh_jingle_metadata(p->plan); radios_playout_plan_free(next);
    } else { p->pending_plan = next; p->window_pending = TRUE; }
    g_clear_error(&error); return TRUE;
}
/* Kılavuz §04/§07 (PDF s.15): the row before a live step with a duration
 * rule ends into MANUAL_WAIT (planner.c). The step lasts duration_ms from
 * that actual end, then the run continues with the next row, exactly like
 * the operator's Next; an earlier Next ends the step earlier. A pause leaves
 * the wait and a later return restarts it. Manual steps keep waiting. */
static gboolean live_wait_poll(RadioSPlayoutController *p, const RadioSAudioRunSnapshot *actual)
{
    JsonObject *step = NULL;
    if (actual->state == RADIOS_AUDIO_RUN_MANUAL_WAIT && p->plan && actual->index < p->plan->entries->len) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(p->plan->entries, actual->index);
        JsonNode *node = g_str_equal(actual->entry_id, entry->run.entry_id) && entry->binding ?
            json_object_get_member(entry->binding, "live_step") : NULL;
        step = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    }
    if (!step || g_strcmp0(radios_json_string(step, "end_rule", ""), "duration")) {
        p->live_wait_until_utc_us = 0; p->snapshot.live_wait_until_utc_us = 0; return FALSE;
    }
    gint64 now = g_get_real_time();
    if (!p->live_wait_until_utc_us || p->live_wait_epoch != actual->epoch || p->live_wait_index != actual->index) {
        p->live_wait_epoch = actual->epoch; p->live_wait_index = actual->index;
        p->live_wait_until_utc_us = now + MAX(radios_json_int(step, "duration_ms", 0), G_GINT64_CONSTANT(0)) * 1000;
    }
    p->snapshot.live_wait_until_utc_us = p->live_wait_until_utc_us;
    if (now < p->live_wait_until_utc_us) return FALSE;
    GError *error = NULL; gboolean moved = radios_playout_ui_action(p->ctx, RADIOS_PLAYOUT_NEXT, &error);
    if (moved) { p->live_wait_until_utc_us = 0; p->snapshot.live_wait_until_utc_us = 0; }
    g_clear_error(&error); return moved;
}
static gboolean poll(gpointer value)
{
    RadioSPlayoutController *p = value;
    if (p->closed) return G_SOURCE_REMOVE;
    if (p->preparing || p->failed) return G_SOURCE_CONTINUE;
    RadioSAudioRunSnapshot actual; runner_snapshot(p, &actual);
    capture_history(p);
    if (p->replacement_plan) {
        RadioSPlayoutPlan *future = p->replacement_plan; p->replacement_plan = NULL; GError *error = NULL;
        if (actual.epoch != p->replacement_epoch) {
            radios_playout_plan_free(future); notice(p, "Şema hazırlanırken yayın değişti; eski sıraya dokunulmadı.");
        } else if (!replace_remaining_commit(p, future, &actual, &error)) {
            if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_BUSY)) p->replacement_plan = future;
            else { notice(p, error ? error->message : "Yeni şema uygulanamadı; mevcut sıra korunuyor."); radios_playout_plan_free(future); }
        }
        g_clear_error(&error); runner_snapshot(p, &actual);
    }
    p->snapshot.state = map_state(actual.state); p->snapshot.index = actual.index;
    p->snapshot.total = actual.count; p->snapshot.deck = actual.deck_current;
    p->snapshot.next_deck = actual.deck_next;
    p->snapshot.position_ms = actual.actual_current_frame / 48;
    g_strlcpy(p->snapshot.entry_id, actual.entry_id, sizeof p->snapshot.entry_id);
    p->snapshot.next_entry_id[0] = '\0'; p->snapshot.cue_in_ms = 0;
    g_strlcpy(p->snapshot.error, actual.error, sizeof p->snapshot.error);
    if (p->plan && actual.index < p->plan->entries->len) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(p->plan->entries, actual.index);
        p->snapshot.duration_ms = entry->run.media.cue_out_frame / 48;
        p->snapshot.cue_in_ms = entry->run.media.cue_in_frame / 48;
        guint next_index = actual.deck_next >= 0 ? actual.next_index : actual.index + 1;
        if (next_index < p->plan->entries->len) {
            RadioSPlayoutEntry *next = g_ptr_array_index(p->plan->entries, next_index);
            g_strlcpy(p->snapshot.next_entry_id, next->run.entry_id, sizeof p->snapshot.next_entry_id);
        }
        p->snapshot.transition = entry->run.mode;
        p->snapshot.overlap_ms = entry->run.overlap_frames / 48;
        guint64 ending_frame = radios_audio_run_trigger_frame(&entry->run, &entry->run.rhythm,
            entry->run.tempo_ratio > 0 ? entry->run.tempo_ratio : 1, entry->run.media.cue_out_frame);
        if (entry->run.fade_start_frame > entry->run.media.cue_in_frame)
            ending_frame = MIN(ending_frame, entry->run.fade_start_frame);
        /* Kılavuz s.11: "bitişe yaklaşma" comes before the transition. The
         * warning opens a preparation region ahead of the accepted trigger
         * (the transition's own length, at least ten seconds) — S still
         * shows there — and lasts until the transition completes. */
        guint64 lead = MAX(entry->run.overlap_frames, G_GUINT64_CONSTANT(10) * 48000);
        ending_frame = ending_frame > entry->run.media.cue_in_frame + lead ? ending_frame - lead : entry->run.media.cue_in_frame;
        p->snapshot.ending = actual.actual_current_frame >= ending_frame &&
            actual.actual_current_frame < entry->run.media.cue_out_frame;
        p->snapshot.start_utc_us = p->plan->options.start_utc_us;
        g_strlcpy(p->snapshot.title, entry->title, sizeof p->snapshot.title);
        const gchar *invalid_utf8 = NULL;
        if (!g_utf8_validate(p->snapshot.title, -1, &invalid_utf8)) p->snapshot.title[invalid_utf8 - p->snapshot.title] = '\0';
        if (actual.deck_current >= 0) radios_audio_ui_runner_duration(p->ctx, (guint)actual.deck_current, entry->run.duration_frames / 48);
        if (actual.deck_next >= 0 && actual.next_index < p->plan->entries->len) {
            RadioSPlayoutEntry *next = g_ptr_array_index(p->plan->entries, actual.next_index);
            radios_audio_ui_runner_duration(p->ctx, (guint)actual.deck_next, next->run.duration_frames / 48);
        }
    }
    /* The run moved on: the next tick observes it afresh. */
    if (live_wait_poll(p, &actual)) return G_SOURCE_CONTINUE;
    if (native_window_poll(p, &actual)) return G_SOURCE_CONTINUE;
    /* A durable advert handover owns the next queue boundary. Keep observing
     * the old PCM/history, but do not replace it with a rotation continuation. */
    if (actual.handover_pending || radios_advert_ui_pending(p->ctx) || radios_broadcast_ui_emergency_pending(p->ctx)) return G_SOURCE_CONTINUE;
    if (p->automatic && actual.state == RADIOS_AUDIO_RUN_FINISHED && p->pending_plan) {
        RadioSPlayoutPlan *ready = p->pending_plan; p->pending_plan = NULL;
        GError *error = NULL;
        runner_command(p,RADIOS_AUDIO_PLAYER_RUN_STOP, NULL);
        if (!install_plan(p, ready, &error)) {
            radios_playout_plan_free(ready); p->automatic = FALSE; p->failed = TRUE; p->snapshot.state = RADIOS_PLAYOUT_ERROR;
            g_strlcpy(p->snapshot.error, error ? error->message : "Rotasyon devam ettirilemedi.", sizeof p->snapshot.error);
            notice(p, p->snapshot.error);
        }
        g_clear_error(&error);
        return G_SOURCE_CONTINUE;
    }
    if (p->automatic && actual.state == RADIOS_AUDIO_RUN_FINISHED && p->extension_error[0] && !p->extension_retry_at) {
        p->automatic = FALSE; p->failed = TRUE; p->snapshot.state = RADIOS_PLAYOUT_ERROR;
        g_strlcpy(p->snapshot.error, p->extension_error, sizeof p->snapshot.error); notice(p, p->snapshot.error);
    } else if (p->automatic && p->automation_config && p->plan && !p->prefetching && !p->pending_plan &&
        (!p->extension_error[0] || (p->extension_retry_at && g_get_monotonic_time() >= p->extension_retry_at)) &&
        (actual.state == RADIOS_AUDIO_RUN_PLAYING || actual.state == RADIOS_AUDIO_RUN_OVERLAP || actual.state == RADIOS_AUDIO_RUN_FINISHED) &&
        actual.count - MIN(actual.count, actual.index) <= MAX(3u, MIN(100u, p->plan->music_count / 2))) {
        GError *error = NULL;
        if (!prepare_extension(p, &actual, &error)) {
            extension_failed(p, error);
        }
        g_clear_error(&error);
    }
    programme_poll(p, &actual);
    return G_SOURCE_CONTINUE;
}
/* AutoDJ players reach MAIN through a mixer strip. A player without one gets
 * the first free strip, so starting AutoDJ never asks the operator to route
 * it by hand; the studio microphone strip is never taken. */
static gboolean autodj_route(RadioSPlayoutController *p, RadioSAudioSnapshot *audio, guint deck, GError **error)
{
    gint strip = -1;
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
        if (audio->mixer.channels[channel].source_deck == (gint)deck) strip = (gint)channel;
    if (strip < 0) {
        for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS && strip < 0; ++channel)
            if (audio->mixer.channels[channel].source_deck == -1 && !audio->mixer.channels[channel].pfl) strip = (gint)channel;
        if (strip < 0) {
            gchar *message = g_strdup_printf("Oynatıcı %u için boş mikser kanalı kalmadı; DJ mikserinde bir kanalı boşaltın.", deck + 1);
            gboolean result = invalid(error, message); g_free(message); return result;
        }
        RadioSAudioMixerConfig config = audio->mixer; config.channels[strip].source_deck = (gint)deck;
        if (!radios_audio_ui_set_mixer(p->ctx, &config, error)) return FALSE;
        audio->mixer = config;
    }
    if (!audio->decks[deck].main_routed && !radios_audio_ui_set_deck_main(p->ctx, deck, TRUE, error)) return FALSE;
    audio->decks[deck].main_routed = TRUE; return TRUE;
}
/* A jingle is optional accompaniment: its explicit player is never replaced
 * by another target or taken away from manual/CUE playback. Empty players can
 * obtain the same automatic MAIN mapping as the primary AutoDJ players. The
 * engine repeats this check atomically and skips unavailable accompaniment. */
static void route_adjuncts(RadioSPlayoutController *p, RadioSPlayoutPlan *plan,
    const RadioSAudioRunOptions *primary)
{
    plan->options.skip_unavailable_adjuncts = TRUE;
    gboolean auxiliary[32] = {FALSE};
    for (guint i = 0; i < plan->entries->len; ++i) {
        RadioSAudioRunAdjunct *voice = &((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, i))->run.adjunct;
        if (voice->enabled && voice->player < 32) auxiliary[voice->player] = TRUE;
    }
    for (guint i = 0; i < primary->pool_count; ++i) auxiliary[primary->deck_pool[i]] = FALSE;
    for (guint deck = 0; deck < 32; ++deck) if (auxiliary[deck]) {
        RadioSAudioSnapshot audio; radios_audio_engine_snapshot(p->engine, &audio);
        gint source = -1;
        /* An append may reuse this run's currently sounding auxiliary. Its
         * existing lease and physical mapping must remain untouched. */
        if (radios_audio_engine_deck_runner_owner(p->engine, deck, &source)) continue;
        gboolean cue = FALSE;
        for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
            if (audio.mixer.channels[channel].source_deck == (gint)deck) cue |= audio.mixer.channels[channel].pfl;
        if (audio.decks[deck].state != RADIOS_AUDIO_EMPTY || cue) continue;
        GError *routing_error = NULL;
        if (!autodj_route(p, &audio, deck, &routing_error)) {
            gchar *message = g_strdup_printf("Oynatıcı %u jingle için kullanılamıyor; yayın sırası devam edecek. %s",
                deck + 1, routing_error ? routing_error->message : "");
            notice(p, message); g_free(message); g_clear_error(&routing_error);
        }
    }
}
/* The native start/append may omit an unavailable optional voice. Mirror
 * that accepted omission once, so a later free auxiliary or a refill cannot
 * revive a jingle that was skipped in an earlier occurrence's event label. */
static void accepted_adjunct_metadata(RadioSPlayoutController *p, RadioSPlayoutPlan *plan)
{
    gboolean changed = FALSE;
    for (guint i = 0; i < plan->entries->len; ++i) {
        RadioSAudioRunAdjunct *voice = &((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, i))->run.adjunct;
        if (!voice->enabled) continue;
        gint owner = -2;
        if (!radios_audio_engine_deck_runner_owner(p->engine, voice->player, &owner) || owner != p->player_deck) {
            voice->enabled = FALSE; changed = TRUE;
        }
    }
    if (changed) radios_playout_plan_refresh_jingle_metadata(plan);
}
static gboolean choose_pool(RadioSPlayoutController *p, RadioSPlayoutPlan *plan, GError **error)
{
    if (plan->player_autodj) {
        RadioSAudioSnapshot audio; radios_audio_engine_snapshot(p->engine, &audio);
        for (guint index = 0; index < plan->options.pool_count; ++index) {
            guint deck = plan->options.deck_pool[index];
            gchar *key = g_strdup_printf("radios-deck-owner-%u", deck); GtkWidget *card = g_object_get_data(G_OBJECT(p->ctx->parent), key); g_free(key);
            RadioSAudioRunSnapshot private; radios_audio_engine_player_runner_snapshot(p->engine, deck, &private);
            gboolean defined = p->ctx->players ? radios_player_workspace_contains(p->ctx->players, deck) :
                card && !gtk_widget_in_destruction(card);
            gboolean other = private.owned[deck] && (!p->scoped || deck != (guint)p->player_deck);
            gchar *message = !defined ? g_strdup_printf("Oynatıcı %u tanımlı değil; rotasyonun kanal dağılımını güncelleyin.", deck + 1) :
                other ? g_strdup_printf("Oynatıcı %u kendi playlist sırasını çalıyor; o sırayı durdurun ya da başka kanal seçin.", deck + 1) :
                index && audio.decks[deck].state == RADIOS_AUDIO_PLAYING ? g_strdup_printf("Oynatıcı %u şu an çalıyor; AutoDJ çalan sesi kesmez. Durdurun ya da başka kanal seçin.", deck + 1) : NULL;
            if (message) { gboolean result = invalid(error, message); g_free(message); return result; }
            /* Paused, cued, loading or failed players are taken over: install
             * unloads every pool player that does not already hold the first item. */
            if (!autodj_route(p, &audio, deck, error)) return FALSE;
        }
        guint source = plan->options.deck_pool[0]; RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0);
        gboolean matches = g_str_equal(audio.decks[source].media_id, first->run.media.media_id) && g_str_equal(audio.decks[source].version_id, first->run.media.version_id);
        if (audio.decks[source].state == RADIOS_AUDIO_PLAYING && !matches) return invalid(error, "Çalan kaynak seçilen AutoDJ listesinden farklı; önce durdurun.");
        if (matches && fabs(audio.decks[source].tempo_ratio - first->run.tempo_ratio) > 0.000001)
            return invalid(error, "AutoDJ kaynağında önce manuel SYNC'i kapatın; mevcut tempo ve ses korunuyor.");
        if (matches && audio.decks[source].state == RADIOS_AUDIO_PLAYING) {
            RadioSPlayoutRequest request = {.now_utc_us = g_get_real_time(), .history = p->history};
            if (!radios_playout_plan_adopt_source(plan, audio.decks[source].cursor_frames, &request, error)) return FALSE;
        }
        if (first->run.media.cue_in_frame >= first->run.media.cue_out_frame)
            return invalid(error, "Çalan parçanın kalan süresi AutoDJ geçişi için çok kısa.");
        return TRUE;
    }
    if (p->player_deck >= 0) {
        RadioSAudioSnapshot audio; radios_audio_engine_snapshot(p->engine,&audio); guint deck = (guint)p->player_deck;
        gboolean mapped = FALSE;
        for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel) mapped |= audio.mixer.channels[channel].source_deck == (gint)deck;
        if (!mapped || !audio.decks[deck].main_routed || audio.decks[deck].state != RADIOS_AUDIO_EMPTY)
            return invalid(error,"Seçili oynatıcı boş ve MAIN'e atanmış olmalı; başka oynatıcıya geçilmedi.");
        for (guint index = 0; index < plan->entries->len; ++index) {
            RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries,index);
            if (entry->run.mode == RADIOS_AUDIO_RUN_CROSSFADE && entry->run.overlap_frames)
                return invalid(error,"Tek oynatıcı sırası bindirme yapamaz; Parça sonu, Cue veya Manuel geçiş seçin.");
            entry->run.overlap_frames = 0; entry->run.preferred_deck = (gint)deck;
        }
        plan->options.pool_count = 1; plan->options.deck_pool[0] = deck; return TRUE;
    }
    if (plan->automation) return TRUE;
    RadioSAudioSnapshot audio; radios_audio_engine_snapshot(p->engine, &audio);
    plan->options.pool_count = 0;
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS && plan->options.pool_count < 2; ++deck) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
        GtkWidget *card = p->ctx->parent ? g_object_get_data(G_OBJECT(p->ctx->parent), key) : NULL;
        g_free(key);
        if (!card || gtk_widget_in_destruction(card)) continue;
        gboolean mapped = FALSE;
        for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
            if (audio.mixer.channels[channel].source_deck == (gint)deck) mapped = TRUE;
        if (mapped && audio.decks[deck].main_routed && audio.decks[deck].state == RADIOS_AUDIO_EMPTY)
            plan->options.deck_pool[plan->options.pool_count++] = deck;
    }
    if (!plan->options.pool_count) return invalid(error, "Sıra için boş ve MAIN'e atanmış oynatıcı bulunamadı; canlı oynatıcılar korunuyor.");
    gboolean overlap = FALSE;
    for (guint i = 0; i < plan->entries->len; ++i) if (((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, i))->run.overlap_frames) overlap = TRUE;
    if (overlap && plan->options.pool_count < 2) return invalid(error, "Bindirme için boş ve MAIN'e atanmış iki oynatıcı gerekli.");
    return TRUE;
}
static gboolean install_plan(RadioSPlayoutController *p, RadioSPlayoutPlan *plan, GError **error)
{
    checkpoint_advert(p);
    JsonObject *copyright = radios_playout_ui_copyright_policy_get(p->ctx);
    if (copyright) radios_playout_plan_copyright(plan, copyright);
    if (!choose_pool(p, plan, error)) return FALSE;
    RadioSPlayoutPlan *tail = NULL;
    guint window = accepted_window_size(p, plan);
    if (window < plan->entries->len) {
        RadioSPlayoutPlan *head = radios_playout_plan_take_prefix(plan, window);
        tail = g_new(RadioSPlayoutPlan, 1); *tail = *plan; *plan = *head; g_free(head);
    }
    route_adjuncts(p, plan, &plan->options);
    RadioSAudioRunItem *items = radios_playout_plan_items(plan);
    gboolean configured = p->scoped ? radios_audio_engine_scope_runner_configure(p->engine,
        (guint)p->player_deck, items, plan->entries->len, &plan->options, error) :
        p->player_deck >= 0 ? radios_audio_engine_player_runner_configure(p->engine,(guint)p->player_deck,items,plan->entries->len,&plan->options,error) :
        radios_audio_engine_runner_configure(p->engine, items, plan->entries->len, &plan->options, error);
    gint adopt = -1; guint64 generation = 0;
    if (configured && plan->player_autodj) {
        RadioSAudioSnapshot audio; radios_audio_engine_snapshot(p->engine, &audio); guint source = plan->options.deck_pool[0];
        if ((audio.decks[source].state == RADIOS_AUDIO_READY || audio.decks[source].state == RADIOS_AUDIO_PAUSED || audio.decks[source].state == RADIOS_AUDIO_PLAYING) &&
            g_str_equal(audio.decks[source].media_id, items[0].media.media_id) && g_str_equal(audio.decks[source].version_id, items[0].media.version_id)) {
            adopt = (gint)source; generation = audio.decks[source].generation;
        }
    }
    gboolean accepted = configured && (p->scoped && plan->player_autodj ?
        radios_audio_engine_scope_runner_start_replacing(p->engine, (guint)p->player_deck, adopt, generation, error) :
        plan->player_autodj ? radios_audio_engine_runner_start_replacing(p->engine, adopt, generation, error) :
        runner_command(p,RADIOS_AUDIO_PLAYER_RUN_START,error));
    g_free(items);
    if (!accepted) { radios_playout_plan_free(tail); return FALSE; }
    if (tail) { radios_playout_plan_free(p->window_tail); p->window_tail = tail; }
    accepted_adjunct_metadata(p, plan);
    scoped_capture_epoch(p);
    if (plan->player_autodj) {
        GtkWidget *owner = g_weak_ref_get(&p->owner);
        const gchar *name = owner ? g_object_get_data(G_OBJECT(owner), "radios-player-playlist-name") : NULL;
        JsonArray *inputs = NULL; guint first = 0, deck = 0;
        gchar *loaded_name = NULL;
        if (owner && radios_broadcast_player_playlist(p->ctx, owner, &inputs, &loaded_name, &first, &deck)) {
            JsonArray *rows = json_object_get_array_member(json_node_get_object(plan->display), "entries");
            for (guint i = 0; i < plan->entries->len && first + i < json_array_get_length(inputs); ++i) {
                RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i);
                JsonObject *input = json_array_get_object_element(inputs, first + i);
                if (!g_str_equal(entry->run.media.media_id, radios_json_string(input, "media_id", "")) ||
                    !g_str_equal(entry->run.media.version_id, radios_json_string(input, "version_id", ""))) break;
                const gchar *input_name = radios_json_string(input, "playlist_name", "");
                const gchar *playlist_id = radios_json_string(input, "playlist_id", "");
                if (!*input_name) input_name = loaded_name ? loaded_name : "";
                JsonObject *row = json_array_get_object_element(rows, i);
                if (*input_name && !*radios_json_string(row, "playlist_id", "")) {
                    json_object_set_string_member(row, "source_name", input_name);
                    if (*playlist_id) json_object_set_string_member(row, "playlist_id", playlist_id);
                    g_free(entry->source_name); entry->source_name = g_strdup(input_name);
                }
                if (i == 0 && *input_name) name = input_name;
            }
            if (name && *name) json_object_set_string_member(json_node_get_object(plan->display), "playlist_title", name);
            json_array_unref(inputs); g_free(loaded_name);
        } else if (name && *name)
            json_object_set_string_member(json_node_get_object(plan->display), "playlist_title", name);
        g_clear_object(&owner);
    }
    radios_playout_plan_free(p->plan); p->plan = plan; g_hash_table_remove_all(p->observed);
    p->snapshot = (RadioSPlayoutSnapshot){.state = RADIOS_PLAYOUT_PREPARING, .total = plan->entries->len, .deck = -1, .next_deck = -1, .start_utc_us = plan->options.start_utc_us};
    return TRUE;
}
static gboolean replace_remaining_commit(RadioSPlayoutController *p, RadioSPlayoutPlan *future,
    const RadioSAudioRunSnapshot *actual, GError **error)
{
    if (!p->plan || actual->deck_current < 0 || actual->index >= p->plan->entries->len || !future->entries->len)
        return invalid(error, "Mevcut yayın artık yeni şemanın kaynağı değil.");
    RadioSAudioSnapshot audio; radios_audio_engine_snapshot(p->engine, &audio);
    guint prefix = actual->index + 1;
    if (future->entries->len > RADIOS_AUDIO_RUN_MAX_ITEMS - prefix)
        return invalid(error, "Yeni şema bu yayın penceresinin kapasitesini aşıyor.");
    /* Fixed pools remain the owned pool; an unrouted new schema inherits its
     * actual source/target order rather than choosing occupied MAIN players. */
    for (guint i = 0; i < future->entries->len; ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(future->entries, i);
        if (!future->routed_rotation) entry->run.preferred_deck =
            (gint)p->plan->options.deck_pool[(prefix + i) % p->plan->options.pool_count];
    }
    RadioSAudioRunItem *items = radios_playout_plan_items(future);
    gboolean result = radios_audio_engine_runner_replace_future(p->engine, p->player_deck,
        actual->epoch, audio.decks[actual->deck_current].generation, actual->entry_id, items, future->entries->len, error);
    g_free(items); if (!result) return FALSE;
    RadioSPlayoutEntry *current = g_ptr_array_index(p->plan->entries, actual->index);
    RadioSPlayoutEntry *first = g_ptr_array_index(future->entries, 0);
    current->run.mode = first->run.mode; current->run.overlap_frames = first->run.overlap_frames;
    current->run.effect = first->run.effect; current->run.shape = first->run.shape;
    JsonArray *rows = json_object_get_array_member(json_node_get_object(p->plan->display), "entries");
    JsonArray *added = json_object_get_array_member(json_node_get_object(future->display), "entries");
    while (json_array_get_length(rows) > prefix) json_array_remove_element(rows, prefix);
    while (p->plan->entries->len > prefix) g_ptr_array_remove_index(p->plan->entries, prefix);
    for (guint i = 0; added && i < json_array_get_length(added); ++i)
        json_array_add_element(rows, radios_programme_clone(json_array_get_element(added, i)));
    while (future->entries->len) g_ptr_array_add(p->plan->entries, g_ptr_array_steal_index(future->entries, 0));
    radios_playout_plan_refresh_jingle_metadata(p->plan);
    extension_clear(p); p->automatic = FALSE; g_clear_pointer(&p->automation_config, json_node_free);
    radios_playout_plan_free(future); return TRUE;
}
gboolean radios_playout_ui_replace_remaining_prepared(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared, guint64 expected_intent, GError **error)
{
    RadioSPlayoutController *p = controller(ctx);
    if (!p || !owner || gtk_widget_in_destruction(owner) || !radios_data_connected(ctx) || p->intent != expected_intent ||
        p->preparing || !p->plan || !prepared || g_strcmp0(radios_json_string(prepared, "kind", ""), "programme"))
        return invalid(error, "Yeni şema hazırlanırken yayın veya bağlantı değişti.");
    RadioSPlayoutRequest request = {.station_id = radios_data_station_id(ctx), .database_file = ctx->config->database_file,
        .now_utc_us = g_get_real_time(), .mode = RADIOS_AUDIO_RUN_EOF, .live = TRUE, .history = p->history,
        .single_player = p->player_deck >= 0 && !p->scoped};
    default_policy_snapshot(ctx, &request);
    RadioSPlayoutPlan *future = build_plan(ctx, prepared, &request, error); if (!future) return FALSE;
    RadioSAudioRunSnapshot actual; runner_snapshot(p, &actual);
    if (replace_remaining_commit(p, future, &actual, error)) { ++p->intent; return TRUE; }
    if (error && g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_BUSY)) {
        g_clear_error(error); radios_playout_plan_free(p->replacement_plan);
        p->replacement_plan = future; p->replacement_epoch = actual.epoch; ++p->intent; return TRUE;
    }
    radios_playout_plan_free(future); return FALSE;
}
gboolean radios_playout_ui_remove_future_entries(RadioSViewContext *ctx, const gchar *const *entry_ids, guint count, GError **error)
{
    RadioSPlayoutController *p = controller(ctx);
    if (!p || p->preparing || p->failed || !p->plan || !entry_ids || !count) return invalid(error, "Yayındaki sıra hazır değil.");
    if (p->scoped && !scoped_owns_run(p)) return invalid(error, "Bu AutoDJ kapsamı artık bu sıranın sahibi değil.");
    RadioSAudioRunSnapshot actual; runner_snapshot(p, &actual);
    if (actual.deck_current < 0 || actual.index >= p->plan->entries->len) return invalid(error, "Yayındaki sıra şu an değiştirilemiyor.");
    guint total = p->plan->entries->len, prefix = actual.index + 1, removed = 0;
    gboolean *drop = g_new0(gboolean, total);
    for (guint c = 0; c < count; ++c) {
        guint found = G_MAXUINT;
        for (guint i = 0; i < total && found == G_MAXUINT; ++i)
            if (g_strcmp0(((RadioSPlayoutEntry *)g_ptr_array_index(p->plan->entries, i))->run.entry_id, entry_ids[c]) == 0) found = i;
        if (found == G_MAXUINT) {
            g_free(drop); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "İçerik yayındaki sırada yok."); return FALSE;
        }
        if (found < prefix) {
            g_free(drop);
            return invalid(error, found + 1 == prefix ? "Çalan içerik kaldırılamaz; sonrakine geçmek için ⏭ kullanın." : "Bu içerik zaten çaldı.");
        }
        /* A live step hangs on the row before it (its live_step hint): taking
         * that row out would silently drop the step from the outer order. */
        RadioSPlayoutEntry *entry = g_ptr_array_index(p->plan->entries, found);
        if (entry->binding && json_object_has_member(entry->binding, "live_step")) {
            g_free(drop); return invalid(error, "Bu içeriğin ardında canlı adım var; kaldırılırsa adım da yayından çıkar.");
        }
        if (!drop[found]) { drop[found] = TRUE; ++removed; }
    }
    /* The sounding content keeps its own exit policy (a live step waiting
     * after it stays); an empty future ends the run after it. */
    guint keep = total - prefix - removed;
    RadioSAudioRunItem *all = radios_playout_plan_items(p->plan), *items = keep ? g_new(RadioSAudioRunItem, keep) : NULL;
    for (guint i = prefix, n = 0; i < total; ++i) {
        if (drop[i]) continue;
        items[n] = all[i];
        /* Each remaining row takes the player of the slot it moves into, so
         * the order's player pattern goes on from the sounding content (pool
         * alternation keeps every cluster player loaded, a semi-automatic
         * order stays on its one player). A routed rotation keeps each row on
         * the player its kind is routed to. */
        if (!p->plan->routed_rotation) items[n].preferred_deck = all[prefix + n].preferred_deck;
        ++n;
    }
    RadioSAudioSnapshot audio; radios_audio_engine_snapshot(p->engine, &audio);
    gboolean accepted = radios_audio_engine_runner_remove_future(p->engine, p->player_deck, actual.epoch,
        audio.decks[actual.deck_current].generation, actual.entry_id, items, keep, error);
    if (accepted) {
        JsonArray *rows = json_object_get_array_member(json_node_get_object(p->plan->display), "entries");
        for (guint i = total; i-- > prefix;) {
            if (!drop[i]) continue;
            g_ptr_array_remove_index(p->plan->entries, i);
            if (rows && i < json_array_get_length(rows)) json_array_remove_element(rows, i);
        }
        for (guint i = prefix; i < p->plan->entries->len; ++i)
            ((RadioSPlayoutEntry *)g_ptr_array_index(p->plan->entries, i))->run.preferred_deck = items[i - prefix].preferred_deck;
        radios_playout_plan_refresh_jingle_metadata(p->plan); ++p->intent;
    }
    g_free(all); g_free(items); g_free(drop); return accepted;
}
static gboolean replacement_ready(RadioSPlayoutController *p, RadioSPlayoutPlan *plan, GError **error)
{
    RadioSAudioSnapshot audio; RadioSAudioRunSnapshot actual;
    radios_audio_engine_snapshot(p->engine, &audio); runner_snapshot(p, &actual);
    guint available = 0;
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) {
        gboolean mapped = FALSE;
        for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
            mapped |= audio.mixer.channels[channel].source_deck == (gint)deck;
        if (mapped && audio.decks[deck].main_routed && audio.decks[deck].state == RADIOS_AUDIO_PLAYING && !actual.owned[deck])
            return invalid(error, "MAIN'de elle çalınan kaynak var; önce operatör bu kaynağı durdurmalı.");
        gboolean usable = mapped && audio.decks[deck].main_routed && (actual.owned[deck] || audio.decks[deck].state == RADIOS_AUDIO_EMPTY);
        if (usable) ++available;
        if (plan->automation) for (guint i = 0; i < plan->options.pool_count; ++i)
            if (plan->options.deck_pool[i] == deck && !usable) return invalid(error, "Normal yayın dönüş oynatıcısı elle kullanılıyor; mevcut kaynak korunuyor.");
    }
    gboolean overlap = FALSE;
    for (guint i = 0; i < plan->entries->len; ++i) overlap |= ((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, i))->run.overlap_frames != 0;
    return available >= (overlap ? 2u : 1u) || invalid(error, "Devralma için boş veya sıraya ait MAIN oynatıcısı gerekli; mevcut kaynak korunuyor.");
}
static gdouble source_output_frames(const RadioSAudioRunItem *item, guint64 source_frame)
{
    RadioSAudioTempoProfile profile = radios_audio_run_tempo_profile(item);
    guint64 elapsed = source_frame > item->media.cue_in_frame ? source_frame - item->media.cue_in_frame : 0;
    return radios_audio_tempo_profile_output(&profile, elapsed);
}
static gint64 extension_start(RadioSPlayoutController *p, Preparation *job, RadioSPlayoutPlan *plan)
{
    RadioSPlayoutEntry *last = g_ptr_array_index(p->plan->entries, p->plan->entries->len - 1);
    RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0);
    RadioSAudioRunItem boundary = last->run;
    boundary.overlap_frames = MIN(boundary.overlap_frames, radios_audio_run_incoming_span_frames(&first->run));
    gdouble ratio = boundary.tempo_ratio > 0 ? boundary.tempo_ratio : 1;
    guint64 trigger = radios_audio_run_trigger_frame(&boundary, &boundary.rhythm, ratio, boundary.media.cue_out_frame);
    guint64 elapsed = (guint64)ceil(source_output_frames(&boundary, trigger));
    return MAX(job->request.now_utc_us, job->boundary_start_utc_us + (gint64)(elapsed * G_USEC_PER_SEC / 48000));
}
static void append_fade_in(RadioSPlayoutController *p, RadioSPlayoutPlan *plan, gint64 expected_start)
{
    RadioSPlayoutEntry *last = g_ptr_array_index(p->plan->entries, p->plan->entries->len - 1);
    RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0);
    RadioSAudioRunItem boundary = last->run;
    boundary.overlap_frames = MIN(boundary.overlap_frames, radios_audio_run_incoming_span_frames(&first->run));
    gdouble ratio = boundary.tempo_ratio > 0 ? boundary.tempo_ratio : 1;
    guint64 trigger = radios_audio_run_trigger_frame(&boundary, &boundary.rhythm, ratio, boundary.media.cue_out_frame);
    guint64 remaining = (guint64)ceil(source_output_frames(&boundary, boundary.media.cue_out_frame) - source_output_frames(&boundary, trigger));
    guint64 delay = (guint64)MAX((gint64)0, first->planned_utc_us - expected_start) * 48000 / 1000000;
    remaining = remaining > delay ? remaining - delay : 0;
    guint64 incoming_span = (guint64)ceil(source_output_frames(&first->run, first->run.media.cue_out_frame));
    first->run.fade_in_frames = MIN(radios_audio_run_overlap_frames(&boundary, remaining),
        MIN(incoming_span, radios_audio_run_incoming_span_frames(&first->run)));
}
static void prepared(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *failure, gpointer value)
{
    Preparation *job = value; RadioSPlayoutController *p = job->playout;
    if (p->closed || (!job->preview && job->intent != p->intent)) return;
    GError *error = failure ? g_error_copy(failure) : NULL; RadioSPlayoutPlan *plan = NULL;
    if (!error && result && JSON_NODE_HOLDS_OBJECT(result)) {
        JsonObject *response = json_node_get_object(result);
        /* A continuation window carries the full programme twice (config and
         * programme.config); the duplicate is dropped before the plan clone. */
        if (job->programme_window) json_object_remove_member(response, "programme");
        JsonNode *history = json_object_get_member(response, "history");
        if (!job->preview && p->player_deck < 0 && !p->history_loaded && history && JSON_NODE_HOLDS_OBJECT(history)) {
            JsonObject *record = json_node_get_object(history);
            JsonObject *configuration = json_object_get_object_member(record, "config");
            radios_playout_history_import(p->history, json_object_get_array_member(configuration, "entries"));
            radios_playout_history_set_rotation(p->history, json_object_has_member(configuration, "rotation") ? json_object_get_object_member(configuration, "rotation") : NULL);
            p->history_loaded = TRUE;
        }
        job->request.now_utc_us = g_get_real_time();
        plan = build_plan(ctx, response, &job->request, &error);
    } else if (!error) invalid(&error, "Oynatma hazırlığı geçerli veri döndürmedi.");
    if (plan) preparation_loudness_origin(job, plan);
    if (job->preview) {
        if (job->callback) job->callback(ctx, owner, plan ? plan->display : NULL, error, job->user);
        radios_playout_plan_free(plan); g_clear_error(&error); return;
    }
    if (job->programme_window) {
        p->prefetching = FALSE;
        RadioSAudioRunSnapshot actual; runner_snapshot(p, &actual);
        if (actual.epoch != job->epoch || p->automatic || !p->plan || !p->programme_config) { radios_playout_plan_free(plan); g_clear_error(&error); return; }
        if (plan) {
            gint64 start = extension_start(p, job, plan);
            if (start != job->request.start_utc_us) {
                radios_playout_plan_free(plan); job->request.start_utc_us = start;
                plan = build_plan(ctx, json_node_get_object(result), &job->request, &error);
                if (plan && extension_start(p, job, plan) != start) {
                    radios_playout_plan_free(plan); plan = NULL;
                    g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_BUSY, "Program penceresinin başlangıcı değişti; yeniden hazırlanacak.");
                }
            }
        }
        if (plan) {
            preparation_loudness_origin(job, plan);
            p->extension_error[0] = 0; p->extension_failures = 0; p->extension_retry_at = 0;
            append_fade_in(p, plan, job->request.start_utc_us);
            route_adjuncts(p, plan, &p->plan->options);
            RadioSAudioRunItem *items = radios_playout_plan_items(plan);
            gboolean appended = p->plan->entries->len + plan->entries->len <= RADIOS_AUDIO_RUN_MAX_ITEMS &&
                radios_audio_engine_runner_append(p->engine, items, plan->entries->len, &error);
            g_free(items);
            if (appended) {
                accepted_adjunct_metadata(p, plan);
                /* Programme rows keep their own block identity; display and
                 * plan entries of the window join the accepted sequence. */
                JsonArray *accepted_rows = json_object_get_array_member(json_node_get_object(p->plan->display), "entries");
                JsonArray *added_rows = json_object_get_array_member(json_node_get_object(plan->display), "entries");
                for (guint row = 0; added_rows && row < json_array_get_length(added_rows); ++row)
                    json_array_add_element(accepted_rows, radios_programme_clone(json_array_get_element(added_rows, row)));
                while (plan->entries->len) g_ptr_array_add(p->plan->entries, g_ptr_array_steal_index(plan->entries, 0));
                radios_playout_plan_refresh_jingle_metadata(p->plan);
                p->programme_next_index = job->window_next; p->programme_window_size = job->window_size;
                radios_playout_plan_free(plan); g_clear_error(&error); return;
            }
            runner_snapshot(p, &actual);
            if (actual.state == RADIOS_AUDIO_RUN_FINISHED || p->plan->entries->len + plan->entries->len > RADIOS_AUDIO_RUN_MAX_ITEMS ||
                (error && error->domain == RADIOS_AUDIO_ERROR_DOMAIN && error->code == RADIOS_AUDIO_ERROR_STATE && actual.state != RADIOS_AUDIO_RUN_ERROR)) {
                p->pending_plan = plan; p->programme_next_index = job->window_next; p->programme_window_size = job->window_size;
                g_clear_error(&error); return;
            }
            radios_playout_plan_free(plan);
        }
        extension_failed(p, error); g_clear_error(&error); return;
    }
    if (job->append) {
        p->prefetching = FALSE;
        RadioSAudioRunSnapshot actual; runner_snapshot(p, &actual);
        if (actual.epoch != job->epoch || !p->automatic || !p->plan) { radios_playout_plan_free(plan); g_clear_error(&error); return; }
        if (plan) {
            /* The incoming cue was unknown when the request was sent. Rebuild
             * at the engine's capped boundary before validating source windows;
             * shifting display timestamps would leave eligibility unchecked. */
            gint64 start = extension_start(p, job, plan);
            if (start != job->request.start_utc_us) {
                radios_playout_plan_free(plan); job->request.start_utc_us = start;
                plan = build_plan(ctx, json_node_get_object(result), &job->request, &error);
                if (plan && extension_start(p, job, plan) != start) {
                    radios_playout_plan_free(plan); plan = NULL;
                    g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_BUSY,
                        "Sonraki AutoDJ aralığı değişti; yayın penceresi yeniden hazırlanacak.");
                }
            }
        }
        if (plan) {
            preparation_loudness_origin(job, plan);
            p->extension_error[0] = 0; p->extension_failures = 0; p->extension_retry_at = 0;
            append_fade_in(p, plan, job->request.start_utc_us);
            route_adjuncts(p, plan, &p->plan->options);
            RadioSAudioRunItem *items = radios_playout_plan_items(plan);
            gboolean appended = p->plan->entries->len + plan->entries->len <= RADIOS_AUDIO_RUN_MAX_ITEMS &&
                radios_audio_engine_runner_append(p->engine, items, plan->entries->len, &error);
            g_free(items);
            if (appended) {
                accepted_adjunct_metadata(p, plan);
                p->plan->music_count = plan->music_count;
                /* Refill occurrences retain their accepted event boundaries
                 * and jingle selections after the temporary plan is freed. */
                JsonArray *accepted_rows = json_object_get_array_member(json_node_get_object(p->plan->display), "entries");
                JsonArray *added_rows = json_object_get_array_member(json_node_get_object(plan->display), "entries");
                const gchar *continued_block = NULL, *incoming_block = NULL;
                if (p->plan->automation && plan->automation && json_array_get_length(accepted_rows) && json_array_get_length(added_rows)) {
                    JsonObject *last = json_array_get_object_element(accepted_rows, json_array_get_length(accepted_rows) - 1);
                    JsonObject *first = json_array_get_object_element(added_rows, 0);
                    if (!g_strcmp0(radios_json_string(last, "source_id", ""), radios_json_string(first, "source_id", "")) &&
                        !g_strcmp0(radios_json_string(last, "kind", ""), radios_json_string(first, "kind", ""))) {
                        continued_block = radios_json_string(last, "block_id", "");
                        incoming_block = radios_json_string(first, "block_id", "");
                    }
                }
                for (guint row = 0; added_rows && row < json_array_get_length(added_rows); ++row) {
                    JsonNode *copy = radios_programme_clone(json_array_get_element(added_rows, row));
                    JsonObject *display = json_node_get_object(copy);
                    if (continued_block && *continued_block && incoming_block && *incoming_block &&
                        g_str_equal(radios_json_string(display, "block_id", ""), incoming_block))
                        json_object_set_string_member(display, "block_id", continued_block);
                    json_array_add_element(accepted_rows, copy);
                }
                while (plan->entries->len) g_ptr_array_add(p->plan->entries, g_ptr_array_steal_index(plan->entries, 0));
                radios_playout_plan_refresh_jingle_metadata(p->plan);
                radios_playout_plan_free(plan); g_clear_error(&error); return;
            }
            runner_snapshot(p, &actual);
            if (actual.state == RADIOS_AUDIO_RUN_FINISHED || p->plan->entries->len + plan->entries->len > RADIOS_AUDIO_RUN_MAX_ITEMS ||
                (error && error->domain == RADIOS_AUDIO_ERROR_DOMAIN && error->code == RADIOS_AUDIO_ERROR_STATE && actual.state != RADIOS_AUDIO_RUN_ERROR)) {
                p->pending_plan = plan; g_clear_error(&error); return;
            }
            radios_playout_plan_free(plan);
        }
        extension_failed(p, error); g_clear_error(&error); return;
    }
    p->preparing = FALSE;
    if (plan && job->player_autodj) {
        RadioSAudioSnapshot audio; radios_audio_engine_snapshot(p->engine, &audio);
        RadioSAudioPlayerOptions options = {-1, -1};
        if (job->pair_selection_captured &&
            (!radios_audio_ui_player_options(ctx, job->pair[0], &options) || options.autodj_target != job->pair_selection)) {
            invalid(&error, "AutoDJ hazırlanırken hedef seçimi değişti; mevcut oynatıcılar korundu.");
            radios_playout_plan_free(plan); plan = NULL;
        }
        for (guint index = 0; plan && index < 2; ++index) if (audio.decks[job->pair[index]].generation != job->pair_generation[index]) {
            invalid(&error, "AutoDJ hazırlanırken kaynak veya hedef deck değişti; yeni yükleme korunuyor.");
            radios_playout_plan_free(plan); plan = NULL; break;
        }
    }
    if (plan && !install_plan(p, plan, &error)) { radios_playout_plan_free(plan); plan = NULL; }
    if (!plan) {
        p->automatic = FALSE; p->failed = TRUE; p->snapshot.state = RADIOS_PLAYOUT_ERROR;
        g_strlcpy(p->snapshot.error, error ? error->message : "Sıra başlatılamadı.", sizeof p->snapshot.error); notice(p, p->snapshot.error); g_clear_error(&error); return;
    }
    p->automatic = job->automatic;
    if (p->automation_config) json_node_free(p->automation_config);
    p->automation_config = job->automatic ? clone(job->config) : NULL;
    notice(p, plan->options.start_utc_us > g_get_real_time() ? "Sıra doğrulandı; UTC başlangıcını bekliyor." : "Doğrulanmış sıra C ses motorunda hazırlanıyor.");
    g_clear_error(&error);
}
static gboolean prepare_extension(RadioSPlayoutController *p, const RadioSAudioRunSnapshot *actual, GError **error)
{
    GtkWidget *owner = g_weak_ref_get(&p->owner);
    if (!owner || gtk_widget_in_destruction(owner)) {
        g_clear_object(&owner); p->automatic = FALSE; return TRUE;
    }
    Preparation *job = g_new0(Preparation, 1); job->playout = ref(p); job->automatic = job->append = TRUE;
    job->intent = p->intent; job->epoch = actual->epoch; job->config = clone(p->automation_config);
    job->loudness_context = p->plan->options.loudness_context;
    g_strlcpy(job->loudness_automation_id, p->plan->options.loudness_automation_id, sizeof job->loudness_automation_id);
    job->loudness_assigned = p->plan->options.loudness_assigned;
    job->history = radios_playout_history_copy(p->history);
    RadioSPlayoutEntry *last = g_ptr_array_index(p->plan->entries, p->plan->entries->len - 1);
    gdouble ratio = last->run.tempo_ratio > 0 ? last->run.tempo_ratio : 1;
    guint64 trigger_frame = radios_audio_run_trigger_frame(&last->run, &last->run.rhythm, ratio, last->run.media.cue_out_frame);
    guint64 trigger = (guint64)ceil(source_output_frames(&last->run, trigger_frame));
    gint64 shift = 0;
    if (actual->index < p->plan->entries->len && actual->actual_current_frame) {
        RadioSPlayoutEntry *current = g_ptr_array_index(p->plan->entries, actual->index);
        gdouble consumed = source_output_frames(&current->run, actual->actual_current_frame);
        shift = g_get_real_time() - current->planned_utc_us - (gint64)(ceil(consumed) * 1000000 / 48000);
    }
    for (guint i = 0; i < p->plan->entries->len; ++i) {
        gchar key[64]; g_snprintf(key, sizeof key, "entry:%" G_GUINT64_FORMAT ":%u", actual->epoch, i);
        RadioSPlayoutEntry *entry = g_ptr_array_index(p->plan->entries, i);
        if (!g_hash_table_contains(p->observed, key) && i >= actual->index)
            radios_playout_history_observe(job->history, entry->run.media.media_id, entry->run.media.version_id,
                entry->artist, entry->album, entry->planned_utc_us + shift, 1);
    }
    radios_playout_history_set_rotation(job->history, last->rotation_after);
    job->boundary_start_utc_us = last->planned_utc_us + shift;
    job->request = (RadioSPlayoutRequest){.station_id = radios_data_station_id(p->ctx), .database_file = p->ctx->config->database_file,
        .now_utc_us = g_get_real_time(), .start_utc_us = MAX(g_get_real_time(), last->planned_utc_us + shift + (gint64)(trigger * 1000000 / 48000)),
        .live = TRUE, .history = job->history};
    default_policy_snapshot(p->ctx, &job->request);
    JsonObject *config = json_node_get_object(p->automation_config);
    JsonArray *decks = json_object_get_array_member(config, "decks");
    guint count = radios_json_int(config, "deckMode", 0) == 1 ? 1 : json_array_get_length(decks);
    for (guint entry_index = p->plan->entries->len; entry_index > 0; --entry_index) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(p->plan->entries, entry_index - 1);
        gboolean found = FALSE;
        for (guint deck_index = 0; deck_index < count; ++deck_index)
            if ((gint)g_ascii_strtoll(json_array_get_string_element(decks, deck_index), NULL, 10) - 1 == entry->run.preferred_deck) {
                job->request.deck_phase = (deck_index + 1) % count; found = TRUE; break;
            }
        if (found) break;
    }
    JsonNode *body = radios_data_body(p->ctx); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "kind", "automation");
    json_object_set_object_member(o, "config", json_object_ref(json_node_get_object(job->config)));
    p->prefetching = TRUE;
    gboolean queued = radios_data_request(p->ctx, owner, "POST", "/v1/workflows/desktop-playout/prepare", body, prepared, job, preparation_free);
    json_node_free(body); g_object_unref(owner);
    if (!queued) { p->prefetching = FALSE; g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Sonraki rotasyon isteği gönderilemedi."); return FALSE; }
    return TRUE;
}
/* Kılavuz §06/§08 "çekme adedi": see playout_ui.h. Events are counted when
 * the owning block changes; the window never skips a content, so the queue
 * order is the programme order and the next window resumes at *next_index. */
gboolean radios_playout_programme_window_entries(JsonObject *config, guint start_index, guint event_fetch, guint content_fetch, JsonArray **ids, guint *next_index)
{
    if (ids) *ids = NULL;
    JsonNode *node = config ? json_object_get_member(config, "events") : NULL;
    JsonArray *events = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
    guint total = events ? json_array_get_length(events) : 0, index = MIN(start_index, total);
    if (next_index) *next_index = index;
    if (!ids || (!event_fetch && !content_fetch) || start_index >= total) return FALSE;
    RadioSProgrammeTree *tree = radios_programme_tree_new(config);
    GHashTable *owners = g_hash_table_new(g_str_hash, g_str_equal);
    guint main = 0;
    for (guint b = 0; b < tree->blocks->len; ++b) {
        RadioSProgrammeBlock *block = g_ptr_array_index(tree->blocks, b);
        if (!b || !radios_programme_kind_attaches(radios_json_string(block->block, "kind", ""))) main = b + 1;
        for (guint l = 0; l < block->leaves->len; ++l)
            g_hash_table_insert(owners, (gpointer)radios_json_string(g_ptr_array_index(block->leaves, l), "id", ""), GUINT_TO_POINTER(main));
    }
    JsonArray *result = json_array_new(); guint taken = 0, in_event = 0, current = 0;
    for (; index < total; ++index) {
        JsonNode *element = json_array_get_element(events, index);
        const gchar *id = JSON_NODE_HOLDS_OBJECT(element) ? radios_json_string(json_node_get_object(element), "id", "") : "";
        if (!*id) continue; /* a content without identity cannot be prepared by id */
        guint owner = GPOINTER_TO_UINT(g_hash_table_lookup(owners, id));
        if (!taken || owner != current) {
            if (event_fetch && taken == event_fetch) break;
            ++taken; current = owner; in_event = 0;
        }
        guint cost = radios_programme_leaf_content_count(json_node_get_object(element));
        if (json_array_get_length(result) && content_fetch && in_event + cost > content_fetch) break;
        if (json_array_get_length(result) == RADIOS_AUDIO_RUN_MAX_ITEMS) break;
        json_array_add_string_element(result, id); in_event += cost;
    }
    g_hash_table_unref(owners); radios_programme_tree_free(tree);
    if (next_index) *next_index = index;
    if (!json_array_get_length(result)) { json_array_unref(result); return FALSE; }
    *ids = result; return TRUE;
}
/* The ids a programme start prepares; NULL keeps the whole-programme request
 * (0/0 settings while the remainder fits one request of 500). */
static JsonArray *programme_window_ids(RadioSPlayoutController *p, JsonObject *config, guint start_index, guint *next)
{
    guint events = p->window_event_fetch, contents = p->window_content_fetch; JsonArray *ids = NULL;
    if (!events && !contents) {
        guint total = events_total(config);
        if (total - MIN(total, start_index) <= RADIOS_AUDIO_RUN_MAX_ITEMS) return NULL;
        events = G_MAXUINT; /* a long programme is windowed by the 500-item cap alone */
    }
    return radios_playout_programme_window_entries(config, start_index, events, contents, &ids, next) ? ids : NULL;
}
static gboolean prepare_programme_window(RadioSPlayoutController *p, const RadioSAudioRunSnapshot *actual, GError **error)
{
    GtkWidget *owner = g_weak_ref_get(&p->owner);
    if (!owner || gtk_widget_in_destruction(owner)) { g_clear_object(&owner); g_clear_pointer(&p->programme_config, json_node_free); return TRUE; }
    JsonObject *config = json_node_get_object(p->programme_config); JsonArray *ids = NULL; guint next = p->programme_next_index;
    guint events = p->window_event_fetch ? p->window_event_fetch : G_MAXUINT;
    if (!radios_playout_programme_window_entries(config, p->programme_next_index, events, p->window_content_fetch, &ids, &next)) {
        g_object_unref(owner); g_clear_pointer(&p->programme_config, json_node_free); return TRUE;
    }
    Preparation *job = g_new0(Preparation, 1); job->playout = ref(p); job->programme_window = TRUE;
    job->intent = p->intent; job->epoch = actual->epoch; job->window_next = next; job->window_size = json_array_get_length(ids);
    job->loudness_context = p->plan->options.loudness_context;
    g_strlcpy(job->loudness_automation_id, p->plan->options.loudness_automation_id, sizeof job->loudness_automation_id);
    job->loudness_assigned = p->plan->options.loudness_assigned;
    job->history = p->history_loaded ? radios_playout_history_copy(p->history) : NULL;
    /* The same boundary estimate as the rotation refill: the accepted last
     * item's trigger, shifted by the observed drift of the current item. */
    RadioSPlayoutEntry *last = g_ptr_array_index(p->plan->entries, p->plan->entries->len - 1);
    gdouble ratio = last->run.tempo_ratio > 0 ? last->run.tempo_ratio : 1;
    guint64 trigger_frame = radios_audio_run_trigger_frame(&last->run, &last->run.rhythm, ratio, last->run.media.cue_out_frame);
    guint64 trigger = (guint64)ceil(source_output_frames(&last->run, trigger_frame));
    gint64 shift = 0;
    if (actual->index < p->plan->entries->len && actual->actual_current_frame) {
        RadioSPlayoutEntry *current = g_ptr_array_index(p->plan->entries, actual->index);
        gdouble consumed = source_output_frames(&current->run, actual->actual_current_frame);
        shift = g_get_real_time() - current->planned_utc_us - (gint64)(ceil(consumed) * 1000000 / 48000);
    }
    job->boundary_start_utc_us = last->planned_utc_us + shift;
    job->request = (RadioSPlayoutRequest){.station_id = radios_data_station_id(p->ctx), .database_file = p->ctx->config->database_file,
        .mode = p->programme_mode, .overlap_seconds = p->programme_overlap, .now_utc_us = g_get_real_time(),
        .start_utc_us = MAX(g_get_real_time(), last->planned_utc_us + shift + (gint64)(trigger * 1000000 / 48000)),
        .live = TRUE, .history = job->history};
    default_policy_snapshot(p->ctx, &job->request);
    /* The run's own programme copy goes out by reference: no re-parse or
     * clone of the full programme per window. */
    JsonNode *body = radios_data_body(p->ctx); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "kind", "programme");
    json_object_set_object_member(o, "config", json_object_ref(config));
    json_object_set_array_member(o, "entry_ids", ids);
    p->prefetching = TRUE;
    gboolean queued = radios_data_request(p->ctx, owner, "POST", "/v1/workflows/desktop-playout/prepare", body, prepared, job, preparation_free);
    json_node_free(body); g_object_unref(owner);
    if (!queued) { p->prefetching = FALSE; g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Programın sonraki penceresi istenemedi."); return FALSE; }
    return TRUE;
}
static gboolean prepare_start_origin(RadioSViewContext *ctx, GtkWidget *owner, const gchar *kind, const gchar *playlist_id,
    JsonArray *entry_ids, JsonObject *config, guint start_index, guint mode, gdouble overlap, gint64 start_utc,
    gboolean preview, RadioSDataCallback callback, gpointer user, GDestroyNotify destroy,
    const gchar *automation_id, gboolean assigned, GError **error)
{
    if (!loudness_origin_valid(automation_id, assigned, error)) { if (destroy) destroy(user); return FALSE; }
    RadioSPlayoutController *p = controller(ctx);
    if (!p || !owner || gtk_widget_in_destruction(owner) || !radios_data_connected(ctx) || mode > 3 || !isfinite(overlap) || overlap < 0 || overlap > 30) {
        if (destroy) destroy(user);
        return invalid(error, "Sıra, medya bağlantısı veya ses motoru hazır değil.");
    }
    if (p->player_deck >= 0 && (g_str_equal(kind, "automation") || !radios_data_document_ready(p->history_document))) {
        if (destroy) destroy(user);
        return invalid(error, "Oynatıcı geçmişi henüz yüklenmedi veya genel AutoDJ komutu ayrı sıraya yönlendirilemez.");
    }
    if (p->player_deck >= 0 && (mode == 1 || overlap != 0 ||
        (config && json_object_has_member(config, "autodj_players")) ||
        (config && json_object_has_member(config, "queue_player") && radios_json_int(config, "queue_player", 0) != p->player_deck + 1))) {
        if (destroy) destroy(user);
        return invalid(error, "Oynatıcı kimliği veya tek oynatıcı geçiş ayarı uyuşmuyor; mevcut sıra korunuyor.");
    }
    if (p->player_deck >= 0) p->history_loaded = TRUE;
    Preparation *job = g_new0(Preparation, 1); job->playout = ref(p); job->preview = preview;
    job->automatic = g_str_equal(kind, "automation"); job->callback = callback; job->user = user; job->destroy = destroy;
    if (config && json_object_has_member(config, "autodj_players")) {
        JsonArray *pair = json_object_get_array_member(config, "autodj_players");
        if (pair && json_array_get_length(pair) == 2) {
            RadioSAudioSnapshot audio; radios_audio_engine_snapshot(p->engine, &audio); job->player_autodj = TRUE;
            for (guint index = 0; index < 2; ++index) {
                gint64 player = json_array_get_int_element(pair, index) - 1;
                if (player < 0 || player >= 32) { preparation_free(job); return invalid(error, "AutoDJ hedefi geçersiz."); }
                job->pair[index] = (guint)player; job->pair_generation[index] = audio.decks[player].generation;
            }
            RadioSAudioPlayerOptions options = {-1, -1};
            job->pair_selection_captured = radios_audio_ui_player_options(ctx, job->pair[0], &options);
            job->pair_selection = options.autodj_target;
        }
    }
    job->loudness_context = job->automatic ? RADIOS_AUDIO_LOUDNESS_AUTOMATION | RADIOS_AUDIO_LOUDNESS_AUTODJ :
        job->player_autodj ? RADIOS_AUDIO_LOUDNESS_AUTODJ : start_utc > 0 ? RADIOS_AUDIO_LOUDNESS_AUTOMATION : RADIOS_AUDIO_LOUDNESS_MANUAL;
    g_strlcpy(job->loudness_automation_id, automation_id ? automation_id : "", sizeof job->loudness_automation_id);
    job->loudness_assigned = assigned;
    job->request = (RadioSPlayoutRequest){.station_id = radios_data_station_id(ctx), .database_file = ctx->config->database_file,
        .mode = mode, .start_index = start_index, .overlap_seconds = overlap, .now_utc_us = g_get_real_time(), .start_utc_us = start_utc,
        .live = !preview};
    if (p->player_deck < 0) default_policy_snapshot(ctx, &job->request);
    else {
        /* A private queue owns one deck. Its explicit transport mode must
         * survive the shared policy attached to every prepare response. */
        job->request.has_default_policy = TRUE;
        job->request.default_mode = mode;
        job->request.default_overlap_seconds = 0;
        job->request.default_beat_sync = FALSE;
    }
    /* Kılavuz §06/§08: a windowed programme start prepares its first window
     * by id (the plan then starts at its first row); the remaining windows
     * are appended in the background by programme_poll. */
    JsonArray *window_ids = NULL; guint window_next = 0;
    if (!preview && !entry_ids && config && g_str_equal(kind, "programme")) {
        window_ids = programme_window_ids(p, config, start_index, &window_next);
        if (window_ids) { entry_ids = window_ids; job->request.start_index = 0; }
    }
    JsonNode *body = radios_data_body(ctx); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "kind", kind);
    if (playlist_id) json_object_set_string_member(o, "playlist_id", playlist_id);
    if (entry_ids) json_object_set_array_member(o, "entry_ids", json_array_ref(entry_ids));
    if (config) { job->config = clone_object(config); json_object_set_object_member(o, "config", json_object_ref(json_node_get_object(job->config))); }
    if (!preview) {
        runner_command(p,RADIOS_AUDIO_PLAYER_RUN_STOP, NULL); checkpoint_broadcast(p); checkpoint_advert(p); capture_history(p); extension_clear(p);
        ++p->intent; job->intent = p->intent; p->preparing = TRUE; p->automatic = p->failed = FALSE;
        owner_clear(p);
        g_weak_ref_set(&p->owner, owner); p->owner_handler = g_signal_connect(owner, "destroy", G_CALLBACK(owner_destroyed), p);
        p->snapshot = (RadioSPlayoutSnapshot){.state = RADIOS_PLAYOUT_PREPARING, .deck = -1, .next_deck = -1};
        if (window_ids && window_next < events_total(config)) {
            /* The job's immutable programme copy is shared, not copied again. */
            p->programme_config = json_node_new(JSON_NODE_OBJECT); json_node_set_object(p->programme_config, json_node_get_object(job->config));
            p->programme_next_index = window_next; p->programme_window_size = json_array_get_length(window_ids);
            p->programme_mode = mode; p->programme_overlap = overlap;
        }
    }
    job->history = p->history_loaded ? radios_playout_history_copy(p->history) : NULL;
    job->request.history = job->history;
    gboolean queued = radios_data_request(ctx, owner, "POST", "/v1/workflows/desktop-playout/prepare", body, prepared, job, preparation_free);
    json_node_free(body); if (window_ids) json_array_unref(window_ids);
    if (!queued && !preview) { p->preparing = FALSE; p->failed = TRUE; p->snapshot.state = RADIOS_PLAYOUT_ERROR; g_clear_pointer(&p->programme_config, json_node_free); }
    if (queued && !preview) radios_broadcast_queue_run_config(ctx, g_str_equal(kind, "programme") ? config : NULL);
    return queued;
}
static gboolean prepare_start(RadioSViewContext *ctx, GtkWidget *owner, const gchar *kind, const gchar *playlist_id,
    JsonArray *entry_ids, JsonObject *config, guint start_index, guint mode, gdouble overlap, gint64 start_utc,
    gboolean preview, RadioSDataCallback callback, gpointer user, GDestroyNotify destroy, GError **error)
{
    return prepare_start_origin(ctx, owner, kind, playlist_id, entry_ids, config, start_index, mode, overlap, start_utc,
        preview, callback, user, destroy, NULL, FALSE, error);
}
static RadioSPlayoutController *playout_new(RadioSViewContext *ctx, gint player_deck, GtkWidget *owner, gboolean scoped)
{
    if (!ctx || !ctx->audio) return NULL;
    RadioSPlayoutController *p = g_new0(RadioSPlayoutController, 1); p->references = 1; p->ctx = ctx;
    p->player_deck = player_deck; p->scoped = scoped;
    p->scoped_epochs = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
    p->engine = radios_audio_ui_engine_acquire(ctx); p->history = radios_playout_history_new();
    p->calendar_turns = json_object_new();
    g_weak_ref_init(&p->owner, NULL); p->observed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    p->snapshot = (RadioSPlayoutSnapshot){.state = RADIOS_PLAYOUT_IDLE, .deck = -1, .next_deck = -1};
    gchar *name = player_deck < 0 ? g_strdup("Yerel oynatma geçmişi") :
        g_strdup_printf(scoped ? "Oynatıcı %d AutoDJ geçmişi" : "Oynatıcı %d oynatma geçmişi",player_deck + 1);
    p->history_document = radios_data_document_new(ctx, owner, "playout_history", name, history_loaded, p); g_free(name);
    copyright_controllers = g_list_prepend(copyright_controllers, p);
    p->timer = g_timeout_add(50, poll, p); return p;
}
RadioSPlayoutController *radios_playout_ui_new(RadioSViewContext *ctx)
{ return ctx ? playout_new(ctx,-1,GTK_WIDGET(ctx->parent),FALSE) : NULL; }
RadioSPlayoutController *radios_playout_ui_new_player(RadioSViewContext *ctx, guint deck, GtkWidget *owner)
{ return ctx && deck < 32 && owner ? playout_new(ctx,(gint)deck,owner,FALSE) : NULL; }
RadioSPlayoutController *radios_playout_ui_new_scope(RadioSViewContext *ctx, guint source, GtkWidget *owner)
{ return ctx && source < 32 && owner ? playout_new(ctx,(gint)source,owner,TRUE) : NULL; }
void radios_playout_ui_close(RadioSPlayoutController *p)
{
    if (!p || p->closed) return;
    runner_command(p,RADIOS_AUDIO_PLAYER_RUN_STOP, NULL); checkpoint_broadcast(p); checkpoint_advert(p); capture_history(p); extension_clear(p);
    if (!radios_data_document_preserve_pending(p->history_document))
        notice(p, "Son çalma checkpoint'i doğrulanamadı; kurtarmada ölçülmeyen son kuyruk belirsiz kalır.");
    copyright_controllers = g_list_remove(copyright_controllers, p);
    p->closed = TRUE; ++p->intent; p->automatic = p->preparing = FALSE;
    if (p->timer) { g_source_remove(p->timer); p->timer = 0; }
    owner_clear(p);
}
void radios_playout_ui_unref(RadioSPlayoutController *p)
{
    if (!p || !g_atomic_int_dec_and_test(&p->references)) return;
    radios_playout_ui_close(p); radios_audio_engine_unref(p->engine); radios_playout_plan_free(p->plan);
    radios_playout_history_free(p->history); g_hash_table_unref(p->observed); g_hash_table_unref(p->scoped_epochs);
    json_object_unref(p->calendar_turns);
    g_clear_pointer(&p->probability_checkpoint, json_node_free); g_clear_pointer(&p->policy_selection_checkpoint, json_node_free);
    g_weak_ref_clear(&p->owner);
    if (p->automation_config) json_node_free(p->automation_config);
    g_free(p);
}
gboolean radios_playout_ui_start_prepared_scope_with_loudness_origin(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared, const guint *decks, guint count, RadioSAutoDJMode mode,
    const gchar *automation_id, gboolean assigned, GError **error)
{
    if (!loudness_origin_valid(automation_id, assigned, error)) return FALSE;
    RadioSPlayoutController *p = controller(ctx);
    if (!p || !p->scoped || !GTK_IS_WIDGET(owner) || gtk_widget_in_destruction(owner) ||
        !prepared || !decks || !count || count > 32 || decks[0] != (guint)p->player_deck ||
        (mode != RADIOS_AUTODJ_SEMI && mode != RADIOS_AUTODJ_TRANSITIONS) ||
        (mode == RADIOS_AUTODJ_SEMI && count != 1) || (mode == RADIOS_AUTODJ_TRANSITIONS && count < 2))
        return invalid(error, "AutoDJ kapsamı veya çalışma düzeyi geçersiz.");
    guint32 mask = 0;
    for (guint i = 0; i < count; ++i) {
        if (decks[i] >= 32 || (mask & ((guint32)1 << decks[i])) ||
            (ctx->players && (!radios_player_workspace_contains(ctx->players, decks[i]) ||
             !radios_player_topology_same_group(radios_player_workspace_topology(ctx->players), decks[0], decks[i]))))
            return invalid(error, "AutoDJ oynatıcıları tanımlı, farklı ve aynı kapsamda olmalı.");
        mask |= (guint32)1 << decks[i];
    }
    if (!radios_playout_ui_ready(ctx)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "AutoDJ kapsamının geçmişi henüz yüklenmedi.");
        return FALSE;
    }
    RadioSAudioRunSnapshot run;
    radios_audio_engine_player_runner_snapshot(p->engine, (guint)p->player_deck, &run);
    if (p->preparing || run.state == RADIOS_AUDIO_RUN_PLAYING || run.state == RADIOS_AUDIO_RUN_OVERLAP ||
        run.state == RADIOS_AUDIO_RUN_PAUSED || run.state == RADIOS_AUDIO_RUN_SCHEDULED ||
        run.state == RADIOS_AUDIO_RUN_PREROLL || run.state == RADIOS_AUDIO_RUN_WAITING_MEDIA)
        return invalid(error, "Bu kapsamın sırası çalışıyor; önce durdurun.");
    JsonNode *copy = clone_object(prepared); JsonObject *response = json_node_get_object(copy);
    JsonNode *config_node = json_object_get_member(response, "config");
    JsonObject *config = config_node && JSON_NODE_HOLDS_OBJECT(config_node) ? json_node_get_object(config_node) : NULL;
    if (!config) { json_node_free(copy); return invalid(error, "Hazırlanmış yayın sırası ayarları eksik."); }
    json_object_remove_member(config, "queue_player"); json_object_remove_member(config, "queue_transition");
    json_object_remove_member(config, "queue_overlap"); json_object_remove_member(config, "autodj_players");
    json_object_remove_member(config, "beat_sync");
    RadioSPlayoutRequest request = {.station_id = radios_data_station_id(ctx), .database_file = ctx->config->database_file,
        .now_utc_us = g_get_real_time(), .mode = RADIOS_AUDIO_RUN_EOF, .live = TRUE, .history = p->history,
        .allow_single_routed = TRUE,
        /* The selection was made before the prepare round trip; trailing
         * items pushed past the block end are dropped and refilled later. */
        .trim_calendar_end = TRUE,
        .single_player = mode == RADIOS_AUTODJ_SEMI};
    if (mode == RADIOS_AUTODJ_TRANSITIONS) {
        default_policy_snapshot(ctx, &request);
        /* A station without a saved rule uses the default that the Otomasyon
         * selector and Ayarlar › Auto DJ show (Crossfade · 3 sn), not a hard cut. */
        if (!request.has_default_policy && radios_broadcast_transition_policy_ready(ctx)) {
            request.has_default_policy = TRUE; request.default_mode = RADIOS_AUDIO_RUN_CROSSFADE;
            request.default_overlap_seconds = 3; request.default_beat_sync = FALSE;
        }
    }
    RadioSPlayoutPlan *plan = build_plan(ctx, response, &request, error); json_node_free(copy);
    if (!plan) {
        /* As in the queue start path: a genuine timing failure of the
         * selection is retried by the scope runner instead of ending the schedule. */
        calendar_timing_retry(error); return FALSE;
    }
    plan->player_autodj = TRUE; plan->options.pool_count = count;
    memcpy(plan->options.deck_pool, decks, count * sizeof *decks);
    /* Kılavuz §03 "kümeden kümeye" and the user's report "AutoDJ kanallarını
     * seçtiğimde etki etmiyor": a kind the schema routes to defined unclustered
     * players outside this scope (jingles on players 1–2 while the cluster
     * plays music) plays there while they are free; a busy or foreign player
     * gives its rows back to this scope's own players instead of refusing. */
    guint32 pooled = mask, refused = 0;
    RadioSAudioSnapshot audio; radios_audio_engine_snapshot(p->engine, &audio);
    const RadioSPlayerTopology *topology = ctx->players ? radios_player_workspace_topology(ctx->players) : NULL;
    for (guint i = 0; i < plan->entries->len; ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i);
        if (plan->routed_rotation && mode == RADIOS_AUTODJ_TRANSITIONS && entry->run.preferred_deck >= 0 &&
            ((guint)entry->run.preferred_deck >= 32 || !(mask & ((guint32)1 << entry->run.preferred_deck)))) {
            guint deck = (guint)entry->run.preferred_deck; guint32 bit = deck < 32 ? (guint32)1 << deck : 0;
            gboolean usable = bit && !(refused & bit);
            if (usable && !(pooled & bit)) {
                gint owner = -1;
                usable = topology && radios_player_workspace_contains(ctx->players, deck) && !topology->players[deck].cluster_id[0] &&
                    (!radios_audio_engine_deck_runner_owner(p->engine, deck, &owner) || owner == p->player_deck) &&
                    audio.decks[deck].state != RADIOS_AUDIO_PLAYING && plan->options.pool_count < 32;
                if (usable) { plan->options.deck_pool[plan->options.pool_count++] = deck; pooled |= bit; }
                else {
                    refused |= bit;
                    gchar *message = g_strdup_printf("Oynatıcı %u şu an kullanılamıyor; şemanın o oynatıcıya ayırdığı içerikler kümenin oynatıcılarında çalıyor.", deck + 1);
                    notice(p, message); g_free(message);
                }
            }
            if (!usable) {
                /* The scope's own player that neither neighbour holds. */
                RadioSPlayoutEntry *previous = i ? g_ptr_array_index(plan->entries, i - 1) : NULL;
                RadioSPlayoutEntry *next = i + 1 < plan->entries->len ? g_ptr_array_index(plan->entries, i + 1) : NULL;
                gint chosen = -1;
                for (guint pass = 0; pass < 2 && chosen < 0; ++pass)
                    for (guint j = 0; j < count && chosen < 0; ++j) {
                        gint candidate = (gint)decks[(i + j) % count];
                        if ((!previous || previous->run.preferred_deck != candidate) && (pass || !next || next->run.preferred_deck != candidate)) chosen = candidate;
                    }
                entry->run.preferred_deck = chosen >= 0 ? chosen : (gint)decks[0];
            }
        }
        if (!plan->routed_rotation) entry->run.preferred_deck = (gint)decks[i % count];
        if (mode == RADIOS_AUTODJ_SEMI) {
            entry->run.preferred_deck = (gint)decks[0];
        }
    }
    plan_loudness_origin(plan, RADIOS_AUDIO_LOUDNESS_AUTODJ |
        (automation_id && *automation_id ? RADIOS_AUDIO_LOUDNESS_AUTOMATION : 0), automation_id, assigned);
    if (!install_plan(p, plan, error)) { radios_playout_plan_free(plan); return FALSE; }
    owner_clear(p); g_weak_ref_set(&p->owner, owner);
    p->owner_handler = g_signal_connect(owner, "destroy", G_CALLBACK(owner_destroyed), p);
    ++p->intent; p->failed = p->automatic = FALSE;
    return TRUE;
}

gboolean radios_playout_ui_start_prepared_scope(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared, const guint *decks, guint count, RadioSAutoDJMode mode, GError **error)
{ return radios_playout_ui_start_prepared_scope_with_loudness_origin(ctx, owner, prepared, decks, count, mode, NULL, FALSE, error); }

gboolean radios_playout_ui_start_player_autodj(RadioSViewContext *ctx, GtkWidget *owner, JsonArray *items, guint source, guint target, GError **error)
{
    RadioSPlayoutController *playout = controller(ctx);
    if (!playout || playout->player_deck >= 0 || source >= 32 || target >= 32 || source == target || !items || json_array_get_length(items) < 2)
        return invalid(error, "AutoDJ için farklı iki oynatıcı ve en az iki doğrulanmış yerel parça seçin.");
    RadioSAudioRunSnapshot run; runner_snapshot(playout, &run);
    for (guint deck = 0; deck < 32; ++deck) if (run.owned[deck]) return invalid(error, "Genel yayın deckleri henüz kullanımda; mevcut ses korunuyor.");
    if (playout->preparing || run.state == RADIOS_AUDIO_RUN_PLAYING || run.state == RADIOS_AUDIO_RUN_OVERLAP || run.state == RADIOS_AUDIO_RUN_PAUSED ||
        run.state == RADIOS_AUDIO_RUN_SCHEDULED || run.state == RADIOS_AUDIO_RUN_WAITING_MEDIA || run.state == RADIOS_AUDIO_RUN_PREROLL)
        return invalid(error, "Genel AutoDJ / yayın sırası kullanımda; mevcut yayın korunuyor.");
    JsonObject *config = json_object_new(); gchar *name = g_strdup_printf("Oynatıcı %u → %u AutoDJ", source + 1, target + 1);
    json_object_set_string_member(config, "title", name); g_free(name); json_object_set_string_member(config, "note", "");
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d"); g_date_time_unref(now);
    json_object_set_string_member(config, "date", date); g_free(date);
    JsonArray *events = json_array_new(), *pair = json_array_new(); json_object_set_array_member(config, "events", events);
    json_array_add_int_element(pair, source + 1); json_array_add_int_element(pair, target + 1); json_object_set_array_member(config, "autodj_players", pair);
    json_object_set_boolean_member(config, "beat_sync", TRUE);
    for (guint index = 0; index < json_array_get_length(items); ++index) {
        JsonObject *input = json_array_get_object_element(items, index);
        const gchar *binding[] = {"media_id", "version_id", "media_file_id", "media_sha256"};
        if (!g_uuid_string_is_valid(radios_json_string(input, "media_id", ""))) { json_object_unref(config); return invalid(error, "AutoDJ SYNC yalnız doğrulanmış yerel medya kullanır; çevrimiçi PCM deck analizi değildir."); }
        JsonObject *event = json_object_new(); gchar *id = g_uuid_string_random(); json_object_set_string_member(event, "id", id); g_free(id);
        const gchar *metadata[] = {"title", "artist", "album"};
        for (guint field = 0; field < G_N_ELEMENTS(metadata); ++field) json_object_set_string_member(event, metadata[field], radios_json_string(input, metadata[field], ""));
        for (guint field = 0; field < G_N_ELEMENTS(binding); ++field) json_object_set_string_member(event, binding[field], radios_json_string(input, binding[field], ""));
        json_object_set_string_member(event, "kind", "music"); json_object_set_int_member(event, "duration_ms", radios_json_int(input, "duration_ms", 0));
        const gchar *empty[] = {"start_time", "version_label", "primary_file", "backup_file", "note"};
        for (guint field = 0; field < G_N_ELEMENTS(empty); ++field) json_object_set_string_member(event, empty[field], "");
        json_array_add_object_element(events, event);
    }
    gboolean accepted = radios_playout_ui_start_programme(ctx, owner, config, 0, RADIOS_AUDIO_RUN_CROSSFADE, 2, error);
    json_object_unref(config); return accepted;
}
static RadioSPlayoutPlan *prepared_rotation_plan(RadioSViewContext *ctx, JsonObject *response,
    guint source, guint target, GError **error)
{
    if (!ctx || !ctx->config || !response || source >= 32 ||
        !g_str_equal(radios_json_string(response, "kind", ""), "programme")) {
        invalid(error, "Yüklü rotasyon ve kaynak player hazır olmalı."); return NULL;
    }
    JsonNode *copy = clone_object(response); JsonObject *prepared = json_node_get_object(copy);
    JsonNode *node = json_object_get_member(prepared, "config");
    JsonObject *config = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    node = json_object_get_member(prepared, "tracks"); JsonArray *tracks = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
    if (!config || !tracks || json_array_get_length(tracks) < 2) {
        json_node_free(copy); invalid(error, "Rotasyon en az iki doğrulanmış parça içermeli."); return NULL;
    }
    gboolean routed = FALSE;
    for (guint i = 0; i < json_array_get_length(tracks); ++i) {
        node = json_array_get_element(tracks, i);
        routed |= JSON_NODE_HOLDS_OBJECT(node) && json_object_has_member(json_node_get_object(node), "calendar_routing");
    }
    if (routed) json_object_remove_member(config, "autodj_players");
    else {
        if (target >= 32 || source == target) {
            json_node_free(copy); invalid(error, "Rotasyon için farklı iki player veya kayıtlı içerik/player dağılımı seçin."); return NULL;
        }
        JsonArray *players = json_array_new(); json_array_add_int_element(players, source + 1); json_array_add_int_element(players, target + 1);
        json_object_set_array_member(config, "autodj_players", players);
    }
    json_object_set_boolean_member(config, "beat_sync", FALSE);
    RadioSPlayoutController *p = controller(ctx);
    RadioSPlayoutRequest request = {.station_id = radios_data_station_id(ctx), .database_file = ctx->config->database_file,
        .now_utc_us = g_get_real_time(), .mode = RADIOS_AUDIO_RUN_EOF, .live = TRUE, .trim_calendar_end = TRUE,
        .history = p ? p->history : NULL};
    default_policy_snapshot(ctx, &request);
    RadioSPlayoutPlan *plan = build_plan(ctx, prepared, &request, error); json_node_free(copy); return plan;
}
gboolean radios_playout_prepared_rotation_pool(RadioSViewContext *ctx, JsonObject *prepared,
    guint source, guint target, guint decks[32], guint *count, gboolean *routed, GError **error)
{
    if (count) *count = 0;
    if (routed) *routed = FALSE;
    if (!decks || !count) return invalid(error, "Player dağılımı sonucu eksik.");
    RadioSPlayoutPlan *plan = prepared_rotation_plan(ctx, prepared, source, target, error);
    if (!plan) return FALSE;
    *count = plan->options.pool_count; memcpy(decks, plan->options.deck_pool, *count * sizeof(guint));
    if (routed) *routed = plan->routed_rotation;
    radios_playout_plan_free(plan); return TRUE;
}

gboolean radios_playout_ui_start_prepared_player_rotation(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared_response, guint source, guint target, GError **error)
{
    RadioSPlayoutController *p = controller(ctx);
    if (!p || p->player_deck >= 0 || !owner || gtk_widget_in_destruction(owner) ||
        !radios_data_connected(ctx) || !prepared_response || source >= 32 ||
        !g_str_equal(radios_json_string(prepared_response, "kind", ""), "programme"))
        return invalid(error, "Yüklü rotasyon ve farklı iki oynatıcı hazır olmalı.");
    RadioSAudioRunSnapshot actual; runner_snapshot(p, &actual);
    for (guint deck = 0; deck < 32; ++deck)
        if (actual.owned[deck]) return invalid(error, "Genel yayın deckleri kullanımda; mevcut ses korunuyor.");
    if (p->preparing || actual.state == RADIOS_AUDIO_RUN_PLAYING || actual.state == RADIOS_AUDIO_RUN_OVERLAP ||
        actual.state == RADIOS_AUDIO_RUN_PAUSED || actual.state == RADIOS_AUDIO_RUN_SCHEDULED ||
        actual.state == RADIOS_AUDIO_RUN_WAITING_MEDIA || actual.state == RADIOS_AUDIO_RUN_PREROLL)
        return invalid(error, "Genel yayın sırası kullanımda; yüklü rotasyon başlatılmadı.");
    RadioSPlayoutPlan *plan = prepared_rotation_plan(ctx, prepared_response, source, target, error);
    if (!plan) return FALSE;
    /* Fresh authenticated rotation loader binds this local-only provenance. */
    const gchar *automation_id = radios_json_string(prepared_response, "loudness_automation_document_id", "");
    if (!loudness_origin_valid(automation_id, FALSE, error)) { radios_playout_plan_free(plan); return FALSE; }
    plan_loudness_origin(plan, RADIOS_AUDIO_LOUDNESS_AUTOMATION | RADIOS_AUDIO_LOUDNESS_AUTODJ, automation_id, FALSE);
    /* Resolve the exact source/target pool before replacing controller state.
     * Third-party manual players and independent queues keep their ownership. */
    if (!choose_pool(p, plan, error)) { radios_playout_plan_free(plan); return FALSE; }
    checkpoint_broadcast(p); checkpoint_advert(p); capture_history(p); extension_clear(p);
    ++p->intent; p->preparing = p->automatic = p->failed = FALSE; owner_clear(p);
    g_clear_pointer(&p->automation_config, json_node_free);
    g_weak_ref_set(&p->owner, owner); p->owner_handler = g_signal_connect(owner, "destroy", G_CALLBACK(owner_destroyed), p);
    if (!install_plan(p, plan, error)) {
        radios_playout_plan_free(plan); p->failed = TRUE; p->snapshot.state = RADIOS_PLAYOUT_ERROR;
        g_strlcpy(p->snapshot.error, error && *error ? (*error)->message : "Rotasyon başlatılamadı.", sizeof p->snapshot.error);
        notice(p, p->snapshot.error); return FALSE;
    }
    radios_broadcast_queue_run_config(ctx, NULL);
    notice(p, plan->routed_rotation ? "Yüklü rotasyon kayıtlı player dağılımıyla başlatıldı." :
        "Yüklü rotasyon seçili oynatıcı çiftinde, kendi geçiş ayarlarıyla başlatıldı."); return TRUE;
}
guint64 radios_playout_ui_intent(RadioSViewContext *ctx)
{ RadioSPlayoutController *p = controller(ctx); return p ? p->intent : 0; }
void radios_playout_ui_resume_free(RadioSPlayoutResume *resume)
{
    if (!resume) return;
    radios_playout_plan_free(resume->plan);
    g_clear_pointer(&resume->automation_config, json_node_free);
    g_clear_pointer(&resume->programme_config, json_node_free); g_free(resume);
}
gboolean radios_playout_ui_interrupt_prepared(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *response, RadioSPlayoutResume **out, GError **error)
{
    RadioSPlayoutController *p = controller(ctx);
    if (!p || !owner || !out || *out || !radios_data_connected(ctx) || gtk_widget_in_destruction(owner))
        return invalid(error, "Acil yürütücüsü veya bağlantı hazır değil.");
    RadioSPlayoutRequest request = {.station_id = radios_data_station_id(ctx), .database_file = ctx->config->database_file,
        .mode = RADIOS_AUDIO_RUN_EOF, .now_utc_us = g_get_real_time(), .live = TRUE, .history = p->history};
    default_policy_snapshot(ctx, &request);
    RadioSPlayoutPlan *emergency = build_plan(ctx, response, &request, error);
    if (!emergency) return FALSE; /* Reject invalid media before touching live PCM. */
    if (!replacement_ready(p, emergency, error)) { radios_playout_plan_free(emergency); return FALSE; }
    RadioSAudioRunSnapshot actual; runner_snapshot(p, &actual);
    RadioSPlayoutResume *resume = g_new0(RadioSPlayoutResume, 1);
    resume->paused = actual.state == RADIOS_AUDIO_RUN_PAUSED;
    resume->automatic = p->automatic;
    resume->automation_config = p->automation_config ? clone(p->automation_config) : NULL;
    if (p->programme_config) {
        resume->programme_config = radios_programme_clone(p->programme_config); resume->programme_next_index = p->programme_next_index;
        resume->programme_window_size = p->programme_window_size; resume->programme_mode = p->programme_mode; resume->programme_overlap = p->programme_overlap;
    }
    if (p->plan) {
        g_strlcpy(resume->loudness_automation_id, p->plan->options.loudness_automation_id, sizeof resume->loudness_automation_id);
        resume->loudness_assigned = p->plan->options.loudness_assigned;
    }
    if (!runner_command(p,RADIOS_AUDIO_PLAYER_RUN_STOP, error)) {
        radios_playout_plan_free(emergency); radios_playout_ui_resume_free(resume); return FALSE;
    }
    radios_advert_ui_checkpoint(ctx); capture_history(p);
    if ((p->automatic || p->programme_config) && actual.state == RADIOS_AUDIO_RUN_FINISHED && p->pending_plan) {
        resume->plan = p->pending_plan; p->pending_plan = NULL;
    }
    extension_clear(p);
    if (p->plan && actual.index < p->plan->entries->len && actual.state != RADIOS_AUDIO_RUN_FINISHED && actual.state != RADIOS_AUDIO_RUN_IDLE && actual.state != RADIOS_AUDIO_RUN_ERROR) {
        resume->plan = p->plan; p->plan = NULL;
        guint index = actual.index;
        guint64 cursor = actual.actual_current_frame;
        if (actual.next_index > actual.index && actual.next_index < resume->plan->entries->len && actual.actual_next_frame >
            ((RadioSPlayoutEntry *)g_ptr_array_index(resume->plan->entries, actual.next_index))->run.media.cue_in_frame) {
            index = actual.next_index; cursor = actual.actual_next_frame;
        }
        RadioSAudioRunReceipt receipts[RADIOS_AUDIO_RUN_MAX_RECEIPTS];
        guint n = runner_receipts(p, receipts, G_N_ELEMENTS(receipts)); guint64 occurrence = 0;
        for (guint i = 0; i < n; ++i) if (receipts[i].epoch == actual.epoch && receipts[i].index < resume->plan->entries->len && receipts[i].occurrence >= occurrence &&
            receipts[i].position_frames > ((RadioSPlayoutEntry *)g_ptr_array_index(resume->plan->entries, receipts[i].index))->run.media.cue_in_frame) {
            occurrence = receipts[i].occurrence;
            if (index != receipts[i].index) { index = receipts[i].index; cursor = receipts[i].position_frames; }
            else cursor = MAX(cursor, receipts[i].position_frames);
        }
        for (guint i = 0; i < index; ++i) g_ptr_array_remove_index(resume->plan->entries, 0);
        RadioSPlayoutEntry *first = g_ptr_array_index(resume->plan->entries, 0);
        cursor = MAX(first->run.media.cue_in_frame, cursor);
        if (cursor >= first->run.media.cue_out_frame) g_ptr_array_remove_index(resume->plan->entries, 0);
        else {
            /* A checkpoint becomes a new source origin. Keep the remaining
             * linear tempo return, rather than restarting its incoming hold. */
            RadioSAudioTempoProfile profile = radios_audio_run_tempo_profile(&first->run);
            gdouble elapsed = source_output_frames(&first->run, cursor);
            first->run.tempo_ratio = radios_audio_tempo_profile_ratio(&profile, elapsed);
            first->run.tempo_return_start_frames = elapsed < profile.hold_frames ? (guint64)ceil(profile.hold_frames - elapsed) : 0;
            first->run.tempo_return_frames = profile.return_frames && elapsed < profile.hold_frames + profile.return_frames ?
                (guint64)ceil(profile.return_frames - MAX(0.0, elapsed - profile.hold_frames)) : 0;
            first->run.media.cue_in_frame = cursor;
            if (first->run.autodj_out_frame && first->run.autodj_out_frame <= cursor) first->run.autodj_out_frame = 0;
            first->run.auto_next_frame = MAX(first->run.auto_next_frame, cursor);
            first->run.fade_start_frame = MAX(first->run.fade_start_frame, cursor);
            first->run.fade_end_frame = MAX(first->run.fade_end_frame, first->run.fade_start_frame);
            first->run.fade_in_frames = 240; /* Five millisecond return fade. */
            first->run.overlap_frames = MIN(first->run.overlap_frames, first->run.media.cue_out_frame - cursor);
        }
    }
    if (resume->plan) {
        resume->plan->options.start_utc_us = resume->plan->options.end_utc_us = 0;
        for (guint i = 0; i < resume->plan->entries->len; ++i) {
            RadioSPlayoutEntry *entry = g_ptr_array_index(resume->plan->entries, i);
            entry->run.start_utc_us = entry->run.latest_start_utc_us = 0;
        }
    }
    ++p->intent; p->automatic = p->preparing = p->failed = FALSE; owner_clear(p);
    g_clear_pointer(&p->automation_config, json_node_free);
    g_weak_ref_set(&p->owner, owner); p->owner_handler = g_signal_connect(owner, "destroy", G_CALLBACK(owner_destroyed), p);
    *out = resume;
    if (!install_plan(p, emergency, error)) { radios_playout_plan_free(emergency); p->failed = TRUE; return FALSE; }
    return TRUE;
}
gboolean radios_playout_ui_repeat_completed(RadioSViewContext *ctx,
    guint64 expected_epoch, GError **error)
{
    RadioSPlayoutController *p = controller(ctx);
    if (!p || p->player_deck >= 0 || !p->plan || p->automatic || p->preparing || p->failed)
        return invalid(error, "Tekrarlanacak tamamlanmış sıra hazır değil.");
    RadioSAudioRunSnapshot actual; runner_snapshot(p, &actual);
    if (actual.epoch != expected_epoch || actual.state != RADIOS_AUDIO_RUN_FINISHED)
        return invalid(error, "Sıra değişti veya henüz tamamlanmadı; tekrar başlatılmadı.");
    for (guint i = 0; i < RADIOS_AUDIO_NORMAL_DECKS; ++i)
        if (actual.owned[i]) return invalid(error, "Önceki ses henüz çıkıştan tüketilmedi.");
    GtkWidget *owner = g_weak_ref_get(&p->owner);
    gboolean ready = owner && !gtk_widget_in_destruction(owner);
    g_clear_object(&owner);
    if (!ready) return invalid(error, "Sıranın çalışma sahibi kapandı.");
    capture_history(p);
    prune_observed(p);
    return runner_command(p, RADIOS_AUDIO_PLAYER_RUN_START, error);
}
gboolean radios_playout_ui_restore(RadioSViewContext *ctx, GtkWidget *owner,
    RadioSPlayoutResume *resume, GError **error)
{
    RadioSPlayoutController *p = controller(ctx);
    if (!p || !owner || !resume || !radios_data_connected(ctx)) return invalid(error, "Normal yayın dönüş bağlamı hazır değil.");
    if (resume->plan && resume->plan->entries->len && !replacement_ready(p, resume->plan, error)) return FALSE;
    if (!radios_playout_ui_action(ctx, RADIOS_PLAYOUT_STOP, error)) return FALSE;
    if (!resume->plan || !resume->plan->entries->len) {
        if (resume->automatic && resume->automation_config)
            return radios_playout_ui_start_automation_with_loudness_origin(ctx, owner, json_node_get_object(resume->automation_config),
                resume->loudness_automation_id, resume->loudness_assigned, error);
        /* An interrupted windowed programme continues from its next window. */
        if (resume->programme_config && resume->programme_next_index < events_total(json_node_get_object(resume->programme_config)))
            return radios_playout_ui_start_programme(ctx, owner, json_node_get_object(resume->programme_config),
                resume->programme_next_index, resume->programme_mode, resume->programme_overlap, error);
        return TRUE;
    }
    g_weak_ref_set(&p->owner, owner); p->owner_handler = g_signal_connect(owner, "destroy", G_CALLBACK(owner_destroyed), p);
    if (!install_plan(p, resume->plan, error)) return FALSE;
    resume->plan = NULL; p->automatic = resume->automatic;
    p->automation_config = resume->automation_config; resume->automation_config = NULL;
    p->programme_config = resume->programme_config; resume->programme_config = NULL;
    p->programme_next_index = resume->programme_next_index; p->programme_window_size = resume->programme_window_size;
    p->programme_mode = resume->programme_mode; p->programme_overlap = resume->programme_overlap;
    if (resume->paused) runner_command(p,RADIOS_AUDIO_PLAYER_RUN_PAUSE, NULL);
    return TRUE;
}
gboolean radios_playout_ui_start_prepared_with_loudness_origin(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared_response, gint64 start_utc_us, const gchar *automation_id, gboolean assigned, GError **error)
{
    if (!loudness_origin_valid(automation_id, assigned, error)) return FALSE;
    RadioSPlayoutController *p = controller(ctx);
    if (!p || !owner || gtk_widget_in_destruction(owner) || !radios_data_connected(ctx) ||
        !prepared_response || !g_str_equal(radios_json_string(prepared_response, "kind", ""), "programme") || start_utc_us < 0)
        return invalid(error, "Doğrulanmış kuşak veya yerel ses motoru hazır değil.");
    RadioSPlayoutRequest request = {.station_id = radios_data_station_id(ctx), .database_file = ctx->config->database_file,
        .mode = RADIOS_AUDIO_RUN_EOF, .now_utc_us = g_get_real_time(), .start_utc_us = start_utc_us,
        .live = TRUE, .history = p->history};
    default_policy_snapshot(ctx, &request);
    RadioSPlayoutPlan *plan = build_plan(ctx, prepared_response, &request, error);
    if (!plan) return FALSE;
    guint context = start_utc_us > 0 || plan->routed_rotation || (automation_id && *automation_id) ?
        RADIOS_AUDIO_LOUDNESS_AUTOMATION : RADIOS_AUDIO_LOUDNESS_MANUAL;
    if (plan->player_autodj || plan->routed_rotation) context |= RADIOS_AUDIO_LOUDNESS_AUTODJ;
    plan_loudness_origin(plan, context, automation_id, assigned);
    runner_command(p,RADIOS_AUDIO_PLAYER_RUN_STOP, NULL); checkpoint_broadcast(p); checkpoint_advert(p); capture_history(p); extension_clear(p);
    ++p->intent; p->preparing = p->automatic = p->failed = FALSE; owner_clear(p);
    g_clear_pointer(&p->automation_config, json_node_free);
    g_weak_ref_set(&p->owner, owner); p->owner_handler = g_signal_connect(owner, "destroy", G_CALLBACK(owner_destroyed), p);
    if (!install_plan(p, plan, error)) {
        radios_playout_plan_free(plan); p->failed = TRUE; p->snapshot.state = RADIOS_PLAYOUT_ERROR;
        g_strlcpy(p->snapshot.error, error && *error ? (*error)->message : "Kuşak başlatılamadı.", sizeof p->snapshot.error);
        notice(p, p->snapshot.error); return FALSE;
    }
    radios_broadcast_queue_run_config(ctx, NULL);
    notice(p, start_utc_us > g_get_real_time() ? "Reklam kuşağı UTC başlangıcını bekliyor." : "Reklam kuşağı C ses motorunda hazırlanıyor.");
    return TRUE;
}
gboolean radios_playout_ui_start_prepared(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared_response, gint64 start_utc_us, GError **error)
{ return radios_playout_ui_start_prepared_with_loudness_origin(ctx, owner, prepared_response, start_utc_us, NULL, FALSE, error); }
gboolean radios_playout_ui_start_playlist(RadioSViewContext *ctx, GtkWidget *owner, const gchar *playlist_id,
    JsonArray *ids, guint mode, gdouble overlap, GError **error)
{ return prepare_start(ctx, owner, "playlist", playlist_id, ids, NULL, 0, mode, overlap, 0, FALSE, NULL, NULL, NULL, error); }
gboolean radios_playout_ui_start_programme(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config,
    guint start_index, guint mode, gdouble overlap, GError **error)
{ return prepare_start(ctx, owner, "programme", NULL, NULL, config, start_index, mode, overlap, 0, FALSE, NULL, NULL, NULL, error); }
gboolean radios_playout_ui_schedule_programme(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config,
    gint64 start_ms, guint mode, gdouble overlap, GError **error)
{
    if (start_ms <= 0 || start_ms > G_MAXINT64 / 1000) return invalid(error, "Takvim başlangıcı geçersiz.");
    return prepare_start(ctx, owner, "programme", NULL, NULL, config, 0, mode, overlap, start_ms * 1000, FALSE, NULL, NULL, NULL, error);
}
gboolean radios_playout_ui_start_automation_with_loudness_origin(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *config, const gchar *automation_id, gboolean assigned, GError **error)
{ return prepare_start_origin(ctx, owner, "automation", NULL, NULL, config, 0, 0, 0, 0, FALSE, NULL, NULL, NULL, automation_id, assigned, error); }
gboolean radios_playout_ui_start_automation(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, GError **error)
{ return radios_playout_ui_start_automation_with_loudness_origin(ctx, owner, config, NULL, FALSE, error); }
gboolean radios_playout_ui_preview_automation(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy, GError **error)
{ return prepare_start(ctx, owner, "automation", NULL, NULL, config, 0, 0, 0, 0, TRUE, callback, user, destroy, error); }
/* Kılavuz s.15: Next on the row before a live step (manual or duration
 * rule) ends that row into the step's wait rather than skipping the step:
 * the source moves to its end and the run enters MANUAL_WAIT (planner.c),
 * where a manual step waits for the next Next and a duration step counts
 * from there (live_wait_poll). *handled FALSE: the ordinary Next applies. */
static gboolean live_step_end_current(RadioSPlayoutController *p, gboolean *handled, GError **error)
{
    *handled = FALSE; if (!p->plan) return FALSE;
    RadioSAudioRunSnapshot run; runner_snapshot(p, &run);
    if (run.state != RADIOS_AUDIO_RUN_PLAYING || run.index >= p->plan->entries->len || run.deck_current < 0) return FALSE;
    RadioSPlayoutEntry *entry = g_ptr_array_index(p->plan->entries, run.index);
    JsonNode *node = entry->binding ? json_object_get_member(entry->binding, "live_step") : NULL;
    JsonObject *step = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    const gchar *rule = radios_json_string(step, "end_rule", "");
    if (!g_str_equal(run.entry_id, entry->run.entry_id) || entry->run.mode != RADIOS_AUDIO_RUN_MANUAL ||
        (g_strcmp0(rule, "manual") && g_strcmp0(rule, "duration"))) return FALSE;
    *handled = TRUE;
    RadioSAudioSnapshot audio; radios_audio_engine_snapshot(p->engine, &audio);
    guint64 end = entry->run.media.cue_out_frame, start = entry->run.media.cue_in_frame;
    return radios_audio_engine_seek_if_generation(p->engine, (guint)run.deck_current,
        audio.decks[run.deck_current].generation, end > start + 480 ? end - 480 : start, error);
}
gboolean radios_playout_ui_action(RadioSViewContext *ctx, RadioSPlayoutAction action, GError **error)
{
    RadioSPlayoutController *p = controller(ctx); if (!p) return invalid(error, "Yerel yürütücü hazır değil.");
    if (p->scoped && !scoped_owns_run(p)) return invalid(error, "Bu AutoDJ kapsamı artık bu sıranın sahibi değil.");
    if (action == RADIOS_PLAYOUT_STOP) {
        gboolean result = runner_command(p,RADIOS_AUDIO_PLAYER_RUN_STOP, error);
        checkpoint_broadcast(p);
        checkpoint_advert(p); capture_history(p); extension_clear(p);
        ++p->intent; p->preparing = p->automatic = p->failed = FALSE; owner_clear(p);
        p->snapshot = (RadioSPlayoutSnapshot){.state = RADIOS_PLAYOUT_IDLE, .deck = -1, .next_deck = -1}; return result;
    }
    if (p->preparing) return invalid(error, "Sıra hazırlanıyor; iptal etmek için Durdur kullanın.");
    switch (action) {
    case RADIOS_PLAYOUT_PAUSE: return runner_command(p,RADIOS_AUDIO_PLAYER_RUN_PAUSE, error);
    case RADIOS_PLAYOUT_RESUME: return runner_command(p,RADIOS_AUDIO_PLAYER_RUN_RESUME, error);
    case RADIOS_PLAYOUT_NEXT:
    case RADIOS_PLAYOUT_PREVIOUS: {
        gboolean handled = FALSE;
        if (action == RADIOS_PLAYOUT_NEXT) { gboolean ended = live_step_end_current(p, &handled, error); if (handled) return ended; }
        gboolean accepted = runner_command(p, action == RADIOS_PLAYOUT_NEXT ?
            RADIOS_AUDIO_PLAYER_RUN_NEXT : RADIOS_AUDIO_PLAYER_RUN_PREVIOUS, error);
        if (accepted) {
            JsonArray *completed = radios_playout_ui_take_completed(ctx); json_array_unref(completed);
        }
        return accepted;
    }
    case RADIOS_PLAYOUT_STOP: break;
    }
    return invalid(error, "Yürütücü komutu geçersiz.");
}
gboolean radios_playout_ui_navigation_snapshot(RadioSViewContext *ctx,
    RadioSAudioRunSnapshot *snapshot)
{
    RadioSPlayoutController *p = controller(ctx);
    if (!p || !snapshot || !p->plan || p->preparing || p->failed) return FALSE;
    runner_snapshot(p, snapshot);
    return TRUE;
}
/* "Sonraki playlist" and the header playlist picker (U3) walk one started
 * plan: its accepted rows while the playing row is the runner's own entry
 * and belongs to an event block. NULL when it cannot navigate by playlist. */
static JsonArray *playlist_rows(RadioSPlayoutController *p, RadioSAudioRunSnapshot *run)
{
    if (!p || !p->plan || p->preparing || p->failed || p->automatic) return NULL;
    runner_snapshot(p, run);
    if ((run->state != RADIOS_AUDIO_RUN_PLAYING && run->state != RADIOS_AUDIO_RUN_OVERLAP &&
         run->state != RADIOS_AUDIO_RUN_MANUAL_WAIT) || run->index >= p->plan->entries->len) return NULL;
    RadioSPlayoutEntry *current = g_ptr_array_index(p->plan->entries, run->index);
    if (!g_str_equal(run->entry_id, current->run.entry_id)) return NULL;
    JsonObject *display = json_node_get_object(p->plan->display);
    JsonArray *rows = json_object_has_member(display, "entries") ?
        json_object_get_array_member(display, "entries") : NULL;
    if (!rows || json_array_get_length(rows) != p->plan->entries->len) return NULL;
    JsonObject *row = json_array_get_object_element(rows, run->index);
    return *radios_json_string(row, "block_id", "") ? rows : NULL;
}
/* First row of the playlist (event block) after the block holding row from. */
static gboolean playlist_after(JsonArray *rows, guint from, guint *target)
{
    const gchar *block = radios_json_string(json_array_get_object_element(rows, from), "block_id", "");
    for (guint i = from + 1; i < json_array_get_length(rows); ++i) {
        const gchar *candidate = radios_json_string(json_array_get_object_element(rows, i), "block_id", "");
        if (*candidate && !g_str_equal(block, candidate)) { *target = i; return TRUE; }
    }
    return FALSE;
}
static gboolean next_playlist_target(RadioSPlayoutController *p,
    RadioSAudioRunSnapshot *run, guint *target)
{
    JsonArray *rows = playlist_rows(p, run);
    return rows && playlist_after(rows, run->index, target);
}
/* The bounded jump within the observed epoch, shared by both entry points. */
static gboolean playlist_jump(RadioSPlayoutController *p, const RadioSAudioRunSnapshot *run,
    guint target, GError **error)
{
    if (p->player_deck >= 0)
        return radios_audio_engine_player_runner_jump_to(p->engine, (guint)p->player_deck,
            run->epoch, target, error);
    return radios_audio_engine_runner_jump_to(p->engine, run->epoch, target, error);
}
gboolean radios_playout_ui_can_next_playlist(RadioSViewContext *ctx)
{
    RadioSAudioRunSnapshot run = {0}; guint target = 0;
    return next_playlist_target(controller(ctx), &run, &target);
}
gboolean radios_playout_ui_next_playlist(RadioSViewContext *ctx, GError **error)
{
    RadioSPlayoutController *p = controller(ctx);
    RadioSAudioRunSnapshot run = {0}; guint target = 0;
    if (!next_playlist_target(p, &run, &target))
        return invalid(error, "Çalan sırada geçilebilecek bir sonraki playlist yok.");
    return playlist_jump(p, &run, target, error);
}
static const gchar *header_playlist(RadioSPlayoutController *p, guint index, const gchar **title);
/* U3 (C10): the header picker lists the playing playlist and exactly the
 * targets that repeated "Sonraki playlist" presses would reach after it. */
JsonArray *radios_playout_ui_playlist_choices(RadioSViewContext *ctx)
{
    RadioSPlayoutController *p = controller(ctx); RadioSAudioRunSnapshot run = {0};
    JsonArray *rows = playlist_rows(p, &run);
    if (!rows) return NULL;
    JsonArray *choices = json_array_new(); guint index = run.index;
    do {
        JsonObject *row = json_array_get_object_element(rows, index), *choice = json_object_new();
        RadioSPlayoutEntry *entry = g_ptr_array_index(p->plan->entries, index);
        const gchar *kind = radios_json_string(row, "block_kind", ""), *title = radios_json_string(row, "block_title", "");
        if (!*title) {
            header_playlist(p, index, &title);
            if (!title || !*title) title = radios_programme_event_title(kind);
        }
        const gchar *label = radios_programme_kind_label(kind);
        gchar *shown = *kind && !g_str_equal(kind, "music") && !g_str_has_prefix(title, label) ?
            g_strdup_printf("%s · %s", label, title) : g_strdup(title);
        json_object_set_int_member(choice, "epoch", (gint64)run.epoch);
        json_object_set_int_member(choice, "index", index);
        json_object_set_string_member(choice, "entry_id", entry->run.entry_id);
        json_object_set_string_member(choice, "title", shown); g_free(shown);
        json_object_set_string_member(choice, "kind", kind);
        json_object_set_string_member(choice, "first_title", entry->title ? entry->title : "");
        json_object_set_string_member(choice, "first_artist", entry->artist ? entry->artist : "");
        json_object_set_boolean_member(choice, "current", index == run.index);
        json_array_add_object_element(choices, choice);
    } while (playlist_after(rows, index, &index));
    return choices;
}
gboolean radios_playout_ui_jump_playlist(RadioSViewContext *ctx, guint64 epoch, guint index,
    const gchar *entry_id, GError **error)
{
    RadioSPlayoutController *p = controller(ctx); RadioSAudioRunSnapshot run = {0};
    JsonArray *rows = playlist_rows(p, &run);
    if (!rows) return invalid(error, "Çalan sırada geçilebilecek bir playlist yok.");
    if (run.epoch != epoch) return invalid(error, "Yayın sırası değişti; playlist listesini yeniden açın.");
    guint target = run.index;
    while (target < index && playlist_after(rows, target, &target)) {}
    RadioSPlayoutEntry *entry = target == index && index > run.index ? g_ptr_array_index(p->plan->entries, index) : NULL;
    if (!entry || g_strcmp0(entry->run.entry_id, entry_id))
        return invalid(error, "Seçilen playlist artık sırada değil.");
    return playlist_jump(p, &run, index, error);
}
gboolean radios_playout_ui_get_snapshot(RadioSViewContext *ctx, RadioSPlayoutSnapshot *out)
{
    RadioSPlayoutController *p = controller(ctx); if (!p || !out) return FALSE;
    if (p->scoped && !scoped_owns_run(p)) {
        *out = (RadioSPlayoutSnapshot){.state = RADIOS_PLAYOUT_IDLE, .deck = -1, .next_deck = -1}; return TRUE;
    }
    *out = p->snapshot; out->automation = p->automatic; return TRUE;
}
JsonNode *radios_playout_ui_plan_snapshot(RadioSViewContext *ctx)
{
    RadioSPlayoutController *p = controller(ctx);
    if (!p || !p->plan || (p->scoped && !scoped_owns_run(p))) return NULL;
    RadioSAudioRunSnapshot run; runner_snapshot(p, &run);
    if (run.state == RADIOS_AUDIO_RUN_IDLE || run.state == RADIOS_AUDIO_RUN_FINISHED ||
        run.state == RADIOS_AUDIO_RUN_ERROR || run.count != p->plan->entries->len || run.index >= run.count) return NULL;
    gboolean owned = FALSE;
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) owned |= run.owned[deck];
    if (!owned) return NULL;
    RadioSPlayoutEntry *current = g_ptr_array_index(p->plan->entries, run.index);
    if (*run.entry_id && g_strcmp0(run.entry_id, current->run.entry_id)) return NULL;
    JsonObject *result = json_object_new(); JsonArray *entries = json_array_new();
    json_object_set_array_member(result, "entries", entries);
    json_object_set_int_member(result, "source", p->player_deck + 1);
    json_object_set_int_member(result, "current_player", run.deck_current + 1);
    json_object_set_int_member(result, "next_player", run.deck_next + 1);
    JsonArray *players = json_array_new();
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck)
        if (run.owned[deck]) json_array_add_int_element(players, deck + 1);
    json_object_set_array_member(result, "players", players);

    json_object_set_int_member(result, "epoch", (gint64)run.epoch);
    json_object_set_int_member(result, "state", map_state(run.state)); json_object_set_int_member(result, "index", run.index);
    json_object_set_int_member(result, "next_index", run.deck_next >= 0 && run.next_index < run.count ? (gint64)run.next_index : -1);
    json_object_set_string_member(result, "entry_id", current->run.entry_id);
    json_object_set_string_member(result, "programme_title",
        radios_json_string(json_node_get_object(p->plan->display), "programme_title", ""));
    JsonArray *display_rows = json_object_get_array_member(json_node_get_object(p->plan->display), "entries");
    for (guint i = 0; i < p->plan->entries->len; ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(p->plan->entries, i); JsonObject *row = json_object_new();
        json_object_set_string_member(row, "entry_id", entry->run.entry_id);
        json_object_set_string_member(row, "title", entry->title ? entry->title : "");
        json_object_set_string_member(row, "artist", entry->artist ? entry->artist : "");
        json_object_set_string_member(row, "kind", entry->jingle ? "jingle" : entry->kind ? entry->kind : "");
        json_object_set_string_member(row, "source_id", entry->source_id ? entry->source_id : "");
        json_object_set_string_member(row, "source_name", entry->source_name ? entry->source_name : "");
        json_object_set_string_member(row, "playlist_id", radios_json_string(entry->binding, "playlist_id", ""));
        json_object_set_int_member(row, "duration_ms", (gint64)((entry->run.media.cue_out_frame - entry->run.media.cue_in_frame) / 48));
        if (display_rows && i < json_array_get_length(display_rows)) {
            JsonObject *display = json_array_get_object_element(display_rows, i);
            if (!g_strcmp0(radios_json_string(display, "entry_id", ""), entry->run.entry_id)) {
                const gchar *fields[] = {"block_id", "block_kind", "block_title"};
                for (guint field = 0; field < G_N_ELEMENTS(fields); ++field)
                    json_object_set_string_member(row, fields[field], radios_json_string(display, fields[field], ""));
                JsonNode *explicit_block = json_object_get_member(display, "block_explicit");
                if (explicit_block) json_object_set_member(row, "block_explicit", radios_programme_clone(explicit_block));
                JsonNode *jingles = json_object_get_member(display, "planned_jingles");
                if (jingles) json_object_set_member(row, "planned_jingles", radios_programme_clone(jingles));
                const gchar *flow_fields[] = {"live_step", "calendar_constraints", "live", "programme_entry"};
                for (guint field = 0; field < G_N_ELEMENTS(flow_fields); ++field) {
                    JsonNode *hint = json_object_get_member(display, flow_fields[field]);
                    if (hint) json_object_set_member(row, flow_fields[field], radios_programme_clone(hint));
                }
            }
        }
        gint player = entry->run.preferred_deck;
        if (i == run.index && run.deck_current >= 0) player = run.deck_current;
        else if (i == run.next_index && run.deck_next >= 0) player = run.deck_next;
        json_object_set_int_member(row, "player", player + 1);
        json_object_set_int_member(row, "planned_utc_us", entry->planned_utc_us);
        json_array_add_object_element(entries, row);
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, result); return node;
}
JsonArray *radios_playout_ui_take_completed(RadioSViewContext *ctx)
{
    JsonArray *result = json_array_new(); RadioSPlayoutController *p = controller(ctx);
    if (!p || !p->plan || p->preparing || p->failed ||
        p->snapshot.state == RADIOS_PLAYOUT_IDLE || p->snapshot.state == RADIOS_PLAYOUT_ERROR ||
        p->snapshot.state == RADIOS_PLAYOUT_PREPARING || p->snapshot.state == RADIOS_PLAYOUT_SCHEDULED) return result;
    RadioSAudioRunSnapshot run; runner_snapshot(p, &run);
    if (run.state == RADIOS_AUDIO_RUN_IDLE || run.state == RADIOS_AUDIO_RUN_ERROR ||
        run.state == RADIOS_AUDIO_RUN_PREROLL || run.state == RADIOS_AUDIO_RUN_SCHEDULED) return result;
    RadioSAudioRunReceipt receipts[RADIOS_AUDIO_RUN_MAX_RECEIPTS];
    guint count = runner_receipts(p, receipts, G_N_ELEMENTS(receipts));
    for (guint i = 0; i < count; ++i) {
        const RadioSAudioRunReceipt *receipt = &receipts[i];
        if (receipt->epoch != run.epoch || receipt->terminal != RADIOS_AUDIO_RUN_TERMINAL_COMPLETE ||
            receipt->index >= p->plan->entries->len) continue;
        gchar key[96]; g_snprintf(key, sizeof key, "complete:%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT,
            receipt->epoch, receipt->occurrence);
        if (g_hash_table_contains(p->observed, key)) continue;
        g_hash_table_add(p->observed, g_strdup(key));
        RadioSPlayoutEntry *entry = g_ptr_array_index(p->plan->entries, receipt->index);
        json_array_add_string_element(result, entry->run.entry_id);
    }
    return result;
}
static void metadata_copy(gchar *destination, gsize capacity, const gchar *value)
{
    g_strlcpy(destination, value ? value : "", capacity);
    const gchar *invalid_utf8 = NULL;
    if (!g_utf8_validate(destination, -1, &invalid_utf8)) destination[invalid_utf8 - destination] = '\0';
}
static gint64 header_frames_us(gdouble frames)
{ return (gint64)ceil(MAX(0.0, frames) * G_USEC_PER_SEC / 48000.0); }
static const gchar *header_playlist(RadioSPlayoutController *p, guint index, const gchar **title)
{
    JsonObject *display = json_node_get_object(p->plan->display);
    JsonArray *rows = json_object_get_array_member(display, "entries");
    JsonObject *row = rows && index < json_array_get_length(rows) ? json_array_get_object_element(rows, index) : NULL;
    RadioSPlayoutEntry *entry = g_ptr_array_index(p->plan->entries, index);
    const gchar *playlist = radios_json_string(row, "playlist_id", "");
    if (*playlist) { *title = entry->source_name; return playlist; }
    const gchar *block = radios_json_string(row, "block_id", "");
    if (*block && row && json_object_has_member(row, "block_explicit") && json_object_get_boolean_member(row, "block_explicit")) {
        *title = radios_json_string(row, "block_title", ""); return block;
    }
    if (entry->source_id && *entry->source_id) { *title = entry->source_name; return entry->source_id; }
    const gchar *name = radios_json_string(display, "playlist_title", "");
    if (!*name && !*radios_json_string(display, "programme_title", "")) name = entry->source_name;
    if (*name) { *title = name; return "accepted-playlist"; }
    *title = ""; return NULL;
}
static void header_finish(RadioSPlayoutController *p, const RadioSAudioRunSnapshot *run,
    RadioSPlayoutHeaderSnapshot *out)
{
    RadioSPlayoutEntry *current = g_ptr_array_index(p->plan->entries, run->index);
    const gchar *name = NULL, *key = header_playlist(p, run->index, &name);
    metadata_copy(out->playlist_title, sizeof out->playlist_title, name);
    if (run->deck_current < 0 || (guint)run->deck_current >= RADIOS_AUDIO_NORMAL_DECKS || !run->owned[run->deck_current]) return;
    gint64 now = g_get_real_time();
    gdouble start = -source_output_frames(&current->run, run->actual_current_frame);
    gdouble queue_end = 0, playlist_end = 0;
    gboolean queue_known = !p->automatic, playlist_known = FALSE, within_playlist = key != NULL;
    gboolean advert_known = run->state == RADIOS_AUDIO_RUN_PLAYING || run->state == RADIOS_AUDIO_RUN_OVERLAP ||
        run->state == RADIOS_AUDIO_RUN_PAUSED;
    out->track_remaining_us = header_frames_us(start + source_output_frames(&current->run, current->run.media.cue_out_frame));
    for (guint i = run->index; i < p->plan->entries->len; ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(p->plan->entries, i);
        const RadioSAudioRunItem *item = &entry->run;
        if (advert_known && !out->advert_timing_known && !g_strcmp0(entry->kind, "advertisement")) {
            out->advert_timing_known = TRUE;
            out->advert_remaining_us = header_frames_us(start);
        }
        gdouble physical_end = start + source_output_frames(item, item->media.cue_out_frame);
        gdouble end = physical_end, next_start = 0;
        RadioSPlayoutEntry *next = i + 1 < p->plan->entries->len ? g_ptr_array_index(p->plan->entries, i + 1) : NULL;
        if (next) {
            RadioSAudioRunItem policy = *item;
            policy.overlap_frames = MIN(policy.overlap_frames, radios_audio_run_incoming_span_frames(&next->run));
            gboolean anchored = next->run.start_utc_us > 0;
            gboolean cut = anchored && next->run.start_policy == RADIOS_AUDIO_RUN_START_CUT;
            gboolean finish = anchored && next->run.start_policy == RADIOS_AUDIO_RUN_START_FINISH;
            if (i == run->index && run->transition_total_frames && run->next_index == i + 1) {
                next_start = -source_output_frames(&next->run, run->actual_next_frame);
                end = MAX(0.0, (gdouble)run->transition_total_frames - run->transition_frames);
                out->track_remaining_us = MIN(out->track_remaining_us, header_frames_us(end));
            } else {
                if (item->mode == RADIOS_AUDIO_RUN_MANUAL && !cut && !finish) { queue_known = FALSE; break; }
                guint64 trigger = radios_audio_run_trigger_frame(&policy, &policy.rhythm, policy.tempo_ratio, policy.media.cue_out_frame);
                next_start = start + source_output_frames(&policy, trigger);
                gdouble anchor = anchored ? (gdouble)(next->run.start_utc_us - now) * 48000 / G_USEC_PER_SEC : 0;
                if (cut) next_start = anchor;
                else if (finish) {
                    next_start = MAX(physical_end, anchor);
                    if (next->run.max_wait_frames && next_start > anchor + next->run.max_wait_frames) {
                        next_start = anchor + next->run.max_wait_frames; cut = TRUE;
                    }
                } else if (anchored) next_start = MAX(next_start, anchor);
                if (i == run->index) next_start = MAX(0.0, next_start);
                guint64 remaining = (guint64)ceil(MAX(0.0, physical_end - next_start));
                guint64 overlap = cut ? MIN(remaining, 240u) : radios_audio_run_overlap_frames(&policy, remaining);
                overlap = MIN(overlap, radios_audio_run_incoming_span_frames(&next->run));
                end = MIN(physical_end, next_start + overlap);
            }
        }
        if (i == run->index)
            out->track_remaining_us = MIN(out->track_remaining_us, header_frames_us(end));
        queue_end = MAX(queue_end, end);
        if (within_playlist) {
            playlist_end = MAX(playlist_end, end);
            const gchar *next_title = NULL;
            const gchar *next_key = next ? header_playlist(p, i + 1, &next_title) : NULL;
            if (!next || g_strcmp0(key, next_key)) { playlist_known = TRUE; within_playlist = FALSE; }
        }
        start = next_start;
    }
    if (queue_known) out->programme_remaining_us = header_frames_us(queue_end);
    if (playlist_known) out->playlist_remaining_us = header_frames_us(playlist_end);
    gboolean clock_running = run->state == RADIOS_AUDIO_RUN_PLAYING || run->state == RADIOS_AUDIO_RUN_OVERLAP;
    if (clock_running) {
        if (queue_known) out->programme_end_utc_us = now + out->programme_remaining_us;
        if (playlist_known) out->playlist_end_utc_us = now + out->playlist_remaining_us;
    }
    if (p->plan->options.end_utc_us) {
        /* The slot boundary is metadata, independent of an END_FINISH grace. */
        out->programme_end_utc_us = p->plan->options.end_utc_us;
        out->programme_remaining_us = MAX((gint64)0, out->programme_end_utc_us - now);
    }
}
gboolean radios_playout_ui_header_snapshot(RadioSViewContext *ctx, RadioSPlayoutHeaderSnapshot *out)
{
    if (!out) return FALSE;
    memset(out, 0, sizeof(*out));
    RadioSPlayoutController *p = controller(ctx);
    if (!p || p->closed) return FALSE;
    if (!p->plan || p->preparing || p->failed) return TRUE;
    RadioSAudioRunSnapshot run; runner_snapshot(p, &run);
    if (run.state != RADIOS_AUDIO_RUN_PLAYING && run.state != RADIOS_AUDIO_RUN_OVERLAP &&
        run.state != RADIOS_AUDIO_RUN_PAUSED && run.state != RADIOS_AUDIO_RUN_WAITING_MEDIA &&
        run.state != RADIOS_AUDIO_RUN_MANUAL_WAIT) return TRUE;
    if (run.index >= p->plan->entries->len) return TRUE;
    RadioSPlayoutEntry *entry = g_ptr_array_index(p->plan->entries, run.index);
    if (!g_str_equal(run.entry_id, entry->run.entry_id)) return TRUE;
    const gchar *title = radios_json_string(json_node_get_object(p->plan->display), "programme_title", "");
    metadata_copy(out->programme_title, sizeof out->programme_title, title);
    out->programme_active = out->programme_title[0] != '\0';
    header_finish(p, &run, out);
    return TRUE;
}
gboolean radios_playout_ui_owns_deck(RadioSViewContext *ctx, guint deck)
{
    RadioSPlayoutController *p = controller(ctx); if (!p || deck >= 32) return FALSE;
    RadioSAudioRunSnapshot run; runner_snapshot(p, &run); return run.owned[deck];
}
gboolean radios_playout_ui_deck_metadata(RadioSViewContext *ctx, guint deck, RadioSPlayoutDeckMetadata *out)
{
    if (!out) return FALSE;
    memset(out, 0, sizeof(*out));
    RadioSPlayoutController *playout = controller(ctx);
    if (!playout || !playout->plan || deck >= RADIOS_AUDIO_NORMAL_DECKS) return FALSE;
    RadioSAudioRunSnapshot run; runner_snapshot(playout, &run);
    if (!run.owned[deck]) return FALSE;
    RadioSAudioSnapshot audio; radios_audio_engine_snapshot(playout->engine, &audio);
    const RadioSAudioDeckSnapshot *loaded = &audio.decks[deck];
    if (!loaded->media_id[0] || !loaded->version_id[0]) return FALSE;
    guint index = run.deck_next == (gint)deck ? run.next_index : run.index;
    if (index >= playout->plan->entries->len) return FALSE;
    RadioSPlayoutEntry *entry = g_ptr_array_index(playout->plan->entries, index);
    if (!g_str_equal(entry->run.media.media_id, loaded->media_id) || !g_str_equal(entry->run.media.version_id, loaded->version_id)) {
        entry = NULL;
        /* U1: a preloaded pool player holds a later item, bound by its entry. */
        for (guint candidate = run.index + 1; loaded->binding.entry_id[0] && candidate < playout->plan->entries->len; ++candidate) {
            RadioSPlayoutEntry *later = g_ptr_array_index(playout->plan->entries, candidate);
            if (g_str_equal(later->run.entry_id, loaded->binding.entry_id) && g_str_equal(later->run.media.media_id, loaded->media_id) &&
                g_str_equal(later->run.media.version_id, loaded->version_id)) { entry = later; index = candidate; break; }
        }
        for (guint candidate = MIN(run.index + 1, playout->plan->entries->len); !entry && candidate > 0; --candidate) {
            RadioSPlayoutEntry *previous = g_ptr_array_index(playout->plan->entries, candidate - 1);
            if (g_str_equal(previous->run.media.media_id, loaded->media_id) && g_str_equal(previous->run.media.version_id, loaded->version_id)) {
                entry = previous; index = candidate - 1; break;
            }
        }
        if (!entry) return FALSE;
    }
    metadata_copy(out->title, sizeof out->title, entry->title);
    metadata_copy(out->artist, sizeof out->artist, entry->artist);
    metadata_copy(out->album, sizeof out->album, entry->album);
    metadata_copy(out->source_name, sizeof out->source_name, entry->source_name);
    metadata_copy(out->kind, sizeof out->kind, entry->jingle ? "jingle" : entry->kind);
    out->duration_ms = entry->run.duration_frames / 48;
    if (index + 1 < playout->plan->entries->len) {
        RadioSPlayoutEntry *next = g_ptr_array_index(playout->plan->entries, index + 1);
        metadata_copy(out->next_title, sizeof out->next_title, next->title);
        metadata_copy(out->next_artist, sizeof out->next_artist, next->artist);
    }
    return TRUE;
}
static gboolean content_matches(const RadioSAudioDeckSnapshot *deck,
    const RadioSAudioLoadSpec *media, const gchar *entry_id)
{
    return deck->generation && deck->binding.generation == deck->generation &&
        !g_strcmp0(deck->media_id, media->media_id) && !g_strcmp0(deck->version_id, media->version_id) &&
        !g_strcmp0(deck->binding.entry_id, entry_id);
}
static RadioSPlayoutContentState content_state(const RadioSAudioDeckSnapshot *deck)
{
    if (deck->state == RADIOS_AUDIO_PLAYING && !deck->runner_paused && deck->main_routed &&
        deck->main_path_ready && deck->cursor_frames > deck->cue_in_frame) return RADIOS_PLAYOUT_CONTENT_PLAYING;
    if (deck->state == RADIOS_AUDIO_PAUSED || deck->runner_paused) return RADIOS_PLAYOUT_CONTENT_PAUSED;
    if (deck->state == RADIOS_AUDIO_READY || deck->state == RADIOS_AUDIO_LOADING) return RADIOS_PLAYOUT_CONTENT_READY;
    return RADIOS_PLAYOUT_CONTENT_WAITING;
}
gboolean radios_playout_ui_deck_content(RadioSViewContext *ctx, guint deck, RadioSPlayoutDeckContent *out)
{
    if (!out) return FALSE;
    memset(out, 0, sizeof *out);
    RadioSPlayoutController *p = controller(ctx);
    if (!p || !p->plan || p->preparing || p->failed || deck >= RADIOS_AUDIO_NORMAL_DECKS) return FALSE;
    RadioSAudioRunSnapshot before, after; runner_snapshot(p, &before);
    if (before.state == RADIOS_AUDIO_RUN_IDLE || before.state == RADIOS_AUDIO_RUN_FINISHED ||
        before.state == RADIOS_AUDIO_RUN_ERROR || before.index >= p->plan->entries->len) return FALSE;
    gint owner = -2;
    if (!radios_audio_engine_deck_runner_owner(p->engine, deck, &owner) || owner != p->player_deck) return FALSE;
    RadioSAudioSnapshot *audio = g_new0(RadioSAudioSnapshot, 1);
    radios_audio_engine_snapshot(p->engine, audio);
    const RadioSAudioDeckSnapshot *loaded = &audio->decks[deck];
    RadioSPlayoutEntry *entry = NULL;
    guint overlay_deck = RADIOS_AUDIO_NORMAL_DECKS;
    guint64 overlay_generation = 0;
    guint candidates[] = {before.index, before.next_index};
    for (guint i = 0; i < G_N_ELEMENTS(candidates); ++i) {
        if (candidates[i] >= p->plan->entries->len || (i && before.deck_next < 0)) continue;
        RadioSPlayoutEntry *candidate = g_ptr_array_index(p->plan->entries, candidates[i]);
        if (before.owned[deck] && content_matches(loaded, &candidate->run.media, candidate->run.entry_id)) {
            entry = candidate; break;
        }
        const RadioSAudioRunAdjunct *voice = &candidate->run.adjunct;
        if (!before.owned[deck] && voice->enabled && voice->player == deck &&
            content_matches(loaded, &voice->media, voice->entry_id)) {
            entry = candidate; out->auxiliary = TRUE; break;
        }
    }
    /* U1: a preloaded pool player holds a later item of this queue. */
    for (guint candidate = MAX(before.index, before.deck_next >= 0 ? before.next_index : before.index) + 1;
         !entry && before.owned[deck] && candidate < p->plan->entries->len; ++candidate) {
        RadioSPlayoutEntry *later = g_ptr_array_index(p->plan->entries, candidate);
        if (content_matches(loaded, &later->run.media, later->run.entry_id)) entry = later;
    }
    if (entry) {
        const gchar *kind = out->auxiliary ? radios_json_string(entry->binding, "adjunct_kind", "jingle") : entry->kind;
        metadata_copy(out->kind, sizeof out->kind, kind);
        metadata_copy(out->title, sizeof out->title, out->auxiliary ? entry->run.adjunct.title : entry->title);
        metadata_copy(out->artist, sizeof out->artist, out->auxiliary ? entry->run.adjunct.artist : entry->artist);
        out->state = content_state(loaded);
        const RadioSAudioRunAdjunct *voice = &entry->run.adjunct;
        if (!out->auxiliary && voice->enabled && voice->player < RADIOS_AUDIO_NORMAL_DECKS) {
            const RadioSAudioDeckSnapshot *overlay = &audio->decks[voice->player];
            gint voice_owner = -2;
            gboolean bound = radios_audio_engine_deck_runner_owner(p->engine, voice->player, &voice_owner) &&
                voice_owner == owner && content_matches(overlay, &voice->media, voice->entry_id);
            if (bound) { overlay_deck = voice->player; overlay_generation = overlay->generation; }
            RadioSAudioTempoProfile tempo = radios_audio_run_tempo_profile(&entry->run);
            gdouble elapsed = radios_audio_tempo_profile_output(&tempo,
                loaded->cursor_frames > loaded->cue_in_frame ? loaded->cursor_frames - loaded->cue_in_frame : 0);
            if (bound && overlay->cursor_frames > overlay->cue_in_frame && overlay->cursor_frames < overlay->cue_out_frame)
                out->overlay_state = content_state(overlay);
            else if (elapsed < voice->start_output_frame) out->overlay_state = RADIOS_PLAYOUT_CONTENT_READY;
            if (out->overlay_state != RADIOS_PLAYOUT_CONTENT_NONE) {
                metadata_copy(out->overlay_kind, sizeof out->overlay_kind, radios_json_string(entry->binding, "adjunct_kind", "jingle"));
                metadata_copy(out->overlay_title, sizeof out->overlay_title, voice->title);
                JsonObject *leaf = json_object_new(), *hint = json_object_new();
                json_object_set_string_member(leaf, "id", entry->run.entry_id);
                json_object_set_string_member(hint, "entry_id", voice->entry_id);
                json_object_set_string_member(hint, "title", voice->title);
                json_object_set_string_member(hint, "placement", "overlay");
                gchar *identity = radios_programme_hint_id(leaf, hint, 0);
                metadata_copy(out->overlay_id, sizeof out->overlay_id, identity);
                g_free(identity); json_object_unref(leaf); json_object_unref(hint);
                out->overlay_player = voice->player + 1;
            }
        }
    }
    guint64 generation = loaded->generation;
    RadioSAudioDeckSnapshot changed;
    radios_audio_engine_snapshot(p->engine, audio); changed = audio->decks[deck];
    runner_snapshot(p, &after);
    gint still_owner = -2;
    gboolean stable = radios_audio_engine_deck_runner_owner(p->engine, deck, &still_owner) && still_owner == owner &&
        before.epoch == after.epoch && before.index == after.index && before.next_index == after.next_index &&
        before.state == after.state && before.deck_current == after.deck_current && before.deck_next == after.deck_next &&
        generation == changed.generation;
    if (stable && entry) {
        const RadioSAudioLoadSpec *media = out->auxiliary ? &entry->run.adjunct.media : &entry->run.media;
        const gchar *entry_id = out->auxiliary ? entry->run.adjunct.entry_id : entry->run.entry_id;
        stable = content_matches(&changed, media, entry_id);
    }
    if (stable && overlay_deck < RADIOS_AUDIO_NORMAL_DECKS) {
        gint current_owner = -2;
        stable = radios_audio_engine_deck_runner_owner(p->engine, overlay_deck, &current_owner) &&
            current_owner == owner && audio->decks[overlay_deck].generation == overlay_generation &&
            content_matches(&audio->decks[overlay_deck], &entry->run.adjunct.media, entry->run.adjunct.entry_id);
    }
    if (!stable) memset(out, 0, sizeof *out);
    g_free(audio); return TRUE;
}
JsonNode *radios_playout_ui_calendar_turns(RadioSViewContext *ctx)
{
    RadioSPlayoutController *p = controller(ctx); if (!p) return NULL;
    capture_history(p); return clone_object(p->calendar_turns);
}
JsonNode *radios_playout_ui_probability_checkpoint(RadioSViewContext *ctx, gboolean selection)
{
    RadioSPlayoutController *p = controller(ctx); if (!p) return NULL;
    capture_history(p); JsonNode *node = selection ? p->policy_selection_checkpoint : p->probability_checkpoint;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? clone_object(json_node_get_object(node)) : NULL;
}
JsonNode *radios_playout_ui_history(RadioSViewContext *ctx)
{
    RadioSPlayoutController *p = controller(ctx); if (!p) return NULL;
    JsonNode *node = radios_playout_history_json(p->history); JsonArray *rows = json_object_get_array_member(json_node_get_object(node), "entries");
    for (guint i = 0; i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i); const gchar *title = radios_json_string(row, "media_id", "");
        for (guint j = 0; p->plan && j < p->plan->entries->len; ++j) {
            RadioSPlayoutEntry *entry = g_ptr_array_index(p->plan->entries, j);
            if (g_str_equal(radios_json_string(row, "media_id", ""), entry->run.media.media_id)) { title = entry->title; break; }
        }
        json_object_set_string_member(row, "title", title);
    }
    return node;
}
const gchar *radios_playout_ui_state_name(RadioSPlayoutState state)
{
    static const gchar *const names[] = {"Beklemede", "Hazırlanıyor", "UTC saatini bekliyor", "Çalıyor", "Geçiş yapıyor", "Duraklatıldı", "Medya / çıkış bekleniyor", "Operatör geçişini bekliyor", "Sıra bitti", "Yürütme hatası"};
    return state <= RADIOS_PLAYOUT_ERROR ? names[state] : "Bilinmeyen durum";
}

/* Weekly batches preserve all authenticated row hints. An unbound batch uses
 * the selected card, with its optional partner, rather than searching decks. */
static RadioSPlayoutPlan *prepared_weekly_plan(RadioSViewContext *ctx, JsonObject *response,
    guint source, guint target, GError **error)
{
    RadioSPlayoutController *p = controller(ctx);
    if (!p || p->player_deck >= 0 || (source >= 32 && source != G_MAXUINT) || !response ||
        !g_str_equal(radios_json_string(response, "kind", ""), "programme")) {
        invalid(error, "Haftalık program veya kaynak rotasyon hazır değil."); return NULL;
    }
    JsonNode *copy = clone_object(response); JsonObject *prepared = json_node_get_object(copy);
    JsonNode *node = json_object_get_member(prepared, "config");
    JsonObject *config = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    if (!config) { json_node_free(copy); invalid(error, "Program ayarları eksik."); return NULL; }
    json_object_remove_member(config, "autodj_players"); json_object_set_boolean_member(config, "beat_sync", FALSE);
    RadioSPlayoutRequest request = {.station_id = radios_data_station_id(ctx), .database_file = ctx->config->database_file,
        .now_utc_us = g_get_real_time(), .mode = RADIOS_AUDIO_RUN_EOF, .live = TRUE, .history = p->history};
    default_policy_snapshot(ctx, &request);
    RadioSPlayoutPlan *plan = build_plan(ctx, prepared, &request, error); json_node_free(copy);
    if (!plan) { calendar_timing_retry(error); return NULL; }
    if (plan->routed_rotation || source == G_MAXUINT) return plan;
    gboolean partner = target < 32 && target != source, overlap = FALSE;
    for (guint i = 0; i < plan->entries->len; ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i);
        overlap |= entry->run.overlap_frames != 0;
    }
    if (overlap && !partner) {
        radios_playout_plan_free(plan); invalid(error, "Program bindirmesi için ikinci rotasyon veya kayıtlı player dağılımı seçin."); return NULL;
    }
    plan->player_autodj = TRUE; plan->options.pool_count = partner ? 2 : 1;
    plan->options.deck_pool[0] = source; if (partner) plan->options.deck_pool[1] = target;
    for (guint i = 0; i < plan->entries->len; ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i);
        entry->run.preferred_deck = (gint)plan->options.deck_pool[i % plan->options.pool_count];
    }
    return plan;
}
gboolean radios_playout_prepared_weekly_pool(RadioSViewContext *ctx, JsonObject *prepared,
    guint source, guint target, guint decks[32], guint *count, gboolean *routed, GError **error)
{
    if (count) *count = 0;
    if (routed) *routed = FALSE;
    if (!decks || !count) return invalid(error, "Program player dağılımı sonucu eksik.");
    RadioSPlayoutPlan *plan = prepared_weekly_plan(ctx, prepared, source, target, error); if (!plan) return FALSE;
    if (source == G_MAXUINT && !plan->routed_rotation && !choose_pool(controller(ctx), plan, error)) { radios_playout_plan_free(plan); return FALSE; }
    *count = plan->options.pool_count; memcpy(decks, plan->options.deck_pool, *count * sizeof(guint));
    if (routed) *routed = plan->routed_rotation;
    radios_playout_plan_free(plan); return TRUE;
}
gboolean radios_playout_ui_start_prepared_weekly_with_loudness_origin(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared, guint source, guint target, const gchar *automation_id, gboolean assigned, GError **error)
{
    if (!loudness_origin_valid(automation_id, assigned, error)) return FALSE;
    RadioSPlayoutController *p = controller(ctx);
    if (!p || p->player_deck >= 0 || !owner || gtk_widget_in_destruction(owner) || !radios_data_connected(ctx))
        return invalid(error, "Haftalık program yürütücüsü hazır değil.");
    RadioSAudioRunSnapshot run; runner_snapshot(p, &run);
    for (guint deck = 0; deck < 32; ++deck) if (run.owned[deck]) return invalid(error, "Mevcut yayın sırası deckleri kullanımda.");
    if (p->preparing || (run.state != RADIOS_AUDIO_RUN_IDLE && run.state != RADIOS_AUDIO_RUN_FINISHED && run.state != RADIOS_AUDIO_RUN_ERROR))
        return invalid(error, "Mevcut genel yayın korunuyor; program başlatılmadı.");
    RadioSPlayoutPlan *plan = prepared_weekly_plan(ctx, prepared, source, target, error); if (!plan) return FALSE;
    plan_loudness_origin(plan, RADIOS_AUDIO_LOUDNESS_AUTOMATION |
        (plan->player_autodj || plan->routed_rotation ? RADIOS_AUDIO_LOUDNESS_AUTODJ : 0), automation_id, assigned);
    if (!choose_pool(p, plan, error)) { radios_playout_plan_free(plan); return FALSE; }
    checkpoint_broadcast(p); checkpoint_advert(p); capture_history(p); extension_clear(p);
    ++p->intent; p->preparing = p->automatic = p->failed = FALSE; owner_clear(p);
    g_clear_pointer(&p->automation_config, json_node_free);
    g_weak_ref_set(&p->owner, owner); p->owner_handler = g_signal_connect(owner, "destroy", G_CALLBACK(owner_destroyed), p);
    if (!install_plan(p, plan, error)) {
        radios_playout_plan_free(plan); p->failed = TRUE; p->snapshot.state = RADIOS_PLAYOUT_ERROR;
        g_strlcpy(p->snapshot.error, error && *error ? (*error)->message : "Haftalık program başlatılamadı.", sizeof p->snapshot.error);
        return FALSE;
    }
    radios_broadcast_queue_run_config(ctx, NULL); return TRUE;
}

gboolean radios_playout_ui_start_prepared_weekly(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared, guint source, guint target, GError **error)
{ return radios_playout_ui_start_prepared_weekly_with_loudness_origin(ctx, owner, prepared, source, target, NULL, FALSE, error); }

/* Kılavuz 2026-10-18 §06/§08 çekme adedi. Applies to programme starts made
 * afterwards on this controller and to the continuation windows of a run
 * already going; 0/0 restores the whole-programme request. */
void radios_playout_ui_set_programme_window(RadioSViewContext *ctx, guint event_fetch, guint content_fetch)
{
    RadioSPlayoutController *p = controller(ctx); if (!p) return;
    p->window_event_fetch = MIN(event_fetch, 5000u); p->window_content_fetch = MIN(content_fetch, RADIOS_AUDIO_RUN_MAX_ITEMS);
}
