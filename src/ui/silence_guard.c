#include "radios/silence_guard.h"
#include "radios/silence_detector.h"
#include "radios/audio.h"
#include "radios/audio_ui.h"
#include "radios/audio_runner.h"
#include "radios/broadcast_ui.h"
#include "radios/data.h"
#include <math.h>
#include <string.h>

#define GUARD_KEY "radios-silence-guard"
#define TICK_MS 100
#define SETTINGS_REFRESH_SECONDS 60
#define WIDGET_LOOKUP_SECONDS 1

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *window;
    RadioSSilenceSettings settings;
    gboolean settings_valid;
    gchar *settings_error;
    gchar *document_id; gint64 revision; guint strategy;
    RadioSSilenceDetector detector;
    RadioSAudioSnapshot *snapshot;     /* large; allocated once */
    guint timer, refresh_timer;
    gboolean fetching, refresh_again, disposed;
    gint64 fetched_us, looked_up_us;
    gchar *incident_key, *status;
    gdouble level_db[2];
    gboolean measurable, emergency_started, diagnostics_refused;
    GWeakRef meter, level_label;
    RadioSSilenceLevelSource source; gpointer source_user;
} SilenceGuard;

static SilenceGuard *guard_of(RadioSViewContext *ctx)
{ return ctx && GTK_IS_WINDOW(ctx->parent) ? g_object_get_data(G_OBJECT(ctx->parent), GUARD_KEY) : NULL; }

static void guard_free(gpointer value)
{
    SilenceGuard *g = value; g->disposed = TRUE;
    if (g->timer) g_source_remove(g->timer);
    if (g->refresh_timer) g_source_remove(g->refresh_timer);
    g_weak_ref_clear(&g->meter); g_weak_ref_clear(&g->level_label);
    g_free(g->settings_error); g_free(g->document_id); g_free(g->incident_key); g_free(g->status); g_free(g->snapshot);
    g_free(g);
}

static void notify(SilenceGuard *g, const gchar *message)
{ if (g->ctx->notify) g->ctx->notify(g->ctx->notify_data, message); }

static void set_status(SilenceGuard *g, const gchar *text)
{ g_free(g->status); g->status = g_strdup(text); }

/* ------------------------------------------------------------ diagnostics */

static void recorded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer user)
{
    (void)owner; (void)result; (void)user; SilenceGuard *g = guard_of(ctx);
    if (!g || g->disposed || !error || g->diagnostics_refused) return;
    g->diagnostics_refused = TRUE;
    gchar *message = g_strdup_printf("Sessizlik olayı tanı kaydına yazılamadı: %s (bildirimler sürer).", error->message);
    notify(g, message); g_free(message);
}

static void record_incident(SilenceGuard *g, const gchar *condition, const gchar *severity, const gchar *impact,
                            const gchar *code, const gchar *message, const gchar *action, const gchar *result)
{
    if (!radios_data_connected(g->ctx) || g->diagnostics_refused) return;
    JsonNode *body = radios_data_body(g->ctx); JsonObject *object = json_node_get_object(body);
    gchar *key = g_uuid_string_random();
    GDateTime *now = g_date_time_new_now_utc(); gchar *stamp = g_date_time_format_iso8601(now); g_date_time_unref(now);
    json_object_set_string_member(object, "event_key", key);
    json_object_set_string_member(object, "correlation_key", g->incident_key ? g->incident_key : key);
    json_object_set_string_member(object, "component", "output");
    json_object_set_string_member(object, "condition", condition);
    json_object_set_string_member(object, "severity", severity);
    json_object_set_string_member(object, "impact", impact);
    json_object_set_string_member(object, "code", code);
    json_object_set_string_member(object, "message", message);
    json_object_set_string_member(object, "occurred_at", stamp);
    JsonObject *details = json_object_new();
    json_object_set_double_member(details, "threshold_db", g->settings.threshold_db);
    json_object_set_double_member(details, "seconds", g->settings.seconds);
    json_object_set_double_member(details, "left_db", g->level_db[0]);
    json_object_set_double_member(details, "right_db", g->level_db[1]);
    json_object_set_string_member(details, "measure", g->settings.measure == RADIOS_SILENCE_MEASURE_PEAK ? "peak" : "rms");
    json_object_set_string_member(details, "monitor", g->settings.monitor);
    if (action) json_object_set_string_member(details, "action", action);
    if (result) json_object_set_string_member(details, "result", result);
    json_object_set_object_member(object, "details", details);
    radios_data_request(g->ctx, g->window, "POST", "/v1/workflows/diagnostics/record", body, recorded, NULL, NULL);
    json_node_free(body); g_free(key); g_free(stamp);
}

/* ----------------------------------------------------------------- actions */

static gchar *act(SilenceGuard *g, RadioSSilenceAction action)
{
    GError *error = NULL;
    switch (action) {
    case RADIOS_SILENCE_ACTION_NEXT_DECK: {
        RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(g->ctx);
        if (!engine) return g_strdup("ses motoru hazır değil; sıradaki parçaya geçilemedi");
        gboolean ok = radios_audio_engine_runner_next(engine, &error);
        radios_audio_engine_unref(engine);
        if (ok) return g_strdup("etkin otomasyon sırasında sıradaki parçaya geçildi");
        gchar *text = g_strdup_printf("sıradaki parçaya geçilemedi: %s", error ? error->message : "etkin otomasyon sırası yok");
        g_clear_error(&error); return text;
    }
    case RADIOS_SILENCE_ACTION_EMERGENCY_LIST:
        if (g->emergency_started) return g_strdup("acil liste bu olayda zaten başlatıldı");
        if (!g->document_id || g->revision <= 0) return g_strdup("acil hazırlık kaydedilmemiş; acil liste başlatılamadı (Acil sayfasında listeyi kaydedin)");
        if (radios_broadcast_ui_emergency_pending(g->ctx)) return g_strdup("acil hazırlık zaten sürüyor");
        if (!radios_broadcast_ui_emergency_start(g->ctx, g->window, g->document_id, g->revision, g->strategy, &error)) {
            gchar *text = g_strdup_printf("acil liste başlatılamadı: %s", error ? error->message : "bilinmeyen hata");
            g_clear_error(&error); return text;
        }
        g->emergency_started = TRUE;
        return g_strdup("acil liste MAIN çıkışına hazırlanıyor");
    case RADIOS_SILENCE_ACTION_ALARM: return g_strdup("alarm verildi; otomatik işlem yok");
    default: return g_strdup("ek işlem yok");
    }
}

static void step(SilenceGuard *g, guint number, RadioSSilenceAction action, gint64 now_us)
{
    if (number == 1) { g_free(g->incident_key); g->incident_key = g_uuid_string_random(); g->emergency_started = FALSE; }
    gchar *result = act(g, action);
    gchar *threshold = g_strdup_printf("%.0f", g->settings.threshold_db);
    gchar *message = g_strdup_printf("Sessizlik koruması · %u. basamak: MAIN çıkışı %.0f sn'dir %s dB altında → %s: %s.",
        number, radios_silence_detector_quiet_seconds(&g->detector, now_us), threshold, radios_silence_action_label(action), result);
    notify(g, message); set_status(g, message);
    record_incident(g, "failed", "critical", "on_air", number == 1 ? "silence" : "silence-step2", message,
                    radios_silence_action_label(action), result);
    g_free(message); g_free(threshold); g_free(result);
}

static void restored(SilenceGuard *g, gint64 now_us)
{
    gdouble lasted = g->detector.incident_started_us ? (gdouble)(now_us - g->detector.incident_started_us) / G_USEC_PER_SEC : 0;
    gchar *message = g_strdup_printf("Sessizlik koruması: ses geri geldi (olay %.0f sn sürdü). Acil devralma başlatıldıysa normale dönüş operatör kontrolündedir.", lasted);
    notify(g, message); set_status(g, message);
    record_incident(g, "recovered", "warning", "none", "silence-recovered", message, NULL, NULL);
    g_free(message); g_clear_pointer(&g->incident_key, g_free); g->emergency_started = FALSE;
}

/* ------------------------------------------------------------------ levels */

static gboolean engine_levels(SilenceGuard *g, gdouble *left, gdouble *right)
{
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(g->ctx);
    if (!engine) return FALSE;
    if (!g->snapshot) g->snapshot = g_new0(RadioSAudioSnapshot, 1);
    radios_audio_engine_snapshot(engine, g->snapshot);
    radios_audio_engine_unref(engine);
    const RadioSAudioBusSnapshot *bus = &g->snapshot->buses[RADIOS_AUDIO_BUS_MAIN];
    if (!g->snapshot->connected || !bus->ready) return FALSE;
    gboolean peak = g->settings.measure == RADIOS_SILENCE_MEASURE_PEAK;
    *left = peak ? bus->peak[0] : bus->rms[0]; *right = peak ? bus->peak[1] : bus->rms[1];
    return TRUE;
}

static GtkWidget *find_named(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find_named(item->data, name);
    g_list_free(children); return result;
}

static void widgets_update(SilenceGuard *g, gint64 now_us, gdouble left, gdouble right)
{
    GtkWidget *meter = g_weak_ref_get(&g->meter), *label = g_weak_ref_get(&g->level_label);
    if ((!meter || !label) && now_us - g->looked_up_us >= WIDGET_LOOKUP_SECONDS * G_USEC_PER_SEC) {
        g->looked_up_us = now_us;
        if (!meter) { meter = find_named(GTK_WIDGET(g->ctx->parent), "em-silence-meter"); if (meter) g_weak_ref_set(&g->meter, meter); if (meter) g_object_ref(meter); }
        if (!label) { label = find_named(GTK_WIDGET(g->ctx->parent), "em-silence-level"); if (label) g_weak_ref_set(&g->level_label, label); if (label) g_object_ref(label); }
    }
    if (meter) { radios_meter_set_levels(meter, left, right, left, right, left >= 1.0 || right >= 1.0); g_object_unref(meter); }
    if (label) {
        gchar *text;
        if (!g->settings_valid) text = g_strdup_printf("Sessizlik koruması kapalı: %s", g->settings_error ? g->settings_error : "ayar yok");
        else if (!g->settings.enabled) text = g_strdup("Sessizlik koruması kapalı (kutuyu işaretleyip kaydedin).");
        else if (!radios_silence_settings_local(&g->settings)) text = g_strdup_printf("%s bu bilgisayardan ölçülemez; yalnız \"Program çıkışı\" izlenir.", g->settings.monitor);
        else if (!g->measurable) text = g_strdup("Ses çıkışı bağlı değil; ölçüm yok.");
        else text = g_strdup_printf("%.1f / %.1f dB · %s · eşik %.0f dB, %.0f sn%s", g->level_db[0], g->level_db[1],
            g->detector.state == RADIOS_SILENCE_STATE_SOUND ? "ses var" :
            g->detector.state == RADIOS_SILENCE_STATE_QUIET ? "sessiz" : "ALARM", g->settings.threshold_db, g->settings.seconds,
            g->detector.state == RADIOS_SILENCE_STATE_SOUND ? "" : " · süre sayılıyor");
        gtk_label_set_text(GTK_LABEL(label), text); g_free(text); g_object_unref(label);
    }
}

static void fetch_settings(SilenceGuard *g);

static gboolean tick(gpointer value)
{
    SilenceGuard *g = value; if (g->disposed) return G_SOURCE_REMOVE;
    gint64 now = g_get_monotonic_time();
    gdouble left = 0, right = 0;
    gboolean measurable = g->source ? g->source(g->source_user, &left, &right) : engine_levels(g, &left, &right);
    g->measurable = measurable;
    gboolean active = g->settings_valid && g->settings.enabled && radios_silence_settings_local(&g->settings);
    RadioSSilenceSettings effective = g->settings; effective.enabled = active;
    RadioSSilenceEvent event = radios_silence_detector_feed(&g->detector, &effective, now, measurable, left, right);
    if (measurable) { g->level_db[0] = g->detector.level_db[0]; g->level_db[1] = g->detector.level_db[1]; }
    switch (event) {
    case RADIOS_SILENCE_EVENT_SILENCE_BEGAN: set_status(g, "Sessizlik koruması: MAIN çıkışı eşiğin altına düştü; süre sayılıyor."); break;
    case RADIOS_SILENCE_EVENT_SILENCE_ENDED: set_status(g, "Sessizlik koruması: ses süre dolmadan geri geldi."); break;
    case RADIOS_SILENCE_EVENT_STEP1: step(g, 1, g->settings.step1, now); break;
    case RADIOS_SILENCE_EVENT_STEP2: step(g, 2, g->settings.step2, now); break;
    case RADIOS_SILENCE_EVENT_RESTORED: restored(g, now); break;
    default: break;
    }
    widgets_update(g, now, measurable ? left : 0, measurable ? right : 0);
    if (!g->source && radios_data_connected(g->ctx) && now - g->fetched_us >= SETTINGS_REFRESH_SECONDS * G_USEC_PER_SEC) fetch_settings(g);
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------- settings */

gboolean radios_silence_guard_apply_preparation(RadioSViewContext *ctx, JsonObject *preparation,
                                                const gchar *document_id, gint64 revision, GError **error)
{
    SilenceGuard *g = guard_of(ctx);
    g_return_val_if_fail(g != NULL, FALSE);
    g_free(g->document_id); g->document_id = document_id && g_uuid_string_is_valid(document_id) ? g_strdup(document_id) : NULL;
    g->revision = g->document_id ? revision : 0;
    const gchar *strategies[] = {"Acil listeyi başlat", "Sıradaki olaya geç", "Çalan öğeyi sürdür"};
    JsonNode *behaviour = preparation ? json_object_get_member(preparation, "takeover/Devralma davranışı") : NULL;
    const gchar *behaviour_text = behaviour && JSON_NODE_HOLDS_VALUE(behaviour) ? json_node_get_string(behaviour) : NULL;
    g->strategy = 0;
    for (guint i = 0; behaviour_text && i < G_N_ELEMENTS(strategies); ++i) if (g_str_equal(behaviour_text, strategies[i])) g->strategy = i;
    RadioSSilenceSettings settings; GError *local = NULL;
    gboolean ok = radios_silence_settings_from_preparation(preparation, &settings, &local);
    g_clear_pointer(&g->settings_error, g_free);
    if (!ok) {
        radios_silence_settings_default(&g->settings); g->settings_valid = FALSE;
        g->settings_error = g_strdup(local ? local->message : "ayar okunamadı");
        gchar *message = g_strdup_printf("Sessizlik koruması kapalı: %s", g->settings_error);
        set_status(g, message); notify(g, message); g_free(message);
        g_propagate_error(error, local); return FALSE;
    }
    gboolean was_enabled = g->settings_valid && g->settings.enabled;
    g->settings = settings; g->settings_valid = TRUE;
    if (!settings.enabled || !radios_silence_settings_local(&settings)) {
        radios_silence_detector_reset(&g->detector);
        set_status(g, settings.enabled ? "Sessizlik koruması: izlenen nokta bu bilgisayardan ölçülemez." : "Sessizlik koruması kapalı.");
    } else if (!was_enabled) {
        gchar *message = g_strdup_printf("Sessizlik koruması açık: eşik %.0f dB, %.0f sn → %s%s%s.", settings.threshold_db, settings.seconds,
            radios_silence_action_label(settings.step1), settings.step2_seconds > 0 && settings.step2 != RADIOS_SILENCE_ACTION_NONE ? ", sonra " : "",
            settings.step2_seconds > 0 && settings.step2 != RADIOS_SILENCE_ACTION_NONE ? radios_silence_action_label(settings.step2) : "");
        set_status(g, message); g_free(message);
    }
    return TRUE;
}

static void settings_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer user)
{
    (void)owner; (void)user; SilenceGuard *g = guard_of(ctx);
    if (!g || g->disposed) return;
    g->fetching = FALSE; g->fetched_us = g_get_monotonic_time();
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        if (g->refresh_again) { g->refresh_again = FALSE; fetch_settings(g); }
        return;
    }
    JsonNode *rows = json_object_get_member(json_node_get_object(result), "records");
    JsonObject *chosen = NULL;
    for (guint i = 0; rows && JSON_NODE_HOLDS_ARRAY(rows) && i < json_array_get_length(json_node_get_array(rows)); ++i) {
        JsonObject *row = json_array_get_object_element(json_node_get_array(rows), i);
        if (!chosen || g_str_equal(radios_json_string(row, "name", ""), "Acil hazırlık")) chosen = row;
    }
    JsonNode *config = chosen ? json_object_get_member(chosen, "config") : NULL;
    JsonObject *configuration = config && JSON_NODE_HOLDS_OBJECT(config) ? json_node_get_object(config) : NULL;
    JsonNode *preparation = configuration ? json_object_get_member(configuration, "preparation") : NULL;
    radios_silence_guard_apply_preparation(ctx, preparation && JSON_NODE_HOLDS_OBJECT(preparation) ? json_node_get_object(preparation) : NULL,
        chosen ? radios_json_string(chosen, "_id", "") : NULL, chosen ? radios_json_int(chosen, "_rev", 0) : 0, NULL);
    if (g->refresh_again) { g->refresh_again = FALSE; fetch_settings(g); }
}

static void fetch_settings(SilenceGuard *g)
{
    if (g->disposed || !radios_data_connected(g->ctx)) return;
    if (g->fetching) { g->refresh_again = TRUE; return; }
    JsonNode *body = radios_data_body(g->ctx);
    json_object_set_string_member(json_node_get_object(body), "kind", "emergency");
    g->fetching = radios_data_request(g->ctx, g->window, "POST", "/v1/workflows/desktop/list", body, settings_loaded, NULL, NULL);
    json_node_free(body);
}

static gboolean refresh_due(gpointer value)
{
    SilenceGuard *g = value; g->refresh_timer = 0;
    if (!g->disposed) fetch_settings(g);
    return G_SOURCE_REMOVE;
}

static void data_refreshed(RadioSViewContext *ctx, GtkWidget *owner)
{
    (void)owner; SilenceGuard *g = guard_of(ctx);
    if (!g || g->disposed || g->source) return;
    if (!g->refresh_timer) g->refresh_timer = g_timeout_add_seconds(1, refresh_due, g);
}

/* ------------------------------------------------------------------ public */

void radios_silence_guard_bind(RadioSViewContext *ctx, GtkWidget *window)
{
    g_return_if_fail(ctx && GTK_IS_WINDOW(window));
    if (g_object_get_data(G_OBJECT(window), GUARD_KEY)) return;
    SilenceGuard *g = g_new0(SilenceGuard, 1); g->ctx = ctx; g->window = window;
    radios_silence_settings_default(&g->settings); g->settings_valid = TRUE;
    radios_silence_detector_init(&g->detector);
    g->level_db[0] = g->level_db[1] = -120;
    g_weak_ref_init(&g->meter, NULL); g_weak_ref_init(&g->level_label, NULL);
    g->status = g_strdup("Sessizlik koruması: ayarlar bekleniyor.");
    g_object_set_data_full(G_OBJECT(window), GUARD_KEY, g, guard_free);
    if (ctx->data) radios_data_register_refresh(ctx, window, data_refreshed);
    g->timer = g_timeout_add(TICK_MS, tick, g);
}

gboolean radios_silence_guard_status(RadioSViewContext *ctx, gchar **line, gdouble *left_db, gdouble *right_db)
{
    SilenceGuard *g = guard_of(ctx);
    if (!g) return FALSE;
    if (line) *line = g_strdup(g->status ? g->status : "");
    if (left_db) *left_db = g->level_db[0];
    if (right_db) *right_db = g->level_db[1];
    return TRUE;
}

void radios_silence_guard_set_level_source(RadioSViewContext *ctx, RadioSSilenceLevelSource source, gpointer user)
{
    SilenceGuard *g = guard_of(ctx);
    g_return_if_fail(g != NULL);
    g->source = source; g->source_user = user;
    radios_silence_detector_reset(&g->detector);
}
