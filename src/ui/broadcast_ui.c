#include "radios/broadcast_ui.h"
#include "radios/audio_ui.h"
#include "radios/audio_runner.h"
#include "radios/playout_ui.h"
#include "radios/playout.h"
#include "radios/stream_metadata.h"
#include "radios/platform.h"
#include <string.h>

typedef struct {
    gint references;
    gchar *id, *kind;
    gint64 revision;
    guint64 epoch, frames, bytes, previous_frames;
    guint64 cue_in[RADIOS_AUDIO_RUN_MAX_ITEMS];
    guint count;
    JsonNode *retry;
    gchar *queued_state, *queued_detail;
    gint64 retry_at;
    gboolean active_sent, terminal, saving, repeat;
    guint cycles;
} BroadcastRun;
struct RadioSBroadcastController {
    gint references;
    RadioSViewContext *ctx;
    RadioSAudioEngine *engine;
    RadioSAudioOutput *outputs[2];
    BroadcastRun *runs[3];
    guint64 intents[3], emergency_playout_intent;
    gboolean pending[3], closed, automatic_metadata, returning;
    guint timer;
    RadioSPlayoutResume *resume;
    /* Receipt scratch buffer (RADIOS_AUDIO_RUN_MAX_RECEIPTS entries, ~110 KiB):
     * kept on the heap, never on the 100 ms timer's stack. */
    RadioSAudioRunReceipt *receipts;
    gchar emergency_status[256], metadata_title[1025];
};
typedef struct {
    RadioSBroadcastController *controller;
    BroadcastRun *run;
    GWeakRef owner;
    guint index;
    guint64 intent, playout_intent, playout_epoch;
    gchar *password;
} BroadcastJob;
static RadioSBroadcastController *controller(RadioSViewContext *ctx)
{ return ctx && ctx->broadcast && !ctx->broadcast->closed ? ctx->broadcast : NULL; }
static RadioSBroadcastController *ref(RadioSBroadcastController *p)
{ g_atomic_int_inc(&p->references); return p; }
static BroadcastRun *run_ref(BroadcastRun *run)
{ if (run) g_atomic_int_inc(&run->references); return run; }
static void run_unref(BroadcastRun *run)
{
    if (!run || !g_atomic_int_dec_and_test(&run->references)) return;
    g_free(run->id); g_free(run->kind); g_clear_pointer(&run->retry, json_node_free);
    g_free(run->queued_state); g_free(run->queued_detail); g_free(run);
}
static gboolean invalid(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, message); return FALSE; }
static void notice(RadioSBroadcastController *p, const gchar *message)
{ if (!p->closed && p->ctx->notify) p->ctx->notify(p->ctx->notify_data, message); }
static guint kind_index(const gchar *kind)
{ return kind && g_str_equal(kind, "record") ? 0 : kind && g_str_equal(kind, "stream") ? 1 : 2; }
static void job_free(gpointer value)
{
    BroadcastJob *job = value; g_weak_ref_clear(&job->owner);
    if (job->password) { memset(job->password, 0, strlen(job->password)); g_free(job->password); }
    run_unref(job->run); radios_broadcast_ui_unref(job->controller); g_free(job);
}
static BroadcastJob *job_new(RadioSBroadcastController *p, GtkWidget *owner, guint index)
{
    BroadcastJob *job = g_new0(BroadcastJob, 1); job->controller = ref(p); job->index = index;
    job->intent = p->intents[index]; g_weak_ref_init(&job->owner, owner); return job;
}
static void event_queue(BroadcastRun *run, const gchar *state, const gchar *detail)
{
    if (run->terminal) return;
    if (!g_str_equal(state, "active")) run->terminal = TRUE;
    g_free(run->queued_state); g_free(run->queued_detail);
    run->queued_state = g_strdup(state); run->queued_detail = g_utf8_substring(detail ? detail : "", 0, MIN(g_utf8_strlen(detail ? detail : "", -1), 240));
}
static void event_saved(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; BroadcastJob *job = value; RadioSBroadcastController *p = job->controller;
    BroadcastRun *run = job->run; if (p->closed) return;
    run->saving = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        run->retry_at = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
        notice(p, error ? error->message : "Yayın işlem sonucu veritabanında doğrulanamadı."); return;
    }
    run->revision = radios_json_int(json_node_get_object(result), "revision", run->revision);
    g_clear_pointer(&run->retry, json_node_free); run->retry_at = 0;
}
static JsonNode *event_body(RadioSBroadcastController *p, BroadcastRun *run, gint64 revision)
{
    JsonNode *node = radios_data_body(p->ctx); JsonObject *body = json_node_get_object(node);
    json_object_set_string_member(body, "run_id", run->id); json_object_set_int_member(body, "expected_revision", revision);
    json_object_set_string_member(body, "state", run->queued_state); json_object_set_string_member(body, "detail", run->queued_detail);
    json_object_set_int_member(body, "frames", (gint64)run->frames); json_object_set_int_member(body, "bytes", (gint64)run->bytes);
    gchar *key = g_uuid_string_random(); json_object_set_string_member(body, "idempotency_key", key); g_free(key);
    g_clear_pointer(&run->queued_state, g_free); g_clear_pointer(&run->queued_detail, g_free); return node;
}
static void event_save(RadioSBroadcastController *p, guint index)
{
    BroadcastRun *run = p->runs[index];
    if (!run || run->saving || run->retry_at > g_get_monotonic_time() || !radios_data_connected(p->ctx)) return;
    if (!run->retry) {
        if (!run->queued_state) return;
        run->retry = event_body(p, run, run->revision);
    }
    BroadcastJob *job = job_new(p, GTK_WIDGET(p->ctx->parent), index); job->run = run_ref(run); run->saving = TRUE;
    if (!radios_data_request(p->ctx, GTK_WIDGET(p->ctx->parent), "POST", "/v1/workflows/desktop-broadcast/run-event", run->retry, event_saved, job, job_free)) {
        run->saving = FALSE; run->retry_at = g_get_monotonic_time() + G_USEC_PER_SEC;
    }
}
static void output_capture(RadioSBroadcastController *p, guint index)
{
    BroadcastRun *run = p->runs[index]; if (!run || run->terminal) return;
    RadioSAudioOutputSnapshot actual; radios_audio_output_snapshot(p->outputs[index], &actual);
    run->frames = MAX(run->frames, actual.frames_processed); run->bytes = MAX(run->bytes, actual.bytes_written);
    if (!run->active_sent && actual.state == RADIOS_AUDIO_OUTPUT_ACTIVE && actual.frames_processed) {
        event_queue(run, "active", "C motoru MAIN PCM işledi; uzak dinleyici kanıtı değildir."); run->active_sent = TRUE;
    }
    if (actual.state == RADIOS_AUDIO_OUTPUT_FAILED) event_queue(run, "failed", actual.error);
    else if (actual.state == RADIOS_AUDIO_OUTPUT_STOPPED)
        event_queue(run, run->frames && run->bytes ? "completed" : "interrupted", index == 0 ? "WAV kayıt sonlandırıldı." : "Stream aktarımı durduruldu; HTTP sonucu ayrı gösterilir.");
}
static void emergency_capture(RadioSBroadcastController *p, gboolean allow_repeat)
{
    BroadcastRun *run = p->runs[2]; if (!run || !run->epoch || run->terminal || p->pending[2]) return;
    RadioSAudioRunSnapshot actual; radios_audio_engine_runner_snapshot(p->engine, &actual);
    RadioSAudioRunReceipt *receipts = p->receipts;
    guint count = radios_audio_engine_runner_receipts(p->engine, receipts, RADIOS_AUDIO_RUN_MAX_RECEIPTS); guint64 frames = 0;
    gboolean completed[RADIOS_AUDIO_RUN_MAX_ITEMS] = {FALSE}, interrupted = FALSE, failed = FALSE;
    for (guint i = 0; i < count; ++i) if (receipts[i].epoch == run->epoch && receipts[i].index < run->count) {
        if (receipts[i].position_frames > run->cue_in[receipts[i].index]) frames += receipts[i].position_frames - run->cue_in[receipts[i].index];
        completed[receipts[i].index] |= receipts[i].terminal == RADIOS_AUDIO_RUN_TERMINAL_COMPLETE;
        interrupted |= receipts[i].terminal == RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED;
        failed |= receipts[i].terminal == RADIOS_AUDIO_RUN_TERMINAL_FAILED;
    }
    run->frames = MAX(run->frames, run->previous_frames + frames);
    if (!run->active_sent && frames) {
        event_queue(run, "active", "Doğrulanmış acil medya MAIN tarafından tüketildi."); run->active_sent = TRUE;
        g_strlcpy(p->emergency_status, run->repeat ? "Acil yayında · Sürekli tekrar açık · 1. tur" :
            "Acil yayında · MAIN kaynak tüketimi doğrulandı", sizeof p->emergency_status);
    }
    if (actual.epoch != run->epoch) {
        event_queue(run, "interrupted", "Başka bir motor komutu acil sırayı değiştirdi.");
        if (!p->returning) { radios_playout_ui_resume_free(p->resume); p->resume = NULL; }
        g_strlcpy(p->emergency_status, "Acil kesildi · operatör başka bir sırayı başlattı", sizeof p->emergency_status);
    } else if (actual.state == RADIOS_AUDIO_RUN_ERROR) {
        event_queue(run, "failed", actual.error); g_strlcpy(p->emergency_status, actual.error, sizeof p->emergency_status);
    } else if (actual.state == RADIOS_AUDIO_RUN_FINISHED || actual.state == RADIOS_AUDIO_RUN_IDLE) {
        for (guint i = 0; i < RADIOS_AUDIO_NORMAL_DECKS; ++i) if (actual.owned[i]) return;
        gboolean all_complete = run->count > 0;
        for (guint i = 0; i < run->count; ++i) all_complete &= completed[i];
        if (run->repeat && all_complete && !interrupted && !failed && !p->returning &&
            actual.state == RADIOS_AUDIO_RUN_FINISHED &&
            radios_playout_ui_intent(p->ctx) == p->emergency_playout_intent) {
            if (!allow_repeat) return;
            GError *error = NULL;
            if (radios_playout_ui_repeat_completed(p->ctx, run->epoch, &error)) {
                run->previous_frames += frames; ++run->cycles;
                radios_audio_engine_runner_snapshot(p->engine, &actual); run->epoch = actual.epoch;
                g_snprintf(p->emergency_status, sizeof p->emergency_status,
                    "Acil yayında · Sürekli tekrar · %u. tur", run->cycles + 1);
                return;
            }
            event_queue(run, "failed", error ? error->message : "Acil liste tekrar başlatılamadı.");
            g_strlcpy(p->emergency_status, error ? error->message : "Acil liste tekrar başlatılamadı.", sizeof p->emergency_status);
            g_clear_error(&error); return;
        }
        event_queue(run, failed ? "failed" : all_complete && !interrupted ? "completed" : "interrupted", "Acil sıranın gerçek MAIN tüketim makbuzları sonlandırıldı.");
        g_strlcpy(p->emergency_status, "Acil sıra sona erdi · Normal yayına dön komutunu kullanın", sizeof p->emergency_status);
    }
}
static gboolean poll(gpointer value)
{
    RadioSBroadcastController *p = value; if (p->closed) return G_SOURCE_REMOVE;
    output_capture(p, 0); output_capture(p, 1); emergency_capture(p, TRUE);
    for (guint i = 0; i < 3; ++i) event_save(p, i);
    if (p->automatic_metadata) {
        RadioSStreamMetadataSnapshot snapshot = {0};
        /* Share the generation-bound MAIN observer with routing outputs. A
         * rejected jingle/advertisement leaves the previous song at the
         * receiver; do not replace it with either the item or an empty title. */
        if (radios_stream_metadata_resolve(p->ctx, RG_METADATA_AUTOMATION, 0,
                "{title}", &snapshot) && !g_str_equal(snapshot.text, p->metadata_title)) {
            GError *error = NULL;
            if (radios_audio_output_metadata(p->outputs[1], snapshot.text, &error))
                g_strlcpy(p->metadata_title, snapshot.text, sizeof p->metadata_title);
            g_clear_error(&error);
        }
    }
    return G_SOURCE_CONTINUE;
}
void radios_broadcast_ui_checkpoint(RadioSViewContext *ctx)
{
    RadioSBroadcastController *p = controller(ctx); if (!p) return;
    emergency_capture(p, FALSE); event_save(p, 2);
}
/* One Windows file name (no separators, no ':' alternate data stream, no
 * reserved device name, no trailing dot/space) ending in ".wav" in any case. */
static gboolean recording_name_valid(const gchar *filename)
{
    gsize length = strlen(filename);
    return length > 4 && filename[0] != '.' && radios_path_component_valid(filename) &&
        g_ascii_strcasecmp(filename + length - 4, ".wav") == 0;
}
/* The recording root is never a link/junction itself; missing components are
 * created private to the current user (the Windows form of mode 0700). */
static gboolean recording_root_ready(const gchar *root)
{
    RadioSFileInfo info;
    if (!root || !radios_path_is_absolute(root)) return FALSE;
    if (radios_fs_stat(root, FALSE, &info) == 0 && info.type == RADIOS_FILE_TYPE_LINK) return FALSE;
    return radios_fs_mkdir_with_parents(root, 0700) == 0;
}
static void run_created(RadioSViewContext *ctx, GtkWidget *unused, JsonNode *result, const GError *failure, gpointer value)
{
    (void)unused; BroadcastJob *job = value; RadioSBroadcastController *p = job->controller; guint index = job->index;
    if (p->closed) return;
    gboolean current = job->intent == p->intents[index]; p->pending[index] = FALSE;
    if (failure || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        if (current) notice(p, failure ? failure->message : "Yayın işlem kaydı oluşturulamadı.");
        return;
    }
    JsonObject *response = json_node_get_object(result); const gchar *id = radios_json_string(response, "run_id", "");
    if (!g_uuid_string_is_valid(id)) { notice(p, "Yayın işlem kimliği geçersiz."); return; }
    BroadcastRun *run = g_new0(BroadcastRun, 1); run->references = 1; run->id = g_strdup(id);
    run->revision = radios_json_int(response, "revision", 0); run->kind = g_strdup(index == 0 ? "record" : index == 1 ? "stream" : "emergency");
    /* A pending run prevents a newer start until its response is accounted for. */
    run_unref(p->runs[index]); p->runs[index] = run;
    GtkWidget *owner = g_weak_ref_get(&job->owner);
    RadioSAudioRunSnapshot current_run; radios_audio_engine_runner_snapshot(p->engine, &current_run);
    if (!current || !owner || gtk_widget_in_destruction(owner) || (index == 2 &&
        (job->playout_intent != radios_playout_ui_intent(ctx) || job->playout_epoch != current_run.epoch))) {
        event_queue(run, "interrupted", "Hazırlık sırasında operatör iptal etti veya kaynak değişti."); event_save(p, index); g_clear_object(&owner); return;
    }
    GError *error = NULL; gboolean accepted = FALSE;
    if (index == 2) {
        JsonObject *playback = json_object_get_object_member(response, "playback");
        accepted = playback && radios_playout_ui_interrupt_prepared(ctx, owner, playback, &p->resume, &error);
        if (p->resume) p->emergency_playout_intent = radios_playout_ui_intent(ctx);
        if (accepted) {
            JsonObject *emergency = json_object_has_member(playback, "emergency") ? json_object_get_object_member(playback, "emergency") : NULL;
            run->repeat = emergency && json_object_has_member(emergency, "repeat") && json_object_get_boolean_member(emergency, "repeat");
            RadioSAudioRunSnapshot actual; radios_audio_engine_runner_snapshot(p->engine, &actual); run->epoch = actual.epoch;
            RadioSPlayoutRequest request = {.station_id = radios_data_station_id(ctx), .database_file = ctx->config->database_file,
                .mode = RADIOS_AUDIO_RUN_EOF, .now_utc_us = g_get_real_time(), .live = TRUE};
            RadioSPlayoutPlan *plan = radios_playout_plan_build(playback, &request, NULL);
            if (plan) {
                /* cue_in[] holds RADIOS_AUDIO_RUN_MAX_ITEMS entries (the engine's run limit). */
                run->count = MIN(plan->entries->len, RADIOS_AUDIO_RUN_MAX_ITEMS);
                for (guint i = 0; i < run->count; ++i) run->cue_in[i] = ((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, i))->run.media.cue_in_frame;
                radios_playout_plan_free(plan);
            }
            p->emergency_playout_intent = radios_playout_ui_intent(ctx);
            g_strlcpy(p->emergency_status, run->repeat ? "Acil medya hazırlanıyor · Sürekli tekrar açık" :
                "Acil medya hazırlanıyor · gerçek MAIN tüketimi bekleniyor", sizeof p->emergency_status);
        }
    } else {
        JsonObject *profile = json_object_get_object_member(response, "profile");
        JsonObject *config = profile;
        if (config && index == 0) {
            const gchar *filename = radios_json_string(config, "filename", "");
            if (recording_name_valid(filename) && recording_root_ready(ctx->config->recording_root)) {
                gchar *path = g_build_filename(ctx->config->recording_root, filename, NULL);
                accepted = radios_audio_output_start_record(p->outputs[0], path, &error); g_free(path);
            } else invalid(&error, "Kayıt adı veya proje içi kayıt dizini geçersiz.");
        } else if (config) {
            accepted = radios_audio_output_start_stream(p->outputs[1], radios_json_string(config, "url", ""),
                radios_json_string(config, "username", ""), job->password, (guint)radios_json_int(config, "bitrate_kbps", 128), &error);
            if (accepted) p->metadata_title[0] = '\0';
        }
    }
    if (!accepted) {
        event_queue(run, "failed", error ? error->message : "Yayın profili veya doğrulanmış medya geçersiz.");
        notice(p, error ? error->message : "Yayın başlatılamadı."); event_save(p, index);
    }
    g_clear_error(&error); g_object_unref(owner);
}
static gboolean begin(RadioSViewContext *ctx, GtkWidget *owner, guint index,
    const gchar *id, gint64 revision, guint queue_index, const gchar *password, GError **error)
{
    RadioSBroadcastController *p = controller(ctx);
    if (!p || !owner || gtk_widget_in_destruction(owner) || !radios_data_connected(ctx) || !g_uuid_string_is_valid(id) || revision < 1 ||
        !radios_audio_ui_ready(ctx, RADIOS_AUDIO_UI_MAIN)) return invalid(error, "Kaydedilmiş yayın profili, veritabanı veya MAIN çıkışı hazır değil.");
    BroadcastRun *previous = p->runs[index];
    if (p->pending[index] || (previous && (!previous->terminal || previous->saving || previous->retry || previous->queued_state)) ||
        (index == 2 && p->resume)) return invalid(error, "Önceki işlem ve kayıt sonucu tamamlanmalı; acil yayın için önce normal yayına dönün.");
    p->pending[index] = TRUE; ++p->intents[index];
    BroadcastJob *job = job_new(p, owner, index); job->password = g_strdup(password ? password : "");
    job->playout_intent = radios_playout_ui_intent(ctx);
    RadioSAudioRunSnapshot actual; radios_audio_engine_runner_snapshot(p->engine, &actual); job->playout_epoch = actual.epoch;
    JsonNode *body = radios_data_body(ctx); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", index == 0 ? "record" : index == 1 ? "stream" : "emergency");
    json_object_set_string_member(object, "profile_id", id); json_object_set_int_member(object, "expected_revision", revision);
    if (index == 2) json_object_set_int_member(object, "queue_index", queue_index);
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    gboolean accepted = radios_data_request(ctx, GTK_WIDGET(ctx->parent), "POST", "/v1/workflows/desktop-broadcast/run-start", body, run_created, job, job_free);
    json_node_free(body); if (!accepted) { p->pending[index] = FALSE; return invalid(error, "Yayın işlem isteği gönderilemedi."); }
    if (index == 2) g_strlcpy(p->emergency_status, "Acil doğrulama bekleniyor · normal yayın sürüyor", sizeof p->emergency_status);
    return TRUE;
}
gboolean radios_broadcast_ui_output_start(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *kind, JsonObject *profile, const gchar *password, GError **error)
{
    guint index = kind_index(kind); if (index > 1) return invalid(error, "Çıkış türü geçersiz.");
    return begin(ctx, owner, index, radios_json_string(profile, "_id", ""), radios_json_int(profile, "_rev", 0), 0, password, error);
}
gboolean radios_broadcast_ui_output_stop(RadioSViewContext *ctx, const gchar *kind, GError **error)
{
    RadioSBroadcastController *p = controller(ctx); guint index = kind_index(kind);
    if (!p || index > 1) return invalid(error, "Yayın çıkışı hazır değil.");
    ++p->intents[index]; /* Keep pending latch until cancelled request returns. */
    radios_audio_output_stop(p->outputs[index]); output_capture(p, index); event_save(p, index); return TRUE;
}
gboolean radios_broadcast_ui_metadata(RadioSViewContext *ctx, const gchar *title, GError **error)
{
    RadioSBroadcastController *p = controller(ctx); if (!p) return invalid(error, "Stream çıkışı hazır değil.");
    gboolean accepted = radios_audio_output_metadata(p->outputs[1], title, error);
    if (accepted) g_strlcpy(p->metadata_title, title, sizeof p->metadata_title);
    return accepted;
}
void radios_broadcast_ui_metadata_automatic(RadioSViewContext *ctx, gboolean enabled)
{ RadioSBroadcastController *p = controller(ctx); if (p) p->automatic_metadata = enabled; }
gboolean radios_broadcast_ui_output_snapshot(RadioSViewContext *ctx, const gchar *kind, RadioSAudioOutputSnapshot *out)
{
    RadioSBroadcastController *p = controller(ctx); guint index = kind_index(kind); if (!p || index > 1 || !out) return FALSE;
    radios_audio_output_snapshot(p->outputs[index], out); return TRUE;
}
gchar *radios_broadcast_ui_status(RadioSViewContext *ctx, const gchar *kind)
{
    RadioSBroadcastController *p = controller(ctx); guint index = kind_index(kind);
    if (kind && g_str_equal(kind, "metadata")) index = 1;
    if (!p || index > 1) return g_strdup("Çıkış hazır değil");
    if (p->pending[index]) return g_strdup("Veritabanı işlem kaydı bekleniyor");
    RadioSAudioOutputSnapshot snapshot; radios_audio_output_snapshot(p->outputs[index], &snapshot);
    const gchar *states[] = {"Kapalı", "Hazırlanıyor", "Çalışıyor", "Sonlandırılıyor", "Durduruldu", "Hata"};
    gchar *result = g_strdup_printf("%s · %.2f sn · %" G_GUINT64_FORMAT " bayt%s%s%s", states[snapshot.state],
        snapshot.frames_processed / 48000.0, snapshot.bytes_written,
        index == 0 && snapshot.recording_finalized ? " · WAV tamamlandı" : index == 1 && snapshot.response_confirmed ? " · HTTP kabul edildi" : "",
        snapshot.error[0] ? " · " : "", snapshot.error);
    if (index == 1 && snapshot.metadata_sequence) {
        gchar *with_metadata = g_strdup_printf("%s · metadata %s%s%s", result,
            snapshot.metadata_state == RADIOS_AUDIO_METADATA_SUCCESS ? "kabul edildi" : snapshot.metadata_state == RADIOS_AUDIO_METADATA_FAILED ? "hata" : "bekliyor",
            snapshot.metadata_error[0] ? ": " : "", snapshot.metadata_error); g_free(result); return with_metadata;
    }
    return result;
}
gboolean radios_broadcast_ui_emergency_start(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *id, gint64 revision, guint queue_index, GError **error)
{ return begin(ctx, owner, 2, id, revision, queue_index, "", error); }
gboolean radios_broadcast_ui_emergency_return(RadioSViewContext *ctx, GError **error)
{
    RadioSBroadcastController *p = controller(ctx); if (!p) return invalid(error, "Acil yürütücüsü hazır değil.");
    ++p->intents[2];
    if (p->pending[2]) {
        g_strlcpy(p->emergency_status, "Acil hazırlık iptal edildi · normal yayın sürüyor", sizeof p->emergency_status); return TRUE;
    }
    if (!p->resume) return invalid(error, "Bu oturumda geri dönülecek normal sıra yok.");
    if (radios_playout_ui_intent(ctx) != p->emergency_playout_intent) {
        radios_playout_ui_resume_free(p->resume); p->resume = NULL;
        return invalid(error, "Operatör başka bir sıra başlattı; eski yayın dönüşü iptal edildi.");
    }
    emergency_capture(p, FALSE);
    if (!p->resume) return invalid(error, "Acil kaynak başka komutla değişti; dönüş kaydı iptal edildi.");
    p->returning = TRUE;
    gboolean accepted = radios_playout_ui_restore(ctx, GTK_WIDGET(ctx->parent), p->resume, error);
    p->returning = FALSE;
    p->emergency_playout_intent = radios_playout_ui_intent(ctx);
    if (accepted) {
        radios_playout_ui_resume_free(p->resume); p->resume = NULL;
        g_strlcpy(p->emergency_status, "Normal yayın · kaydedilen gerçek konumdan dönüş başlatıldı", sizeof p->emergency_status);
    }
    event_save(p, 2); return accepted;
}
gchar *radios_broadcast_ui_emergency_status(RadioSViewContext *ctx)
{ RadioSBroadcastController *p = controller(ctx); return g_strdup(p ? p->emergency_status : "Acil motor hazır değil"); }
gboolean radios_broadcast_ui_emergency_pending(RadioSViewContext *ctx)
{ RadioSBroadcastController *p = controller(ctx); return p && p->pending[2]; }
RadioSBroadcastController *radios_broadcast_ui_new(RadioSViewContext *ctx)
{
    if (!ctx || !ctx->playout) return NULL;
    RadioSBroadcastController *p = g_new0(RadioSBroadcastController, 1); p->references = 1; p->ctx = ctx;
    p->engine = radios_audio_ui_engine_acquire(ctx);
    p->outputs[0] = radios_audio_engine_output_acquire(p->engine, RADIOS_AUDIO_OUTPUT_RECORD);
    p->outputs[1] = radios_audio_engine_output_acquire(p->engine, RADIOS_AUDIO_OUTPUT_STREAM);
    p->receipts = g_new(RadioSAudioRunReceipt, RADIOS_AUDIO_RUN_MAX_RECEIPTS);
    g_strlcpy(p->emergency_status, "Normal yayın · acil komut verilmedi", sizeof p->emergency_status);
    p->timer = g_timeout_add(100, poll, p); return p;
}
void radios_broadcast_ui_close(RadioSBroadcastController *p)
{
    if (!p || p->closed) return;
    output_capture(p, 0); output_capture(p, 1); emergency_capture(p, FALSE);
    for (guint i = 0; i < 3; ++i) {
        BroadcastRun *run = p->runs[i]; if (!run) continue;
        if (!run->terminal) event_queue(run, "interrupted", "Uygulama kapandı; sonlandırma sonrası ölçülmeyen kuyruk belirsizdir.");
        gint64 revision = run->revision;
        if (run->retry) {
            radios_data_preserve(p->ctx, "/v1/workflows/desktop-broadcast/run-event", run->retry);
            revision = radios_json_int(json_node_get_object(run->retry), "expected_revision", revision) + 1;
        }
        if (run->queued_state) {
            JsonNode *tail = event_body(p, run, revision);
            radios_data_preserve(p->ctx, "/v1/workflows/desktop-broadcast/run-event", tail); json_node_free(tail);
        }
    }
    for (guint i = 0; i < 2; ++i) radios_audio_output_stop(p->outputs[i]);
    p->closed = TRUE;
    for (guint i = 0; i < 3; ++i) ++p->intents[i];
    if (p->timer) { g_source_remove(p->timer); p->timer = 0; }
}
void radios_broadcast_ui_unref(RadioSBroadcastController *p)
{
    if (!p || !g_atomic_int_dec_and_test(&p->references)) return;
    radios_broadcast_ui_close(p);
    for (guint i = 0; i < 2; ++i) radios_audio_output_unref(p->outputs[i]);
    for (guint i = 0; i < 3; ++i) run_unref(p->runs[i]);
    radios_audio_engine_unref(p->engine); radios_playout_ui_resume_free(p->resume); g_free(p->receipts); g_free(p);
}
