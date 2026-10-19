#include "radios/autodj_settings.h"
#include "radios/broadcast_header.h"
#include "radios/autodj_marker_jobs.h"
#include "radios/data.h"
#include <math.h>

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *page, *mode, *overlap, *policy_save, *policy_status;
    GtkWidget *scope, *playlist, *replace, *start, *cancel, *progress, *status, *refresh;
    GtkWidget *threshold, *out_lead;
    RadioSDataClient *client;
    GCancellable *running;
    guint timer, playlist_ticket, marker_ticket;
    gboolean disposed, rendering, policy_dirty, policy_known, playlist_loading, playlist_loaded;
    guint shown_mode;
    gdouble shown_overlap;
    gboolean shown_beat;
} AutoDJSettings;

typedef struct {
    RadioSDataClient *client;
    guint ticket;
} PlaylistRequest;
typedef struct {
    GWeakRef owner;
    guint ticket;
} MarkerRequest;

static GtkWidget *named(GtkWidget *widget, const gchar *name)
{ gtk_widget_set_name(widget, name); return widget; }
static void pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static GtkWidget *label(const gchar *text)
{
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(widget), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 68);
    return widget;
}
static GtkWidget *section(GtkWidget *body, const gchar *title)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8), *heading = label(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "preferences-section");
    gtk_style_context_add_class(gtk_widget_get_style_context(heading), "preferences-heading");
    pack(box, heading, FALSE); pack(body, box, FALSE); return box;
}
static void marker_status(AutoDJSettings *s, const gchar *text)
{ gtk_label_set_text(GTK_LABEL(s->status), text); }
static gboolean all_tracks(AutoDJSettings *s)
{ return g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->scope)), "all") == 0; }
static void sensitivity(AutoDJSettings *s)
{
    gboolean connected = radios_data_connected(s->ctx);
    gboolean busy = s->running != NULL;
    gboolean playlist = !all_tracks(s);
    gboolean policy_ready = connected && radios_broadcast_transition_policy_ready(s->ctx);
    gint selected = gtk_combo_box_get_active(GTK_COMBO_BOX(s->mode));
    gtk_widget_set_sensitive(s->mode, policy_ready);
    gtk_widget_set_sensitive(s->overlap, policy_ready && (selected == 1 || selected == 2 || selected == 4));
    gtk_widget_set_sensitive(s->policy_save, policy_ready && s->policy_dirty);
    gtk_widget_set_sensitive(s->scope, connected && !busy);
    gtk_widget_set_sensitive(s->playlist, connected && !busy && playlist && !s->playlist_loading);
    gtk_widget_set_sensitive(s->replace, connected && !busy);
    gtk_widget_set_sensitive(s->threshold, connected && !busy);
    gtk_widget_set_sensitive(s->out_lead, connected && !busy);
    gtk_widget_set_sensitive(s->refresh, connected && !busy && !s->playlist_loading);
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->playlist));
    gtk_widget_set_sensitive(s->start, connected && !busy && (!playlist || (s->playlist_loaded && id && g_uuid_string_is_valid(id))));
    gtk_widget_set_visible(s->cancel, busy);
    gtk_widget_set_sensitive(s->cancel, busy && !g_cancellable_is_cancelled(s->running));
}
static void policy_render(AutoDJSettings *s, guint mode, gdouble overlap, gboolean beat)
{
    s->rendering = TRUE;
    gtk_combo_box_set_active(GTK_COMBO_BOX(s->mode), beat ? 4 : (gint)mode);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->overlap), overlap);
    s->shown_mode = mode; s->shown_overlap = overlap; s->shown_beat = beat;
    s->policy_known = TRUE; s->rendering = FALSE;
}
static void policy_edited(GtkWidget *widget, gpointer data)
{
    (void)widget; AutoDJSettings *s = data;
    if (s->rendering || s->disposed) return;
    s->policy_dirty = TRUE;
    gtk_label_set_text(GTK_LABEL(s->policy_status), "Değişiklikleri uygulayın.");
    sensitivity(s);
}
static void policy_save(GtkButton *button, gpointer data)
{
    (void)button; AutoDJSettings *s = data;
    gint selected = gtk_combo_box_get_active(GTK_COMBO_BOX(s->mode));
    guint mode = selected == 4 ? 1 : (guint)MAX(0, selected);
    gboolean beat = selected == 4;
    gdouble overlap = gtk_spin_button_get_value(GTK_SPIN_BUTTON(s->overlap));
    GError *error = NULL;
    if (radios_broadcast_transition_policy_set(s->ctx, mode, overlap, beat, &error)) {
        s->policy_dirty = FALSE;
        gtk_label_set_text(GTK_LABEL(s->policy_status), "Geçiş ayarları uygulandı; veritabanına kaydediliyor…");
    } else gtk_label_set_text(GTK_LABEL(s->policy_status), error ? error->message : "Geçiş ayarı uygulanamadı.");
    g_clear_error(&error); sensitivity(s);
}
static void playlist_request_free(gpointer data)
{ PlaylistRequest *request = data; radios_data_client_unref(request->client); g_free(request); }
static void playlists_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *error, gpointer data)
{
    AutoDJSettings *s = g_object_get_data(G_OBJECT(owner), "radios-autodj-settings");
    PlaylistRequest *request = data;
    if (!s || s->disposed || s->playlist_ticket != request->ticket) return;
    RadioSDataClient *current = radios_data_client_acquire(ctx);
    gboolean same = radios_data_client_same_identity(current, request->client);
    g_clear_pointer(&current, radios_data_client_unref);
    if (!same) return;
    s->playlist_loading = FALSE;
    JsonObject *object = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    JsonNode *rows_node = object ? json_object_get_member(object, "playlists") : NULL;
    if (error || !rows_node || !JSON_NODE_HOLDS_ARRAY(rows_node)) {
        marker_status(s, error ? error->message : "Playlistler okunamadı; Yenile ile tekrar deneyin."); sensitivity(s); return;
    }
    gchar *selected = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->playlist)));
    if (!selected) {
        gint64 revision = 0;
        radios_playlist_visible_target(ctx, &selected, &revision);
    }
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(s->playlist));
    JsonArray *rows = json_node_get_array(rows_node); guint count = 0;
    for (guint i = 0; i < json_array_get_length(rows); ++i) {
        JsonNode *row_node = json_array_get_element(rows, i);
        JsonObject *row = row_node && JSON_NODE_HOLDS_OBJECT(row_node) ? json_node_get_object(row_node) : NULL;
        const gchar *id = radios_json_string(row, "_id", "");
        if (!g_uuid_string_is_valid(id)) continue;
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->playlist), id, radios_json_string(row, "name", "Adsız playlist")); ++count;
    }
    if (!selected || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->playlist), selected)) gtk_combo_box_set_active(GTK_COMBO_BOX(s->playlist), count ? 0 : -1);
    g_free(selected); gboolean first_load = !s->playlist_loaded; s->playlist_loaded = TRUE;
    if (!s->running && first_load) marker_status(s, count ? "Hedefi seçip otomatik IN / OUT atamasını başlatın." : "Kayıtlı playlist yok. Tüm şarkıları seçebilir veya Playlist bölümünde bir liste oluşturabilirsiniz.");
    sensitivity(s);
}
static void playlists_refresh(GtkButton *button, gpointer data)
{
    (void)button; AutoDJSettings *s = data;
    if (s->disposed || s->running || s->playlist_loading || !radios_data_connected(s->ctx)) return;
    RadioSDataClient *client = radios_data_client_acquire(s->ctx);
    if (!client) return;
    PlaylistRequest *request = g_new0(PlaylistRequest, 1); request->client = client; request->ticket = ++s->playlist_ticket;
    s->playlist_loading = TRUE;
    JsonNode *body = radios_data_body(s->ctx);
    if (!radios_data_request(s->ctx, s->page, "POST", "/v1/workflows/desktop-playlists/list", body, playlists_loaded, request, playlist_request_free)) s->playlist_loading = FALSE;
    json_node_free(body); sensitivity(s);
}
static void data_refreshed(RadioSViewContext *ctx, GtkWidget *owner)
{ (void)ctx; AutoDJSettings *s = g_object_get_data(G_OBJECT(owner), "radios-autodj-settings"); if (s && !s->disposed) playlists_refresh(NULL, s); }
static void scope_changed(GtkComboBox *widget, gpointer data)
{ (void)widget; sensitivity(data); }
static void marker_request_free(gpointer data)
{ MarkerRequest *request = data; g_weak_ref_clear(&request->owner); g_free(request); }
static void marker_progress(guint completed, guint total, gpointer data)
{
    MarkerRequest *request = data; GtkWidget *owner = g_weak_ref_get(&request->owner);
    AutoDJSettings *s = owner ? g_object_get_data(G_OBJECT(owner), "radios-autodj-settings") : NULL;
    if (s && !s->disposed && request->ticket == s->marker_ticket) {
        gchar *text = total ? g_strdup_printf("%u / %u şarkı", completed, total) : g_strdup("Şarkılar hazırlanıyor…");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(s->progress), total ? MIN(1.0, (gdouble)completed / total) : 0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(s->progress), text); g_free(text);
    }
    g_clear_object(&owner);
}
static void markers_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *error, gpointer data)
{
    MarkerRequest *request = data; AutoDJSettings *s = g_object_get_data(G_OBJECT(owner), "radios-autodj-settings");
    if (!s || s->disposed || request->ticket != s->marker_ticket) return;
    g_clear_object(&s->running);
    JsonObject *result = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    if (error || !result) {
        marker_status(s, error ? error->message : "IN / OUT noktaları atanamadı.");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(s->progress), "İşlem tamamlanamadı");
    } else {
        guint total = (guint)MAX(0, radios_json_int(result, "total", 0));
        guint completed = (guint)MAX(0, radios_json_int(result, "completed", 0));
        gint64 updated = radios_json_int(result, "updated", 0), unchanged = radios_json_int(result, "unchanged", 0);
        gint64 skipped = radios_json_int(result, "skipped", 0), failed = radios_json_int(result, "failed", 0);
        JsonNode *cancelled_node = json_object_get_member(result, "cancelled");
        gboolean cancelled = cancelled_node && JSON_NODE_HOLDS_VALUE(cancelled_node) && json_node_get_value_type(cancelled_node) == G_TYPE_BOOLEAN && json_node_get_boolean(cancelled_node);
        marker_progress(completed, total, request);
        const gchar *first_error = radios_json_string(result, "first_error", "");
        const gchar *reason = *first_error ? first_error : radios_json_string(result, "first_skip_reason", "");
        gboolean incomplete = completed < total;
        gchar *pending = incomplete ? g_strdup_printf(" %u şarkı işlenmedi.", total - completed) : g_strdup("");
        const gchar *state = cancelled ? "Durduruldu" : incomplete ? "Yarım kaldı" : total ? "Tamamlandı" : "İşlenecek şarkı bulunamadı";
        gchar *text = g_strdup_printf("%s · %" G_GINT64_FORMAT " hesaplandı · %" G_GINT64_FORMAT " güncellendi, %" G_GINT64_FORMAT " zaten güncel, %" G_GINT64_FORMAT " atlandı, %" G_GINT64_FORMAT " başarısız.%s%s%s",
            state, updated + unchanged, updated, unchanged, skipped, failed, pending, *reason ? " " : "", reason);
        marker_status(s, text); g_free(text); g_free(pending);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(s->progress), state);
        if (updated) radios_data_refresh(ctx);
    }
    sensitivity(s);
}
static void marker_start(GtkButton *button, gpointer data)
{
    (void)button; AutoDJSettings *s = data;
    if (s->disposed || s->running || !radios_data_connected(s->ctx)) return;
    const gchar *playlist = all_tracks(s) ? NULL : gtk_combo_box_get_active_id(GTK_COMBO_BOX(s->playlist));
    if (!all_tracks(s) && (!playlist || !g_uuid_string_is_valid(playlist))) {
        marker_status(s, "Önce kayıtlı bir playlist seçin."); return;
    }
    MarkerRequest *request = g_new0(MarkerRequest, 1); g_weak_ref_init(&request->owner, s->page); request->ticket = ++s->marker_ticket;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(s->progress), 0); gtk_progress_bar_set_text(GTK_PROGRESS_BAR(s->progress), "Şarkılar hazırlanıyor…");
    marker_status(s, "IN / OUT noktaları hesaplanıp veritabanına kaydediliyor…");
    s->running = radios_autodj_markers_scan_async(s->ctx, s->page, playlist,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(s->replace)),
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(s->threshold)),
        (guint)llround(gtk_spin_button_get_value(GTK_SPIN_BUTTON(s->out_lead)) * 1000),
        marker_progress, markers_done, request, marker_request_free);
    if (!s->running) marker_status(s, "İşlem başlatılamadı; veritabanı bağlantısını kontrol edin.");
    sensitivity(s);
}
/* Marker worker callbacks are bound to this page and the captured database
 * identity. Destruction cancels the work before this state can be freed. */
static void marker_cancel(GtkButton *button, gpointer data)
{
    (void)button; AutoDJSettings *s = data;
    if (s->running) {
        g_cancellable_cancel(s->running);
        marker_status(s, "İşlem durduruluyor; kaydedilen noktalar korunur."); sensitivity(s);
    }
}
static gboolean tick(gpointer data)
{
    AutoDJSettings *s = data;
    if (s->disposed) return G_SOURCE_REMOVE;
    RadioSDataClient *client = radios_data_client_acquire(s->ctx);
    if (!client && s->client) {
        if (s->running) g_cancellable_cancel(s->running);
        g_clear_object(&s->running);
        ++s->playlist_ticket; ++s->marker_ticket;
        s->playlist_loading = s->playlist_loaded = FALSE;
        g_clear_pointer(&s->client, radios_data_client_unref);
        marker_status(s, "Veritabanı bağlantısı kesildi; işlem durduruldu. Daha önce kaydedilen noktalar korunur.");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(s->progress), "Bağlantı bekleniyor");
    }
    gboolean identity_changed = client && !radios_data_client_same_identity(client, s->client);
    if (identity_changed) {
        if (s->running) g_cancellable_cancel(s->running);
        g_clear_object(&s->running);
        g_clear_pointer(&s->client, radios_data_client_unref); s->client = radios_data_client_ref(client);
        ++s->playlist_ticket; ++s->marker_ticket; s->playlist_loaded = s->playlist_loading = s->policy_dirty = s->policy_known = FALSE;
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(s->playlist));
        marker_status(s, "Veritabanı bağlantısı hazır; playlistler yükleniyor…");
        playlists_refresh(NULL, s);
    }
    g_clear_pointer(&client, radios_data_client_unref);
    guint mode = 0; gdouble overlap = 0; gboolean beat = FALSE;
    gboolean ready = radios_data_connected(s->ctx) && radios_broadcast_transition_policy_ready(s->ctx);
    if (!radios_broadcast_transition_policy(s->ctx, &mode, &overlap, &beat)) { mode = 1; overlap = 3; beat = FALSE; }
    if (ready && !s->policy_dirty) {
        if (!s->policy_known || mode != s->shown_mode || fabs(overlap - s->shown_overlap) > .0001 || beat != s->shown_beat) policy_render(s, mode, overlap, beat);
        const gchar *text = radios_broadcast_transition_policy_status(s->ctx);
        if (text && *text) gtk_label_set_text(GTK_LABEL(s->policy_status), text);
    } else if (!ready) gtk_label_set_text(GTK_LABEL(s->policy_status), radios_data_connected(s->ctx) ? "Kayıtlı geçiş ayarları yükleniyor…" : "Geçiş ayarları için veritabanı bağlantısı gerekiyor.");
    sensitivity(s); return G_SOURCE_CONTINUE;
}
static void destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; AutoDJSettings *s = data; s->disposed = TRUE;
    if (s->timer) { g_source_remove(s->timer); s->timer = 0; }
    if (s->running) g_cancellable_cancel(s->running);
}
static void state_free(gpointer data)
{
    AutoDJSettings *s = data;
    if (s->timer) g_source_remove(s->timer);
    if (s->running) g_cancellable_cancel(s->running);
    g_clear_object(&s->running); g_clear_pointer(&s->client, radios_data_client_unref); g_free(s);
}
GtkWidget *radios_autodj_settings_new(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx, NULL);
    AutoDJSettings *s = g_new0(AutoDJSettings, 1); s->ctx = ctx;
    s->page = named(gtk_scrolled_window_new(NULL, NULL), "preferences-autodj-page");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s->page), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_container_set_border_width(GTK_CONTAINER(body), 12); gtk_container_add(GTK_CONTAINER(s->page), body);
    GtkWidget *policy = section(body, "Auto DJ geçişleri");
    const gchar *const modes[] = {"Parça bitince", "Crossfade", "AutoDJ OUT / Cue", "Elle geç", "SYNC beatmatch · 4 vuruş", NULL};
    s->mode = named(gtk_combo_box_text_new(), "autodj-transition-mode");
    for (guint i = 0; modes[i]; ++i) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(s->mode), modes[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(s->mode), 1); pack(policy, s->mode, FALSE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); pack(row, label("Bindirme (sn)"), FALSE);
    s->overlap = named(gtk_spin_button_new_with_range(0, 30, .1), "autodj-transition-overlap"); gtk_spin_button_set_digits(GTK_SPIN_BUTTON(s->overlap), 1); gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->overlap), 3);
    atk_object_set_name(gtk_widget_get_accessible(s->mode), "Auto DJ geçiş türü"); atk_object_set_name(gtk_widget_get_accessible(s->overlap), "Auto DJ bindirme süresi");
    pack(row, s->overlap, FALSE); s->policy_save = named(gtk_button_new_with_label("Geçiş ayarlarını uygula"), "autodj-transition-save"); pack(row, s->policy_save, FALSE); pack(policy, row, FALSE);
    s->policy_status = named(label("Kayıtlı geçiş ayarları yükleniyor…"), "autodj-transition-status"); pack(policy, s->policy_status, FALSE);
    GtkWidget *markers = section(body, "AutoDJ IN / OUT noktaları");
    s->scope = named(gtk_combo_box_text_new(), "autodj-marker-scope"); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->scope), "playlist", "Seçili playlist"); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(s->scope), "all", "Tüm şarkılar"); gtk_combo_box_set_active_id(GTK_COMBO_BOX(s->scope), "playlist");
    atk_object_set_name(gtk_widget_get_accessible(s->scope), "Otomatik işaretleme hedefi"); pack(markers, s->scope, FALSE);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); s->playlist = named(gtk_combo_box_text_new(), "autodj-marker-playlist"); gtk_widget_set_hexpand(s->playlist, TRUE); atk_object_set_name(gtk_widget_get_accessible(s->playlist), "İşaretlenecek playlist");
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(s->playlist)); for (GList *cell = cells; cell; cell = cell->next) if (GTK_IS_CELL_RENDERER_TEXT(cell->data)) g_object_set(cell->data, "ellipsize", PANGO_ELLIPSIZE_END, "max-width-chars", 38, NULL); g_list_free(cells);
    pack(row, s->playlist, TRUE); s->refresh = named(gtk_button_new_with_label("Yenile"), "autodj-marker-refresh"); pack(row, s->refresh, FALSE); pack(markers, row, FALSE);
    s->replace = named(gtk_check_button_new_with_label("Mevcut AutoDJ IN / OUT noktalarını yeniden hesapla"), "autodj-marker-replace"); pack(markers, s->replace, FALSE);
    GtkWidget *detection = gtk_expander_new("Algılama seçenekleri");
    GtkWidget *options = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(options), 12); gtk_grid_set_row_spacing(GTK_GRID(options), 8); gtk_widget_set_margin_top(options, 8);
    gtk_grid_attach(GTK_GRID(options), label("Sessizlik eşiği (dB)"), 0, 0, 1, 1);
    s->threshold = named(gtk_spin_button_new_with_range(-60, -6, 1), "autodj-marker-threshold"); gtk_spin_button_set_value(GTK_SPIN_BUTTON(s->threshold), -45); gtk_grid_attach(GTK_GRID(options), s->threshold, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(options), label("Ritimli çıkış yoksa OUT'u öne al (sn)"), 0, 1, 1, 1);
    s->out_lead = named(gtk_spin_button_new_with_range(0, 30, .1), "autodj-marker-out-lead"); gtk_spin_button_set_digits(GTK_SPIN_BUTTON(s->out_lead), 1); gtk_grid_attach(GTK_GRID(options), s->out_lead, 1, 1, 1, 1);
    atk_object_set_name(gtk_widget_get_accessible(s->threshold), "Sessizlik eşiği"); atk_object_set_name(gtk_widget_get_accessible(s->out_lead), "Ritimli çıkış bulunamazsa OUT noktasını öne alma süresi");
    gtk_widget_set_tooltip_text(s->out_lead, "Yalnız ritimli çıkış bulunamadığında ölçülen ses sonundan geri alınır. Ritimli çıkış noktası vuruşta korunur.");
    gtk_container_add(GTK_CONTAINER(detection), options); pack(markers, detection, FALSE);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); s->start = named(gtk_button_new_with_label("IN / OUT noktalarını ata"), "autodj-marker-start"); pack(row, s->start, FALSE);
    s->cancel = named(gtk_button_new_with_label("Durdur"), "autodj-marker-cancel"); gtk_widget_set_no_show_all(s->cancel, TRUE); pack(row, s->cancel, FALSE); pack(markers, row, FALSE);
    s->progress = named(gtk_progress_bar_new(), "autodj-marker-progress"); gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(s->progress), TRUE); gtk_progress_bar_set_text(GTK_PROGRESS_BAR(s->progress), "Hazır"); pack(markers, s->progress, FALSE);
    s->status = named(label("Veritabanı bağlantısı bekleniyor…"), "autodj-marker-status"); pack(markers, s->status, FALSE);
    g_object_set_data_full(G_OBJECT(s->page), "radios-autodj-settings", s, state_free); g_signal_connect(s->page, "destroy", G_CALLBACK(destroyed), s);
    g_signal_connect(s->mode, "changed", G_CALLBACK(policy_edited), s); g_signal_connect(s->overlap, "value-changed", G_CALLBACK(policy_edited), s); g_signal_connect(s->policy_save, "clicked", G_CALLBACK(policy_save), s);
    g_signal_connect(s->start, "clicked", G_CALLBACK(marker_start), s);
    g_signal_connect(s->scope, "changed", G_CALLBACK(scope_changed), s); g_signal_connect(s->playlist, "changed", G_CALLBACK(scope_changed), s); g_signal_connect(s->refresh, "clicked", G_CALLBACK(playlists_refresh), s); g_signal_connect(s->cancel, "clicked", G_CALLBACK(marker_cancel), s);
    radios_data_register_refresh(ctx, s->page, data_refreshed);
    s->timer = g_timeout_add(500, tick, s); tick(s); sensitivity(s); return s->page;
}
