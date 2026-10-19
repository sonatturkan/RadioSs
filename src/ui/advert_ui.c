#include "radios/advert_ui.h"
#include "radios/audio_ui.h"
#include "radios/audio_runner.h"
#include <string.h>

typedef struct {
    gint references;
    gchar *id;
    guint64 epoch;
    gint64 revision;
    JsonObject *manifest;
    JsonArray *pending;
    JsonNode *retry_body;
    GHashTable *observed;
    gint64 retry_at;
    gboolean active, saving;
} AdvertRun;
struct RadioSAdvertController {
    gint references;
    RadioSViewContext *ctx;
    RadioSAudioEngine *engine;
    GPtrArray *runs;
    AdvertRun *current;
    AdvertRun *waiting;
    JsonObject *waiting_playback;
    gint64 waiting_target;
    guint64 handover_epoch;
    gboolean handover_armed;
    GWeakRef owner;
    gulong owner_handler;
    guint timer;
    guint64 intent, playout_intent;
    gboolean closed, pending;
};
typedef struct {
    RadioSAdvertController *controller;
    AdvertRun *run;
    guint64 intent, playout_intent;
    gboolean scheduled;
} AdvertJob;

static void waiting_step(RadioSAdvertController *p);
static void waiting_cancel(RadioSAdvertController *p, const gchar *reason);

static RadioSAdvertController *ref(RadioSAdvertController *p)
{ g_atomic_int_inc(&p->references); return p; }
static AdvertRun *run_ref(AdvertRun *run)
{ g_atomic_int_inc(&run->references); return run; }
static void run_unref(gpointer value)
{
    AdvertRun *run = value;
    if (!run || !g_atomic_int_dec_and_test(&run->references)) return;
    g_free(run->id); json_object_unref(run->manifest); json_array_unref(run->pending);
    g_clear_pointer(&run->retry_body, json_node_free); g_hash_table_unref(run->observed); g_free(run);
}
static RadioSAdvertController *controller(RadioSViewContext *ctx)
{ return ctx && ctx->advertisement && !ctx->advertisement->closed ? ctx->advertisement : NULL; }
static gboolean invalid(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, message); return FALSE; }
static void notice(RadioSAdvertController *p, const gchar *message)
{ if (!p->closed && p->ctx->notify) p->ctx->notify(p->ctx->notify_data, message); }
static JsonObject *object(JsonObject *o, const gchar *name)
{
    JsonNode *node = o ? json_object_get_member(o, name) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array(JsonObject *o, const gchar *name)
{
    JsonNode *node = o ? json_object_get_member(o, name) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static JsonObject *copy_object(JsonObject *o)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, o);
    gchar *text = json_to_string(node, FALSE); json_node_free(node);
    JsonParser *parser = json_parser_new(); json_parser_load_from_data(parser, text, -1, NULL); g_free(text);
    JsonObject *result = json_object_ref(json_node_get_object(json_parser_get_root(parser))); g_object_unref(parser); return result;
}
static void owner_clear(RadioSAdvertController *p)
{
    GtkWidget *owner = g_weak_ref_get(&p->owner); gulong handler = p->owner_handler; p->owner_handler = 0;
    if (owner && handler && g_signal_handler_is_connected(owner, handler)) g_signal_handler_disconnect(owner, handler);
    g_clear_object(&owner); g_weak_ref_set(&p->owner, NULL);
}
static void job_free(gpointer value)
{
    AdvertJob *job = value; run_unref(job->run); radios_advert_ui_unref(job->controller); g_free(job);
}
static gchar *stamp(gint64 utc)
{
    GDateTime *date = g_date_time_new_from_unix_utc(utc / G_USEC_PER_SEC);
    gchar *whole = g_date_time_format(date, "%Y-%m-%dT%H:%M:%S"); g_date_time_unref(date);
    gchar *result = g_strdup_printf("%s.%06dZ", whole, (gint)(utc % G_USEC_PER_SEC)); g_free(whole); return result;
}
static void event_add(RadioSAdvertController *p, AdvertRun *run, JsonObject *entry,
    const RadioSAudioRunReceipt *receipt, const gchar *state, const gchar *reason, gint64 utc)
{
    JsonObject *event = json_object_new(); gchar *key = g_uuid_string_random(), *time = stamp(utc);
    json_object_set_string_member(event, "event_key", key); g_free(key);
    json_object_set_string_member(event, "entry_id", radios_json_string(entry, "entry_id", ""));
    json_object_set_int_member(event, "occurrence", (gint64)receipt->occurrence);
    json_object_set_string_member(event, "state", state);
    guint64 cue_in = (guint64)radios_json_int(entry, "cue_in_frame", 0);
    json_object_set_int_member(event, "consumed_frames", (gint64)(receipt->position_frames > cue_in ? receipt->position_frames - cue_in : 0));
    json_object_set_string_member(event, "occurred_at", time); g_free(time);
    json_object_set_string_member(event, "reason", reason);
    RadioSAudioSnapshot audio; radios_audio_engine_snapshot(p->engine, &audio);
    json_object_set_string_member(event, "main_sink", audio.buses[RADIOS_AUDIO_BUS_MAIN].device);
    json_array_add_object_element(run->pending, event);
}
static void capture(RadioSAdvertController *p)
{
    AdvertRun *run = p->current;
    if (!run || !run->active) return;
    /* Observe state first: if FINISHED becomes visible, the following receipt
     * read must also see its MAIN retirement. Never retire a run from a newer
     * state snapshot after reading an older, nonterminal receipt array. */
    RadioSAudioRunSnapshot actual; radios_audio_engine_runner_snapshot(p->engine, &actual);
    JsonArray *entries = array(run->manifest, "entries");
    RadioSAudioRunReceipt receipts[RADIOS_AUDIO_RUN_MAX_RECEIPTS];
    guint count = radios_audio_engine_runner_receipts(p->engine, receipts, G_N_ELEMENTS(receipts));
    gboolean current_observed = FALSE; guint64 last_occurrence = 0;
    for (guint i = 0; entries && i < count; ++i) {
        RadioSAudioRunReceipt *receipt = &receipts[i];
        if (receipt->epoch != run->epoch || receipt->index >= json_array_get_length(entries) || !receipt->first_utc_us) continue;
        last_occurrence = MAX(last_occurrence, receipt->occurrence);
        JsonObject *entry = json_array_get_object_element(entries, receipt->index);
        if (receipt->position_frames <= (guint64)radios_json_int(entry, "cue_in_frame", 0)) continue;
        current_observed |= receipt->index == actual.index;
        gchar key[32]; g_snprintf(key, sizeof key, "%" G_GUINT64_FORMAT, receipt->occurrence);
        guint flags = GPOINTER_TO_UINT(g_hash_table_lookup(run->observed, key));
        if (!(flags & 1u)) { event_add(p, run, entry, receipt, "started", "MAIN kaynak tüketimi", receipt->first_utc_us); flags |= 1u; }
        if (!(flags & 2u) && receipt->terminal != RADIOS_AUDIO_RUN_TERMINAL_NONE) {
            const gchar *state = receipt->terminal == RADIOS_AUDIO_RUN_TERMINAL_COMPLETE ? "local_complete" :
                receipt->terminal == RADIOS_AUDIO_RUN_TERMINAL_FAILED ? "failed" : "interrupted";
            event_add(p, run, entry, receipt, state, "C motoru: gerçek MAIN tüketim sonu", receipt->last_observed_utc_us); flags |= 2u;
        }
        g_hash_table_replace(run->observed, g_strdup(key), GUINT_TO_POINTER(flags));
    }
    if (actual.epoch == run->epoch && actual.state == RADIOS_AUDIO_RUN_ERROR && !current_observed &&
        entries && actual.index < json_array_get_length(entries)) {
        JsonObject *entry = json_array_get_object_element(entries, actual.index);
        RadioSAudioRunReceipt failed = {.occurrence = last_occurrence + 1,
            .position_frames = (guint64)radios_json_int(entry, "cue_in_frame", 0)};
        event_add(p, run, entry, &failed, "failed", actual.error[0] ? actual.error : "C çözücü hazırlığı başarısız; kaynak tüketilmedi.", g_get_real_time());
    }
    if (actual.epoch != run->epoch || actual.state == RADIOS_AUDIO_RUN_FINISHED || actual.state == RADIOS_AUDIO_RUN_ERROR || actual.state == RADIOS_AUDIO_RUN_IDLE)
        run->active = FALSE;
}
static void events_saved(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; AdvertJob *job = value; RadioSAdvertController *p = job->controller; AdvertRun *run = job->run;
    if (p->closed) return;
    run->saving = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        run->retry_at = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
        notice(p, error ? error->message : "Yerel reklam tüketim kaydı saklanamadı."); return;
    }
    run->revision = radios_json_int(json_node_get_object(result), "revision", run->revision);
    g_clear_pointer(&run->retry_body, json_node_free); run->retry_at = 0;
}
static void save_events(RadioSAdvertController *p, AdvertRun *run)
{
    if (run->saving || run->retry_at > g_get_monotonic_time() || !radios_data_connected(p->ctx)) return;
    if (!run->retry_body) {
        if (!json_array_get_length(run->pending)) return;
        run->retry_body = radios_data_body(p->ctx); JsonObject *body = json_node_get_object(run->retry_body);
        json_object_set_string_member(body, "run_id", run->id); json_object_set_int_member(body, "expected_revision", run->revision);
        gchar *key = g_uuid_string_random(); json_object_set_string_member(body, "idempotency_key", key); g_free(key);
        JsonArray *batch = json_array_new(); guint n = MIN(64u, json_array_get_length(run->pending));
        for (guint i = 0; i < n; ++i) json_array_add_object_element(batch, json_object_ref(json_array_get_object_element(run->pending, i)));
        for (guint i = 0; i < n; ++i) json_array_remove_element(run->pending, 0);
        json_object_set_array_member(body, "events", batch);
    }
    AdvertJob *job = g_new0(AdvertJob, 1); job->controller = ref(p); job->run = run_ref(run); run->saving = TRUE;
    if (!radios_data_request(p->ctx, GTK_WIDGET(p->ctx->parent), "POST", "/v1/workflows/desktop-advert/run-event", run->retry_body, events_saved, job, job_free)) {
        run->saving = FALSE; run->retry_at = g_get_monotonic_time() + G_USEC_PER_SEC;
    }
}
void radios_advert_ui_checkpoint(RadioSViewContext *ctx)
{
    RadioSAdvertController *p = controller(ctx); if (!p) return;
    capture(p);
    for (guint i = 0; i < p->runs->len; ++i) save_events(p, g_ptr_array_index(p->runs, i));
}
static gboolean poll(gpointer value)
{
    RadioSAdvertController *p = value;
    if (p->closed) return G_SOURCE_REMOVE;
    radios_advert_ui_checkpoint(p->ctx);
    waiting_step(p);
    for (guint i = p->runs->len; i > 0; --i) {
        AdvertRun *run = g_ptr_array_index(p->runs, i - 1);
        if (run != p->waiting && !run->active && !run->saving && !run->retry_body && !json_array_get_length(run->pending)) {
            if (p->current == run) p->current = NULL;
            g_ptr_array_remove_index(p->runs, i - 1);
        }
    }
    return G_SOURCE_CONTINUE;
}
static void unexecuted(RadioSAdvertController *p, AdvertRun *run, guint first, const gchar *reason)
{
    JsonArray *entries = array(run->manifest, "entries");
    RadioSAudioRunReceipt receipt = {.occurrence = 1};
    for (guint i = first; entries && i < json_array_get_length(entries); ++i) {
        JsonObject *entry = json_array_get_object_element(entries, i);
        receipt.position_frames = (guint64)radios_json_int(entry, "cue_in_frame", 0);
        event_add(p, run, entry, &receipt, "notexecuted", reason, g_get_real_time());
    }
    save_events(p, run);
}
static void waiting_cancel(RadioSAdvertController *p, const gchar *reason)
{
    if (p->handover_armed) radios_audio_engine_runner_handover_cancel(p->engine, p->handover_epoch);
    p->handover_armed = FALSE;
    AdvertRun *run = p->waiting; p->waiting = NULL;
    g_clear_pointer(&p->waiting_playback, json_object_unref);
    p->pending = FALSE;
    if (run) unexecuted(p, run, 0, reason);
}
static void waiting_step(RadioSAdvertController *p)
{
    if (!p->waiting || p->closed) return;
    if (p->playout_intent != radios_playout_ui_intent(p->ctx)) {
        waiting_cancel(p, "Başka bir oynatma komutu bekleyen kuşağı iptal etti."); owner_clear(p); return;
    }
    GtkWidget *owner = g_weak_ref_get(&p->owner);
    if (!owner || gtk_widget_in_destruction(owner)) {
        g_clear_object(&owner); waiting_cancel(p, "Kuşak düzenleyicisi kapatıldı."); owner_clear(p); return;
    }
    RadioSAudioRunSnapshot actual; radios_audio_engine_runner_snapshot(p->engine, &actual);
    if (actual.state == RADIOS_AUDIO_RUN_ERROR) {
        notice(p, actual.error[0] ? actual.error : "Mevcut programda ses hatası var; kuşak başlatılmadı.");
        waiting_cancel(p, "Mevcut C ses kuyruğu hatası nedeniyle kuşak çalınmadı.");
        owner_clear(p); g_object_unref(owner); return;
    }
    gboolean owned = FALSE;
    for (guint i = 0; i < RADIOS_AUDIO_NORMAL_DECKS; ++i) owned |= actual.owned[i];
    gboolean finished = actual.state == RADIOS_AUDIO_RUN_IDLE || actual.state == RADIOS_AUDIO_RUN_FINISHED;
    GError *failure = NULL;
    if (!finished) {
        if (!p->handover_armed) {
            gint mode = (gint)radios_json_int(p->waiting->manifest, "start_mode", 0);
            guint64 wait = mode == 2 ? (guint64)radios_json_int(p->waiting->manifest, "wait_seconds", 30) * 48000 : 0;
            if (!radios_audio_engine_runner_handover(p->engine, actual.epoch, p->waiting_target,
                mode == 1 ? RADIOS_AUDIO_RUN_START_CUT : RADIOS_AUDIO_RUN_START_FINISH, wait, 240, &failure)) {
                notice(p, failure ? failure->message : "Kuşak geçişi kurulamadı.");
                g_clear_error(&failure); waiting_cancel(p, "C motorunda kuşak geçişi kurulamadı.");
            } else { p->handover_armed = TRUE; p->handover_epoch = actual.epoch; }
        } else if (actual.epoch != p->handover_epoch || !actual.handover_pending) {
            waiting_cancel(p, "Manuel transport bekleyen kuşak geçişini iptal etti."); owner_clear(p);
        }
        g_object_unref(owner); return;
    }
    if (owned) { g_object_unref(owner); return; } /* wait for MAIN retirement acknowledgements */
    if (p->handover_armed && (actual.epoch != p->handover_epoch || !actual.handover_completed)) {
        waiting_cancel(p, "Manuel oynatıcı komutu kuşak geçişini iptal etti."); owner_clear(p); g_object_unref(owner); return;
    }
    AdvertRun *run = p->waiting; JsonObject *playback = p->waiting_playback;
    p->waiting = NULL; p->waiting_playback = NULL; p->handover_armed = FALSE; p->pending = FALSE;
    gint64 target = MAX(p->waiting_target, (gint64)0);
    if (!radios_playout_ui_start_prepared(p->ctx, owner, playback, target, &failure)) {
        JsonObject *first = json_array_get_object_element(array(run->manifest, "entries"), 0);
        RadioSAudioRunReceipt receipt = {.occurrence = 1, .position_frames = (guint64)radios_json_int(first, "cue_in_frame", 0)};
        event_add(p, run, first, &receipt, "failed", failure ? failure->message : "C motoru başlatılamadı; kaynak tüketilmedi.", g_get_real_time());
        unexecuted(p, run, 1, "Kuşak motoru başlatılamadığı için çalınmadı.");
        notice(p, failure ? failure->message : "Kuşak başlatılamadı."); g_clear_error(&failure); owner_clear(p);
    } else {
        run->active = TRUE; radios_audio_engine_runner_snapshot(p->engine, &actual); run->epoch = actual.epoch;
        p->current = run; notice(p, "Reklam kuşağı C motorunda; MAIN kaynak tüketimi ayrı kanıt sınıfıyla kaydediliyor.");
    }
    json_object_unref(playback); g_object_unref(owner);
}
static void owner_destroyed(GtkWidget *owner, gpointer value)
{
    (void)owner; RadioSAdvertController *p = value; if (p->closed) return;
    RadioSAudioRunSnapshot actual; radios_audio_engine_runner_snapshot(p->engine, &actual);
    if (!p->pending && p->current && p->current->active && actual.epoch == p->current->epoch)
        radios_playout_ui_action(p->ctx, RADIOS_PLAYOUT_STOP, NULL);
    waiting_cancel(p, "Kuşak düzenleyicisi kapatıldı; kaynak çalınmadı.");
    ++p->intent; p->pending = FALSE; p->owner_handler = 0; g_weak_ref_set(&p->owner, NULL); capture(p);
}
static void run_created(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)owner;
    AdvertJob *job = value; RadioSAdvertController *p = job->controller;
    if (p->closed || job->intent != p->intent) return;
    if (job->playout_intent != radios_playout_ui_intent(ctx)) { p->pending = FALSE; owner_clear(p); return; }
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        p->pending = FALSE; owner_clear(p); notice(p, error ? error->message : "Kuşak çalma kaydı oluşturulamadı."); return;
    }
    JsonObject *response = json_node_get_object(result), *manifest = object(response, "manifest"), *playback = object(response, "playback");
    JsonArray *entries = array(manifest, "entries"); const gchar *run_id = radios_json_string(response, "run_id", "");
    if (!g_uuid_string_is_valid(run_id) || !playback || !entries || !json_array_get_length(entries) || json_array_get_length(entries) > 500) {
        p->pending = FALSE; owner_clear(p); notice(p, "Kuşak çalma yanıtındaki medya manifesti geçersiz."); return;
    }
    gint64 start = job->scheduled ? radios_json_int(manifest, "scheduled_utc_us", 0) : g_get_real_time();
    if (job->scheduled && start <= 0) { p->pending = FALSE; owner_clear(p); notice(p, "Kuşağın UTC başlangıcı doğrulanamadı."); return; }
    AdvertRun *run = g_new0(AdvertRun, 1); run->references = 1; run->id = g_strdup(run_id);
    run->revision = radios_json_int(response, "revision", 1); run->manifest = copy_object(manifest);
    run->pending = json_array_new(); run->observed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_ptr_array_add(p->runs, run);
    p->waiting = run; p->waiting_playback = copy_object(playback); p->waiting_target = start;
    waiting_step(p);
}
gboolean radios_advert_ui_start(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *prepared,
    gboolean scheduled, GError **error)
{
    RadioSAdvertController *p = controller(ctx);
    if (!p || !owner || gtk_widget_in_destruction(owner) || !radios_data_connected(ctx) || !prepared)
        return invalid(error, "Reklam kuşağı, veritabanı veya C ses motoru hazır değil.");
    const gchar *document_id = radios_json_string(prepared, "break_document_id", "");
    const gchar *hash = radios_json_string(prepared, "manifest_hash", "");
    gint64 revision = radios_json_int(prepared, "break_revision", 0);
    if (!g_uuid_string_is_valid(document_id) || strlen(hash) != 64 || revision < 1)
        return invalid(error, "Önce rezervasyonu ve gerçek kuşak medyasını hazırlayın.");
    if (p->runs->len >= 8) return invalid(error, "Bekleyen reklam geçmişi kayıtları tamamlanmadan yeni kuşak başlatılamıyor.");
    waiting_cancel(p, "Yeni kuşak hazırlığı önceki bekleyen kuşağı iptal etti.");
    ++p->intent; owner_clear(p);
    p->pending = TRUE; p->playout_intent = radios_playout_ui_intent(ctx);
    g_weak_ref_set(&p->owner, owner); p->owner_handler = g_signal_connect(owner, "destroy", G_CALLBACK(owner_destroyed), p);
    AdvertJob *job = g_new0(AdvertJob, 1); job->controller = ref(p); job->intent = p->intent; job->playout_intent = p->playout_intent; job->scheduled = scheduled;
    JsonNode *body = radios_data_body(ctx); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "break_document_id", document_id); json_object_set_int_member(o, "expected_revision", revision);
    json_object_set_string_member(o, "manifest_hash", hash); gchar *key = g_uuid_string_random(); json_object_set_string_member(o, "idempotency_key", key); g_free(key);
    gboolean queued = radios_data_request(ctx, owner, "POST", "/v1/workflows/desktop-advert/run-start", body, run_created, job, job_free);
    json_node_free(body); if (!queued) p->pending = FALSE; return queued;
}
gboolean radios_advert_ui_action(RadioSViewContext *ctx, RadioSPlayoutAction action, GError **error)
{
    RadioSAdvertController *p = controller(ctx); if (!p) return invalid(error, "Reklam yürütücüsü hazır değil.");
    gboolean cancel_only = action == RADIOS_PLAYOUT_STOP && p->pending;
    if (action == RADIOS_PLAYOUT_STOP) { ++p->intent; waiting_cancel(p, "Operatör bekleyen kuşağı durdurdu."); owner_clear(p); }
    else if (p->pending) return invalid(error, "Kuşak kaydı hazırlanıyor; iptal etmek için Durdur kullanın.");
    gboolean result = cancel_only || radios_playout_ui_action(ctx, action, error); capture(p);
    for (guint i = 0; i < p->runs->len; ++i) save_events(p, g_ptr_array_index(p->runs, i));
    return result;
}
gboolean radios_advert_ui_pending(RadioSViewContext *ctx)
{ RadioSAdvertController *p = controller(ctx); return p && p->pending; }
RadioSAdvertController *radios_advert_ui_new(RadioSViewContext *ctx)
{
    if (!ctx || !ctx->playout) return NULL;
    RadioSAdvertController *p = g_new0(RadioSAdvertController, 1); p->references = 1; p->ctx = ctx;
    p->engine = radios_audio_ui_engine_acquire(ctx); p->runs = g_ptr_array_new_with_free_func(run_unref);
    g_weak_ref_init(&p->owner, NULL); p->timer = g_timeout_add(50, poll, p); return p;
}
void radios_advert_ui_close(RadioSAdvertController *p)
{
    if (!p || p->closed) return;
    waiting_cancel(p, "Uygulama kapanırken bekleyen kuşak çalınmadı.");
    radios_audio_engine_runner_stop(p->engine, NULL); radios_advert_ui_checkpoint(p->ctx);
    /* Flush every known receipt into the durable outbox, including receipts
     * collected while the previous batch's acknowledgement was in flight. */
    for (guint i = 0; i < p->runs->len; ++i) {
        AdvertRun *run = g_ptr_array_index(p->runs, i); gint64 revision = run->revision;
        if (run->retry_body) {
            radios_data_preserve(p->ctx, "/v1/workflows/desktop-advert/run-event", run->retry_body);
            revision = radios_json_int(json_node_get_object(run->retry_body), "expected_revision", revision) + 1;
        }
        while (json_array_get_length(run->pending)) {
            JsonNode *node = radios_data_body(p->ctx); JsonObject *body = json_node_get_object(node);
            json_object_set_string_member(body, "run_id", run->id); json_object_set_int_member(body, "expected_revision", revision++);
            gchar *key = g_uuid_string_random(); json_object_set_string_member(body, "idempotency_key", key); g_free(key);
            JsonArray *batch = json_array_new(); guint n = MIN(64u, json_array_get_length(run->pending));
            for (guint j = 0; j < n; ++j) json_array_add_object_element(batch, json_object_ref(json_array_get_object_element(run->pending, j)));
            json_object_set_array_member(body, "events", batch);
            gboolean stored = radios_data_preserve(p->ctx, "/v1/workflows/desktop-advert/run-event", node); json_node_free(node);
            if (!stored) break;
            for (guint j = 0; j < n; ++j) json_array_remove_element(run->pending, 0);
        }
    }
    p->closed = TRUE; ++p->intent; p->pending = FALSE; owner_clear(p);
    if (p->timer) { g_source_remove(p->timer); p->timer = 0; }
}
void radios_advert_ui_unref(RadioSAdvertController *p)
{
    if (!p || !g_atomic_int_dec_and_test(&p->references)) return;
    radios_advert_ui_close(p); radios_audio_engine_unref(p->engine); g_ptr_array_unref(p->runs); g_weak_ref_clear(&p->owner); g_free(p);
}
