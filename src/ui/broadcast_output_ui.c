#include "radios/broadcast_output_ui.h"
#include "radios/broadcast_ui.h"
#include "radios/data.h"
#include "radios/widgets.h"
#include <string.h>

typedef struct OutputPanel OutputPanel;
/* Status light states: the real worker state, never a timer or a guess. */
enum { LIGHT_OFF = 0, LIGHT_CONNECTING, LIGHT_ON, LIGHT_ERROR };
typedef struct {
    OutputPanel *panel;
    const gchar *kind;
    GtkWidget *owner, *fields[3], *password, *password_save, *status, *light;
    RadioSDocument *document;
    gboolean loading;
    gint64 notice_until;
    gint light_state;
    gboolean light_dim;
} OutputProfile;
struct OutputPanel {
    RadioSViewContext *ctx;
    GtkWidget *root;
    OutputProfile profiles[3];
    guint timer;
    gboolean disposed, initialized;
};
enum { RUN_KIND = 0, RUN_NAME, RUN_STATE, RUN_TIME, RUN_ID, RUN_COLUMNS };
#define RUNS_REFRESH_TICKS 40u /* the runs list ticks every 250 ms */
static const gchar *const bitrates[] = {"64", "96", "128", "160", "192", "256", "320", NULL};

static void pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static GtkWidget *named_entry(const gchar *name, const gchar *placeholder)
{
    GtkWidget *entry = radios_ui_entry(placeholder);
    gtk_widget_set_name(entry, name);
    return entry;
}
static void notice(OutputProfile *profile, const gchar *text)
{
    gtk_label_set_text(GTK_LABEL(profile->status), text);
    profile->notice_until = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    if (profile->panel->ctx->notify) profile->panel->ctx->notify(profile->panel->ctx->notify_data, text);
}
static JsonNode *configuration(OutputProfile *profile)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(node, object);
    if (g_str_equal(profile->kind, "stream")) {
        json_object_set_string_member(object, "url", gtk_entry_get_text(GTK_ENTRY(profile->fields[0])));
        json_object_set_string_member(object, "username", gtk_entry_get_text(GTK_ENTRY(profile->fields[1])));
        gchar *bitrate = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(profile->fields[2]));
        json_object_set_int_member(object, "bitrate_kbps", bitrate ? g_ascii_strtoll(bitrate, NULL, 10) : 128); g_free(bitrate);
        /* The password enters the stored profile only when the operator asked
         * for it; otherwise the document stays free of credentials as before. */
        if (profile->password_save && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(profile->password_save))) {
            json_object_set_string_member(object, "password", profile->password ? gtk_entry_get_text(GTK_ENTRY(profile->password)) : "");
            json_object_set_boolean_member(object, "password_saved", TRUE);
        }
    } else if (g_str_equal(profile->kind, "record")) {
        json_object_set_string_member(object, "filename", gtk_entry_get_text(GTK_ENTRY(profile->fields[0])));
    } else {
        json_object_set_string_member(object, "title", gtk_entry_get_text(GTK_ENTRY(profile->fields[0])));
        json_object_set_boolean_member(object, "automatic", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(profile->fields[1])));
    }
    return node;
}
static void changed(GtkWidget *widget, gpointer data)
{
    OutputProfile *profile = data;
    if (profile->loading || profile->panel->disposed) return;
    /* A session-only password is not part of the document: typing it must
     * not block "Başlat" behind a save. It becomes document content only
     * while "Şifreyi kaydet" is ticked. */
    if (widget && widget == profile->password && profile->password_save &&
        !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(profile->password_save))) return;
    radios_data_document_dirty(profile->document, TRUE);
}
static void apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; (void)owner; OutputProfile *profile = data;
    /* RadioSDocument protects newer edits from stale reads/save replies.
     * A second dirty flag here would reject the successful save ACK forever,
     * leaving even a durably saved profile unable to start. */
    profile->loading = TRUE;
    if (g_str_equal(profile->kind, "stream")) {
        gtk_entry_set_text(GTK_ENTRY(profile->fields[0]), radios_json_string(config, "url", ""));
        gtk_entry_set_text(GTK_ENTRY(profile->fields[1]), radios_json_string(config, "username", "source"));
        for (guint i = 0; bitrates[i]; ++i)
            if (g_ascii_strtoll(bitrates[i], NULL, 10) == radios_json_int(config, "bitrate_kbps", 128))
                gtk_combo_box_set_active(GTK_COMBO_BOX(profile->fields[2]), (gint)i);
        gboolean saved = json_object_has_member(config, "password_saved") &&
            JSON_NODE_HOLDS_VALUE(json_object_get_member(config, "password_saved")) &&
            json_node_get_value_type(json_object_get_member(config, "password_saved")) == G_TYPE_BOOLEAN &&
            json_object_get_boolean_member(config, "password_saved");
        if (profile->password_save) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(profile->password_save), saved);
        /* A saved password returns on every load; a session password typed
         * into the form is kept as it is. */
        if (saved && profile->password) gtk_entry_set_text(GTK_ENTRY(profile->password), radios_json_string(config, "password", ""));
    } else if (g_str_equal(profile->kind, "record")) {
        gtk_entry_set_text(GTK_ENTRY(profile->fields[0]), radios_json_string(config, "filename", ""));
    } else {
        gtk_entry_set_text(GTK_ENTRY(profile->fields[0]), radios_json_string(config, "title", ""));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(profile->fields[1]), json_object_get_boolean_member(config, "automatic"));
        radios_broadcast_ui_metadata_automatic(profile->panel->ctx, json_object_get_boolean_member(config, "automatic"));
    }
    profile->loading = FALSE;
}
static void save(GtkButton *button, gpointer data)
{
    (void)button; OutputProfile *profile = data;
    /* The stored profile accepts at most 256 UTF-8 bytes (server and durable
     * outbox agree); refuse locally instead of queueing a rejected save. */
    if (profile->password_save && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(profile->password_save)) &&
        strlen(gtk_entry_get_text(GTK_ENTRY(profile->password))) > 256) {
        notice(profile, "Kaydedilecek şifre en çok 256 bayt olabilir."); return;
    }
    JsonNode *config = configuration(profile);
    if (radios_data_document_save(profile->document, config)) notice(profile, "Ayar veritabanına kaydediliyor.");
    else notice(profile, "Veritabanı kaydı hazır değil; girdileriniz korunuyor.");
    json_node_free(config);
}
static void start(GtkButton *button, gpointer data)
{
    (void)button; OutputProfile *profile = data;
    gint64 revision = radios_data_document_revision(profile->document);
    if (revision <= 0) {
        notice(profile, "Önce ayarı kaydedin ve veritabanı kayıt sonucunu bekleyin."); return;
    }
    GError *error = NULL; gboolean accepted;
    JsonNode *config = configuration(profile);
    if (g_str_equal(profile->kind, "metadata")) {
        accepted = radios_broadcast_ui_metadata(profile->panel->ctx, gtk_entry_get_text(GTK_ENTRY(profile->fields[0])), &error);
    } else {
        JsonObject *record = json_object_new();
        json_object_set_string_member(record, "_id", radios_data_document_id(profile->document));
        json_object_set_int_member(record, "_rev", revision);
        json_object_set_member(record, "config", json_node_copy(config));
        const gchar *password = profile->password ? gtk_entry_get_text(GTK_ENTRY(profile->password)) : "";
        accepted = radios_broadcast_ui_output_start(profile->panel->ctx, profile->owner, profile->kind, record, password, &error);
        json_object_unref(record);
        /* The form keeps its password after a start: a later reconnect or
         * restart must not require typing it again. */
    }
    json_node_free(config);
    if (!accepted) { notice(profile, error ? error->message : "Çıkış başlatılamadı."); g_clear_error(&error); }
}
static void stop(GtkButton *button, gpointer data)
{
    (void)button; OutputProfile *profile = data; GError *error = NULL;
    if (!radios_broadcast_ui_output_stop(profile->panel->ctx, profile->kind, &error)) {
        notice(profile, error ? error->message : "Çıkış durdurulamadı."); g_clear_error(&error);
    }
}
static GtkWidget *action(OutputProfile *profile, GtkWidget *box, const gchar *label, const gchar *suffix, GCallback callback)
{
    GtkWidget *button = radios_ui_button(label); gchar *name = g_strdup_printf("broadcast-%s-%s", profile->kind, suffix);
    gtk_widget_set_name(button, name); g_free(name);
    g_signal_connect(button, "clicked", callback, profile); pack(box, button, FALSE); return button;
}
static void labelled(GtkWidget *box, const gchar *label, GtkWidget *control)
{
    GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "em-field");
    pack(row, radios_ui_label(label, "em-field-label"), FALSE); pack(row, control, TRUE); pack(box, row, FALSE);
}
static GtkWidget *profile_new(OutputPanel *panel, guint index)
{
    static const gchar *const kinds[] = {"stream", "record", "metadata"};
    static const gchar *const names[] = {"İnternet yayını", "MAIN yayın kaydı", "Yayın bilgisi"};
    OutputProfile *profile = &panel->profiles[index]; profile->panel = panel; profile->kind = kinds[index];
    GtkWidget *box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "em-card"); profile->owner = box;
    gchar *profile_name = g_strdup_printf("broadcast-profile-%s", profile->kind); gtk_widget_set_name(box, profile_name); g_free(profile_name);
    pack(box, radios_ui_label(names[index], "em-card-title"), FALSE);
    if (index == 0) {
        profile->fields[0] = named_entry("broadcast-stream-url", "http(s)://sunucu:port/mount");
        profile->fields[1] = named_entry("broadcast-stream-username", "source");
        gtk_entry_set_text(GTK_ENTRY(profile->fields[1]), "source");
        profile->password = named_entry("broadcast-stream-password", "Yayın şifresi");
        gtk_entry_set_visibility(GTK_ENTRY(profile->password), FALSE);
        gtk_entry_set_input_purpose(GTK_ENTRY(profile->password), GTK_INPUT_PURPOSE_PASSWORD);
        gtk_entry_set_max_length(GTK_ENTRY(profile->password), 256);
        gtk_widget_set_tooltip_text(profile->password, "Şifreyi kaydet işaretli değilse şifre yalnızca bu oturumda kullanılır.");
        profile->password_save = gtk_check_button_new_with_label("Şifreyi kaydet");
        gtk_widget_set_name(profile->password_save, "broadcast-stream-password-save");
        gtk_widget_set_tooltip_text(profile->password_save, "İşaretliyse şifre yayın profiliyle veritabanına kaydedilir ve yeniden yüklemede geri gelir.");
        profile->fields[2] = radios_ui_combo(bitrates, 2);
        gtk_widget_set_name(profile->fields[2], "broadcast-stream-bitrate");
        labelled(box, "Akış adresi", profile->fields[0]); labelled(box, "Kullanıcı", profile->fields[1]); labelled(box, "Şifre", profile->password);
        pack(box, profile->password_save, FALSE);
        labelled(box, "MP3 · kbit/sn", profile->fields[2]);
        g_signal_connect(profile->password, "changed", G_CALLBACK(changed), profile);
        g_signal_connect(profile->password_save, "toggled", G_CALLBACK(changed), profile);
    } else if (index == 1) {
        profile->fields[0] = named_entry("broadcast-record-filename", "yayın.wav");
        GDateTime *now = g_date_time_new_now_utc(); gchar *filename = g_date_time_format(now, "RadioS-%Y%m%d-%H%M%S.wav");
        gtk_entry_set_text(GTK_ENTRY(profile->fields[0]), filename); g_free(filename); g_date_time_unref(now);
        labelled(box, "Dosya adı", profile->fields[0]);
    } else {
        profile->fields[0] = named_entry("broadcast-metadata-title", "Yayındaki içerik");
        gtk_entry_set_max_length(GTK_ENTRY(profile->fields[0]), 512);
        profile->fields[1] = gtk_check_button_new_with_label("Çalan içerik bilgisini otomatik gönder");
        gtk_widget_set_name(profile->fields[1], "broadcast-metadata-automatic");
        labelled(box, "Başlık", profile->fields[0]); pack(box, profile->fields[1], FALSE);
    }
    GtkWidget *buttons = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "em-row");
    action(profile, buttons, "Kaydet", "save", G_CALLBACK(save));
    action(profile, buttons, index == 2 ? "Bilgiyi gönder" : "Başlat", "start", G_CALLBACK(start));
    if (index != 2) action(profile, buttons, "Durdur", "stop", G_CALLBACK(stop));
    pack(box, buttons, FALSE);
    GtkWidget *status_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "em-row");
    profile->light = radios_ui_label("●", "broadcast-light");
    gchar *light_name = g_strdup_printf("broadcast-%s-light", profile->kind); gtk_widget_set_name(profile->light, light_name); g_free(light_name);
    gtk_style_context_add_class(gtk_widget_get_style_context(profile->light), "broadcast-light-off");
    gtk_widget_set_tooltip_text(profile->light, "Kapalı");
    pack(status_row, profile->light, FALSE);
    profile->status = radios_ui_label("Hazır", "em-form-status");
    gchar *status_name = g_strdup_printf("broadcast-%s-status", profile->kind); gtk_widget_set_name(profile->status, status_name); g_free(status_name);
    gtk_label_set_ellipsize(GTK_LABEL(profile->status), PANGO_ELLIPSIZE_END); pack(status_row, profile->status, TRUE);
    pack(box, status_row, FALSE);
    for (guint i = 0; i < G_N_ELEMENTS(profile->fields); ++i) if (profile->fields[i])
        g_signal_connect(profile->fields[i], GTK_IS_TOGGLE_BUTTON(profile->fields[i]) ? "toggled" : "changed", G_CALLBACK(changed), profile);
    gtk_widget_set_sensitive(box, FALSE);
    return box;
}
static void expanded(GObject *object, GParamSpec *spec, gpointer data)
{
    (void)spec; OutputPanel *panel = data;
    if (panel->disposed || panel->initialized || !gtk_expander_get_expanded(GTK_EXPANDER(object))) return;
    panel->initialized = TRUE;
    static const gchar *const kinds[] = {"broadcast_stream", "broadcast_record", "broadcast_metadata"};
    static const gchar *const names[] = {"İnternet yayını", "MAIN yayın kaydı", "Yayın bilgisi"};
    for (guint i = 0; i < G_N_ELEMENTS(panel->profiles); ++i) {
        OutputProfile *profile = &panel->profiles[i];
        profile->document = radios_data_document_new(panel->ctx, profile->owner, kinds[i], names[i], apply, profile);
    }
}
/* Real state of this profile's worker. A stream counts as connected only
 * once the server accepted the source (2xx / OK reply), not when the local
 * encoder started; failures stay red until the worker reports otherwise. */
static gint light_state_of(OutputProfile *profile, gchar *detail, gsize size)
{
    RadioSAudioOutputSnapshot s;
    gboolean metadata = g_str_equal(profile->kind, "metadata");
    if (!radios_broadcast_ui_output_snapshot(profile->panel->ctx, metadata ? "stream" : profile->kind, &s)) {
        g_strlcpy(detail, "Çıkış hazır değil", size); return LIGHT_OFF;
    }
    if (metadata) {
        if (s.state != RADIOS_AUDIO_OUTPUT_STARTING && s.state != RADIOS_AUDIO_OUTPUT_ACTIVE) { g_strlcpy(detail, "Yayın kapalı", size); return LIGHT_OFF; }
        switch (s.metadata_state) {
        case RADIOS_AUDIO_METADATA_PENDING: g_strlcpy(detail, "Şarkı bilgisi gönderiliyor…", size); return LIGHT_CONNECTING;
        case RADIOS_AUDIO_METADATA_SUCCESS: g_snprintf(detail, size, "Şarkı bilgisi kabul edildi · HTTP %ld", s.metadata_http_status); return LIGHT_ON;
        case RADIOS_AUDIO_METADATA_FAILED: g_snprintf(detail, size, "Şarkı bilgisi gönderilemedi%s%s", s.metadata_error[0] ? ": " : "", s.metadata_error); return LIGHT_ERROR;
        default: g_strlcpy(detail, "Henüz bilgi gönderilmedi", size); return LIGHT_OFF;
        }
    }
    gboolean stream = g_str_equal(profile->kind, "stream");
    switch (s.state) {
    case RADIOS_AUDIO_OUTPUT_STARTING:
        g_strlcpy(detail, stream ? "Bağlanıyor…" : "Kayıt açılıyor…", size); return LIGHT_CONNECTING;
    case RADIOS_AUDIO_OUTPUT_ACTIVE:
        if (stream && !s.response_confirmed) { g_strlcpy(detail, "Sunucu yanıtı bekleniyor…", size); return LIGHT_CONNECTING; }
        if (stream) g_snprintf(detail, size, "Bağlı · HTTP %ld · %.2f MB gönderildi", s.http_status, s.bytes_written / 1048576.0);
        else g_snprintf(detail, size, "Kayıt sürüyor · %.1f sn", s.frames_processed / 48000.0);
        return LIGHT_ON;
    case RADIOS_AUDIO_OUTPUT_STOPPING: g_strlcpy(detail, "Sonlandırılıyor…", size); return LIGHT_CONNECTING;
    case RADIOS_AUDIO_OUTPUT_FAILED: g_snprintf(detail, size, "Hata%s%s", s.error[0] ? ": " : "", s.error); return LIGHT_ERROR;
    default: g_strlcpy(detail, s.state == RADIOS_AUDIO_OUTPUT_STOPPED ? "Durduruldu" : "Kapalı", size); return LIGHT_OFF;
    }
}
static void light_update(OutputProfile *profile)
{
    static const gchar *const classes[] = {"broadcast-light-off", "broadcast-light-connecting", "broadcast-light-on", "broadcast-light-error"};
    gchar detail[320]; gint state = light_state_of(profile, detail, sizeof detail);
    GtkStyleContext *style = gtk_widget_get_style_context(profile->light);
    if (state != profile->light_state) {
        /* Only a real state change repaints the colour; nothing flickers on a timer. */
        for (guint i = 0; i < G_N_ELEMENTS(classes); ++i) gtk_style_context_remove_class(style, classes[i]);
        gtk_style_context_add_class(style, classes[state]);
        profile->light_state = state;
        if (profile->light_dim) { gtk_style_context_remove_class(style, "broadcast-light-dim"); profile->light_dim = FALSE; }
    }
    gchar *current = gtk_widget_get_tooltip_text(profile->light);
    if (g_strcmp0(current, detail)) gtk_widget_set_tooltip_text(profile->light, detail);
    g_free(current);
    /* Connecting and error states blink at 1 Hz on the monotonic clock;
     * connected and off are steady. */
    gboolean blinking = state == LIGHT_CONNECTING || state == LIGHT_ERROR;
    gboolean dim = blinking && (g_get_monotonic_time() / (G_TIME_SPAN_SECOND / 2)) % 2 == 1;
    if (dim != profile->light_dim) {
        if (dim) gtk_style_context_add_class(style, "broadcast-light-dim"); else gtk_style_context_remove_class(style, "broadcast-light-dim");
        profile->light_dim = dim;
    }
}
static const gchar *run_kind_label(const gchar *kind)
{
    return g_str_equal(kind, "stream") ? "Yayın" : g_str_equal(kind, "record") ? "Kayıt" :
        g_str_equal(kind, "emergency") ? "Acil" : g_str_equal(kind, "metadata") ? "Bilgi" : kind;
}
static const gchar *run_state_label(const gchar *state)
{
    return g_str_equal(state, "prepared") ? "Hazırlandı" : g_str_equal(state, "active") ? "Çalışıyor" :
        g_str_equal(state, "completed") ? "Tamamlandı" : g_str_equal(state, "failed") ? "Hata" :
        g_str_equal(state, "interrupted") ? "Kesildi" : state;
}
static gchar *run_time_label(const gchar *stamp)
{
    GDateTime *utc = stamp && *stamp ? g_date_time_new_from_iso8601(stamp, NULL) : NULL;
    GDateTime *local = utc ? g_date_time_to_local(utc) : NULL;
    gchar *text = local ? g_date_time_format(local, "%d.%m.%Y %H:%M:%S") : g_strdup("—");
    if (local) g_date_time_unref(local);
    if (utc) g_date_time_unref(utc);
    return text;
}
/* "Son çalıştırmalar": a self-contained view (Teknik yönetim › Sunucular,
 * and the output panel). It asks the data service only while it is on screen. */
typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *root, *view;
    GtkListStore *store;
    gboolean loading, forbidden, disposed;
    guint timer, ticks, backoff;
    guint64 generation[2];
} RunsView;
static void runs_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; (void)data;
    RunsView *runs = owner ? g_object_get_data(G_OBJECT(owner), "broadcast-runs-state") : NULL;
    if (!runs || runs->disposed) return;
    runs->loading = FALSE;
    /* A failed read keeps the previous rows and retries less often (20 s up
     * to ~5 min); a station without permission stops until shown again. */
    if (error) {
        if (g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_FORBIDDEN)) runs->forbidden = TRUE;
        else runs->backoff = MIN(runs->backoff + 1, 5u);
        return;
    }
    if (!result || !JSON_NODE_HOLDS_OBJECT(result)) return;
    runs->backoff = 0;
    JsonNode *runs_node = json_object_get_member(json_node_get_object(result), "runs");
    if (!runs_node || !JSON_NODE_HOLDS_ARRAY(runs_node)) return;
    JsonArray *list = json_node_get_array(runs_node);
    gtk_list_store_clear(runs->store);
    for (guint i = 0; i < json_array_get_length(list); ++i) {
        JsonNode *entry = json_array_get_element(list, i);
        JsonObject *run = JSON_NODE_HOLDS_OBJECT(entry) ? json_node_get_object(entry) : NULL;
        if (!run) continue;
        JsonNode *last_node = json_object_get_member(run, "last_event");
        JsonObject *last = last_node && JSON_NODE_HOLDS_OBJECT(last_node) ? json_node_get_object(last_node) : NULL;
        const gchar *stamp = last ? radios_json_string(last, "occurred_at", "") : radios_json_string(run, "created_at", "");
        gchar *when = run_time_label(stamp);
        gtk_list_store_insert_with_values(runs->store, NULL, -1,
            RUN_KIND, run_kind_label(radios_json_string(run, "kind", "")), RUN_NAME, radios_json_string(run, "name", ""),
            RUN_STATE, run_state_label(radios_json_string(run, "state", "")), RUN_TIME, when,
            RUN_ID, radios_json_string(run, "run_id", ""), -1);
        g_free(when);
    }
}
static void runs_refresh(RunsView *runs)
{
    runs->ticks = 0;
    if (runs->disposed || runs->loading) return;
    /* Without the data service the list is simply empty. */
    if (!runs->ctx->data || !radios_data_connected(runs->ctx)) { gtk_list_store_clear(runs->store); return; }
    JsonNode *body = radios_data_body(runs->ctx);
    runs->loading = radios_data_request(runs->ctx, runs->root, "POST", "/v1/workflows/desktop-broadcast/overview",
        body, runs_loaded, NULL, NULL);
    json_node_free(body);
}
static gboolean runs_on_screen(RunsView *runs) { return !runs->disposed && gtk_widget_get_mapped(runs->view); }
static gboolean runs_tick(gpointer data)
{
    RunsView *runs = data;
    if (!runs_on_screen(runs)) return G_SOURCE_CONTINUE; /* hidden page or closed panel: no requests */
    /* A new local output generation means a run receipt was just created:
     * list it about a second later instead of waiting ten. */
    static const gchar *const kinds[] = {"stream", "record"};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        RadioSAudioOutputSnapshot s;
        if (radios_broadcast_ui_output_snapshot(runs->ctx, kinds[i], &s) && s.generation != runs->generation[i]) {
            runs->generation[i] = s.generation; runs->backoff = 0;
            runs->ticks = MAX(runs->ticks, RUNS_REFRESH_TICKS - 4);
        }
    }
    if (!runs->forbidden && ++runs->ticks >= (RUNS_REFRESH_TICKS << runs->backoff)) runs_refresh(runs);
    return G_SOURCE_CONTINUE;
}
static void runs_shown(GtkWidget *widget, gpointer data)
{
    (void)widget; RunsView *runs = data;
    /* Shown again (page selected, panel opened): list the latest runs at once. */
    runs->forbidden = FALSE; runs->backoff = 0; runs_refresh(runs);
}
static void runs_data_refreshed(RadioSViewContext *ctx, GtkWidget *owner)
{
    (void)ctx; RunsView *runs = g_object_get_data(G_OBJECT(owner), "broadcast-runs-state");
    if (runs && runs_on_screen(runs) && !runs->forbidden) runs_refresh(runs);
}
static void runs_destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; RunsView *runs = data; runs->disposed = TRUE;
    if (runs->timer) { g_source_remove(runs->timer); runs->timer = 0; }
}
GtkWidget *radios_broadcast_runs_view_new(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx, NULL);
    RunsView *runs = g_new0(RunsView, 1); runs->ctx = ctx;
    GtkWidget *box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "em-card"); gtk_widget_set_name(box, "broadcast-runs");
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "technical-card");
    runs->root = box;
    g_object_set_data_full(G_OBJECT(box), "broadcast-runs-state", runs, g_free);
    GtkWidget *title = radios_ui_label("Son çalıştırmalar", "em-card-title");
    gtk_style_context_add_class(gtk_widget_get_style_context(title), "heading");
    gtk_label_set_xalign(GTK_LABEL(title), 0); pack(box, title, FALSE);
    runs->store = gtk_list_store_new(RUN_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    runs->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(runs->store)); g_object_unref(runs->store);
    gtk_widget_set_name(runs->view, "broadcast-runs-list");
    gtk_widget_set_tooltip_text(runs->view, "Veritabanındaki son 50 yayın, kayıt ve acil çalıştırma kaydı");
    static const gchar *const titles[] = {"Tür", "Ad", "Durum", "Son olay"};
    for (guint i = 0; i < G_N_ELEMENTS(titles); ++i) {
        GtkCellRenderer *cell = gtk_cell_renderer_text_new();
        if (i == RUN_NAME) g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], cell, "text", (gint)i, NULL);
        gtk_tree_view_column_set_expand(column, i == RUN_NAME);
        gtk_tree_view_append_column(GTK_TREE_VIEW(runs->view), column);
    }
    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroller, -1, 128);
    gtk_container_add(GTK_CONTAINER(scroller), runs->view); pack(box, scroller, TRUE);
    g_signal_connect(box, "map", G_CALLBACK(runs_shown), runs);
    g_signal_connect(box, "destroy", G_CALLBACK(runs_destroyed), runs);
    if (ctx->data) radios_data_register_refresh(ctx, box, runs_data_refreshed);
    runs->timer = g_timeout_add(250, runs_tick, runs);
    return box;
}
static gboolean status_tick(gpointer data)
{
    OutputPanel *panel = data;
    if (panel->disposed) return G_SOURCE_REMOVE;
    for (guint i = 0; i < G_N_ELEMENTS(panel->profiles); ++i) {
        OutputProfile *profile = &panel->profiles[i];
        if (radios_data_document_ready(profile->document)) gtk_widget_set_sensitive(profile->owner, TRUE);
        light_update(profile);
        gchar *text = radios_broadcast_ui_status(panel->ctx, profile->kind);
        if (text && *text && g_get_monotonic_time() >= profile->notice_until) {
            gtk_label_set_text(GTK_LABEL(profile->status), text);
            gtk_widget_set_tooltip_text(profile->status, text);
        }
        g_free(text);
    }
    return G_SOURCE_CONTINUE;
}
static void destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; OutputPanel *panel = data; panel->disposed = TRUE;
    if (panel->timer) { g_source_remove(panel->timer); panel->timer = 0; }
}
static void lights_style(GtkWidget *widget)
{
    static gsize installed = 0;
    if (!g_once_init_enter(&installed)) return;
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
        ".broadcast-light { font-size: 1.15em; }"
        ".broadcast-light-off { color: #4a5560; }"
        ".broadcast-light-connecting { color: #f2c230; }"
        ".broadcast-light-on { color: #3ddc84; }"
        ".broadcast-light-error { color: #ff5252; }"
        ".broadcast-light-dim { opacity: 0.25; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(widget), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    g_once_init_leave(&installed, 1);
}
GtkWidget *radios_broadcast_output_panel_new(RadioSViewContext *ctx)
{
    OutputPanel *panel = g_new0(OutputPanel, 1); panel->ctx = ctx;
    GtkWidget *expander = gtk_expander_new("Yayın çıkışı · Kayıt"); gtk_widget_set_name(expander, "broadcast-output-panel");
    panel->root = expander; lights_style(expander);
    GtkWidget *body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "em-settings");
    gtk_container_add(GTK_CONTAINER(expander), body);
    g_object_set_data_full(G_OBJECT(expander), "broadcast-output-state", panel, g_free);
    g_signal_connect(expander, "destroy", G_CALLBACK(destroyed), panel);
    for (guint i = 0; i < G_N_ELEMENTS(panel->profiles); ++i) pack(body, profile_new(panel, i), FALSE);
    pack(body, radios_broadcast_runs_view_new(ctx), FALSE);
    g_signal_connect(expander, "notify::expanded", G_CALLBACK(expanded), panel);
    panel->timer = g_timeout_add(250, status_tick, panel);
    return expander;
}
