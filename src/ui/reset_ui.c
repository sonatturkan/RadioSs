#define _POSIX_C_SOURCE 200809L
#include "radios/reset_ui.h"
#include "radios/data.h"
#include "radios/settings_sync.h"
#include <string.h>

typedef struct { const gchar *scope, *title; } Action;
static const Action actions[] = {
    {"settings", "Genel ayarları sıfırla"},
    {"playlists", "Playlistleri sıfırla"},
    {"media", "İçe aktarılan şarkıları sil"},
    {"mixer", "Mikser ayarlarını sıfırla"},
    {"automation", "Otomasyonları sıfırla"},
    {"factory", "Fabrika ayarlarına döndür"},
};
static const struct { const gchar *kind, *label; } kind_labels[] = {
    {"audio_preferences", "Ses aygıtı ve ses yüksekliği ayarları"}, {"audio_routing", "Ses yönlendirme yapılandırmaları"},
    {"midi_mapping", "MIDI haritaları"},
    {"broadcast_stream", "İnternet yayını profili"}, {"broadcast_record", "Kayıt profili"},
    {"broadcast_metadata", "Şarkı bilgisi profili"}, {"server", "Sunucu ayarları"}, {"web", "Web ayarları"},
    {"desktop_preferences", "Görünüm ve ses paneli tercihleri"},
    {"dj_mixer", "DJ mikseri"}, {"channel", "Oynatıcı ve kanal ayarları"}, {"sampler", "Sampler pedleri"},
    {"automation", "Otomasyon kaynakları"}, {"automation_calendar", "Otomasyon takvimi ve rotasyonlar"},
    {"weekly_programme", "Haftalık programlar"}, {"transition", "Geçiş ayarları"}, {"playout_policy", "Genel yayın sırası"},
    {"programme", "Programlar"}, {"emergency", "Acil durum yayınları"}, {"markers", "İşaret setleri"},
    {"advert_break", "Reklam araları"},
    {"live_players", "Oynatıcı düzeni ve kümeleri"}, {"local_event", "Yerel etkinlikler"},
    {"copyright_policy", "Telif politikası"},
    {"online_services", "Çevrimiçi hizmet kayıtları (eski)"}, {"external_playlist", "Dış playlist kayıtları (eski)"},
    /* desktop_reset.KEPT_KINDS: no reset touches these; preview and apply list them as kept. */
    {"playout_history", "Yayın geçmişi"}, {"broadcast_local_run", "Yayın çalıştırma kayıtları"},
    {"advert_local_run", "Reklam çalıştırma kayıtları"}, {"advert_report", "Reklam raporları"},
    {"advert_notifications", "Reklam bildirimleri"},
    {"admin_recovery", "Yönetim kurtarma kayıtları"}, {"admin_restore_fence", "Geri yükleme kilidi kayıtları"},
};
typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *page, *backup, *backup_now, *status, *buttons[G_N_ELEMENTS(actions)];
    guint timer;
    gboolean busy, disposed;
} ResetPage;
typedef struct { guint action; gboolean backup; gchar *key; } Job;

static void pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static GtkWidget *styled(GtkWidget *widget, const gchar *style)
{ gtk_style_context_add_class(gtk_widget_get_style_context(widget), style); return widget; }
static GtkWidget *text(const gchar *value, const gchar *style)
{
    GtkWidget *label = gtk_label_new(value); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(label), 80);
    return style ? styled(label, style) : label;
}
static void job_free(gpointer data) { Job *job = data; g_free(job->key); g_free(job); }
static ResetPage *page_of(GtkWidget *owner) { return owner ? g_object_get_data(G_OBJECT(owner), "radios-reset-page") : NULL; }
static void status(ResetPage *page, const gchar *message)
{
    gtk_label_set_text(GTK_LABEL(page->status), message);
    if (page->ctx->notify) page->ctx->notify(page->ctx->notify_data, message);
}
static JsonObject *member_object(JsonObject *parent, const gchar *name)
{
    JsonNode *node = parent ? json_object_get_member(parent, name) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static gboolean manager(RadioSViewContext *ctx)
{
    JsonObject *context = radios_data_context(ctx), *user = member_object(context, "user"), *station = member_object(context, "station");
    const gchar *uid = radios_json_string(user, "_id", ""), *sid = radios_json_string(station, "_id", "");
    JsonNode *grants = context ? json_object_get_member(context, "permissions") : NULL;
    if (!uid[0] || !sid[0] || !grants || !JSON_NODE_HOLDS_ARRAY(grants)) return FALSE;
    JsonArray *list = json_node_get_array(grants);
    for (guint i = 0; i < json_array_get_length(list); ++i) {
        JsonObject *grant = json_array_get_object_element(list, i);
        if (!grant || g_strcmp0(radios_json_string(grant, "user_id", ""), uid) || g_strcmp0(radios_json_string(grant, "permission", ""), "manage")) continue;
        JsonNode *scope = json_object_get_member(grant, "station_id");
        if (scope && (JSON_NODE_HOLDS_NULL(scope) || !g_strcmp0(radios_json_string(grant, "station_id", ""), sid))) return TRUE;
    }
    return FALSE;
}
static void sensitivity(ResetPage *page)
{
    gboolean allowed = !page->busy && radios_data_connected(page->ctx) && manager(page->ctx);
    for (guint i = 0; i < G_N_ELEMENTS(actions); ++i) gtk_widget_set_sensitive(page->buttons[i], allowed);
    gtk_widget_set_sensitive(page->backup_now, allowed);
}
static gboolean tick(gpointer data)
{
    ResetPage *page = data; if (page->disposed) return G_SOURCE_REMOVE;
    sensitivity(page);
    if (!page->busy && !radios_data_connected(page->ctx)) gtk_label_set_text(GTK_LABEL(page->status), "Sıfırlama için veritabanı bağlantısı gerekiyor.");
    else if (!page->busy && !manager(page->ctx)) gtk_label_set_text(GTK_LABEL(page->status), "Sıfırlama için bu istasyonda yönetici (manage) yetkisi gerekiyor.");
    return G_SOURCE_CONTINUE;
}
static JsonNode *request(RadioSViewContext *ctx, const gchar *scope)
{
    JsonNode *body = radios_data_body(ctx); JsonArray *scopes = json_array_new();
    json_array_add_string_element(scopes, scope); json_object_set_array_member(json_node_get_object(body), "scopes", scopes);
    return body;
}
static const gchar *kind_label(const gchar *kind)
{
    for (guint i = 0; i < G_N_ELEMENTS(kind_labels); ++i) if (g_str_equal(kind_labels[i].kind, kind)) return kind_labels[i].label;
    return kind;
}
/* desktop-reset preview and apply both name the record kinds every reset keeps. */
static gchar *kept_text(JsonObject *result)
{
    JsonNode *kept = result ? json_object_get_member(result, "kept") : NULL;
    if (!kept || !JSON_NODE_HOLDS_ARRAY(kept)) return NULL;
    JsonArray *list = json_node_get_array(kept); GString *text = g_string_new("Korunur: "); gboolean first = TRUE;
    for (guint i = 0; i < json_array_get_length(list); ++i) {
        JsonNode *item = json_array_get_element(list, i);
        if (!JSON_NODE_HOLDS_VALUE(item) || json_node_get_value_type(item) != G_TYPE_STRING) continue;
        if (!first) g_string_append(text, ", ");
        g_string_append(text, kind_label(json_node_get_string(item))); first = FALSE;
    }
    if (first) { g_string_free(text, TRUE); return NULL; }
    return g_string_free(text, FALSE);
}
static gchar *summary(JsonObject *result)
{
    GString *text = g_string_new(NULL);
    JsonObject *documents = member_object(result, "documents");
    GList *kinds = documents ? json_object_get_members(documents) : NULL;
    for (GList *item = kinds; item; item = item->next) {
        g_string_append_printf(text, "• %s (%" G_GINT64_FORMAT ")\n", kind_label(item->data), json_object_get_int_member(documents, item->data));
    }
    g_list_free(kinds);
    gint64 playlists = radios_json_int(result, "playlists", 0), media = radios_json_int(result, "media", 0);
    gint64 entries = radios_json_int(result, "playlist_entries", 0);
    if (playlists) g_string_append_printf(text, "• %" G_GINT64_FORMAT " playlist\n", playlists);
    if (media) g_string_append_printf(text, "• %" G_GINT64_FORMAT " şarkı\n", media);
    if (entries) g_string_append_printf(text, "• Playlistlerden %" G_GINT64_FORMAT " şarkı satırı\n", entries);
    gint64 profiles = radios_json_int(result, "profiles", 0);
    if (profiles) g_string_append_printf(text, "• %" G_GINT64_FORMAT " görünüm / içe aktarım profili\n", profiles);
    JsonNode *skipped = json_object_get_member(result, "skipped");
    for (guint i = 0; skipped && JSON_NODE_HOLDS_ARRAY(skipped) && i < json_array_get_length(json_node_get_array(skipped)); ++i) {
        JsonObject *row = json_array_get_object_element(json_node_get_array(skipped), i);
        g_string_append_printf(text, "• Atlandı: %s — %s\n", radios_json_string(row, "name", ""), radios_json_string(row, "reason", ""));
    }
    if (!text->len) g_string_append(text, "Veritabanında sıfırlanacak kayıt yok.\n");
    return g_string_free(text, FALSE);
}
/* Local workstation files follow a general or factory reset. */
static gboolean local_defaults(ResetPage *page, const gchar *scope, GError **error)
{
    if (g_strcmp0(scope, "settings") && g_strcmp0(scope, "factory")) return TRUE;
    if (!g_strcmp0(scope, "factory")) return radios_settings_local_reset(page->ctx, error);
    RadioSUIPreferences ui; radios_ui_preferences_default(&ui);
    RadioSPreferencesAudio audio; radios_preferences_audio_default(&audio);
    return radios_settings_sync_apply(page->ctx, &ui, &audio, error);
}
static void restart_response(GtkDialog *dialog, gint response, gpointer data)
{
    ResetPage *page = data; RadioSViewContext *ctx = page->disposed ? NULL : page->ctx;
    gtk_widget_destroy(GTK_WIDGET(dialog));
    if (response != GTK_RESPONSE_ACCEPT || !ctx) return;
    GError *error = NULL;
    if (!radios_reset_ui_restart(ctx, &error)) {
        gchar *message = g_strdup_printf("RadioS yeniden başlatılamadı: %s. Uygulamayı kapatıp yeniden açın.", error ? error->message : "");
        status(page, message); g_free(message);
    }
    g_clear_error(&error);
}
static void applied(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *failure, gpointer data)
{
    Job *job = data; ResetPage *page = page_of(owner);
    if (!page || page->disposed) return;
    page->busy = FALSE; sensitivity(page);
    JsonObject *result = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    if (failure || !result) {
        gchar *message = g_strdup_printf("%s yapılamadı: %s. Hiçbir kayıt değişmedi.", actions[job->action].title,
            failure ? failure->message : "geçersiz yanıt");
        status(page, message); g_free(message); return;
    }
    GError *local_error = NULL;
    gboolean local_ok = local_defaults(page, actions[job->action].scope, &local_error);
    radios_data_refresh(ctx);
    gchar *done = summary(result), *kept = kept_text(result);
    const gchar *backup = radios_json_string(result, "backup", "");
    gchar *message = g_strdup_printf("%s%s\n\n%s%s%s%s%s%s%s%s\nDeğişikliklerin bütün sayfalarda görünmesi için RadioS'u yeniden başlatın.",
        actions[job->action].title, local_ok ? " tamamlandı." : ": veritabanı sıfırlandı; yerel ayarlar uygulanamadı.",
        done, kept ? kept : "", kept ? "\n" : "", backup[0] ? "\nYedek: " : "", backup, backup[0] ? "\n" : "",
        local_error ? "\n" : "", local_error ? local_error->message : "");
    g_clear_error(&local_error);
    status(page, message);
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(page->page)), GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_INFO, GTK_BUTTONS_NONE, "%s", message);
    gtk_widget_set_name(dialog, "preferences-reset-done");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Daha sonra", GTK_RESPONSE_CANCEL, "Şimdi yeniden başlat", GTK_RESPONSE_ACCEPT, NULL);
    g_signal_connect(dialog, "response", G_CALLBACK(restart_response), page);
    gtk_widget_show_all(dialog);
    g_free(done); g_free(kept); g_free(message);
}
static void confirm_changed(GtkEditable *editable, gpointer data)
{
    GtkWidget *accept = data;
    gtk_widget_set_sensitive(accept, !g_strcmp0(gtk_entry_get_text(GTK_ENTRY(editable)), "SIFIRLA"));
}
typedef struct { ResetPage *page; Job *job; } Confirm;
static void confirm_response(GtkDialog *dialog, gint response, gpointer data)
{
    Confirm *confirm = data; ResetPage *page = confirm->page; Job *job = confirm->job; g_free(confirm);
    gtk_widget_destroy(GTK_WIDGET(dialog));
    if (response != GTK_RESPONSE_ACCEPT || page->disposed) { job_free(job); status(page, "Sıfırlama iptal edildi; hiçbir şey değişmedi."); return; }
    JsonNode *body = request(page->ctx, actions[job->action].scope); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "confirm", "SIFIRLA"); json_object_set_boolean_member(object, "backup", job->backup);
    json_object_set_string_member(object, "idempotency_key", job->key);
    page->busy = TRUE; sensitivity(page);
    status(page, job->backup ? "Veritabanının yedeği alınıyor ve sıfırlanıyor…" : "Sıfırlanıyor…");
    if (!radios_data_request(page->ctx, page->page, "POST", "/v1/workflows/desktop-reset/apply", body, applied, job, job_free)) {
        page->busy = FALSE; sensitivity(page); status(page, "Veritabanı bağlantısı hazır değil; hiçbir şey değişmedi.");
    }
    json_node_free(body);
}
static void previewed(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *failure, gpointer data)
{
    (void)ctx; Job *job = data; ResetPage *page = page_of(owner);
    if (!page || page->disposed) { job_free(job); return; }
    page->busy = FALSE; sensitivity(page);
    JsonObject *result = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    if (failure || !result) {
        gchar *message = g_strdup_printf("Sıfırlama hazırlanamadı: %s", failure ? failure->message : "geçersiz yanıt");
        status(page, message); g_free(message); job_free(job); return;
    }
    const Action *action = &actions[job->action]; gboolean factory = !g_strcmp0(action->scope, "factory");
    GtkWidget *dialog = gtk_dialog_new_with_buttons(action->title, GTK_WINDOW(gtk_widget_get_toplevel(page->page)),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL, NULL);
    gtk_widget_set_name(dialog, "preferences-reset-confirm");
    GtkWidget *accept = gtk_dialog_add_button(GTK_DIALOG(dialog), factory ? "Fabrika ayarlarına döndür" : "Sıfırla", GTK_RESPONSE_ACCEPT);
    gtk_widget_set_name(accept, "preferences-reset-accept");
    styled(accept, "destructive-action");
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_container_set_border_width(GTK_CONTAINER(area), 16);
    gtk_box_set_spacing(GTK_BOX(area), 10);
    gchar *list = summary(result), *kept = kept_text(result);
    pack(area, text("Şunlar sıfırlanacak:", NULL), FALSE);
    GtkWidget *lines = text(list, NULL); gtk_widget_set_name(lines, "preferences-reset-summary"); pack(area, lines, FALSE);
    if (kept) {
        GtkWidget *keeps = text(kept, "preferences-subtitle"); gtk_widget_set_name(keeps, "preferences-reset-kept"); pack(area, keeps, FALSE);
    }
    g_free(kept);
    if (factory) {
        pack(area, text("Onaylamak için SIFIRLA yazın:", NULL), FALSE);
        GtkWidget *entry = gtk_entry_new(); gtk_widget_set_name(entry, "preferences-reset-confirm-text");
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "SIFIRLA"); pack(area, entry, FALSE);
        gtk_widget_set_sensitive(accept, FALSE);
        g_signal_connect(entry, "changed", G_CALLBACK(confirm_changed), accept);
    }
    g_free(list);
    Confirm *confirm = g_new0(Confirm, 1); confirm->page = page; confirm->job = job;
    g_signal_connect(dialog, "response", G_CALLBACK(confirm_response), confirm);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    gtk_widget_show_all(dialog);
}
static void clicked(GtkButton *button, gpointer data)
{
    ResetPage *page = data; if (page->busy) return;
    Job *job = g_new0(Job, 1); job->action = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "reset-action"));
    job->backup = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page->backup)); job->key = g_uuid_string_random();
    JsonNode *body = request(page->ctx, actions[job->action].scope);
    page->busy = TRUE; sensitivity(page); status(page, "Sıfırlanacak kayıtlar sayılıyor…");
    if (!radios_data_request(page->ctx, page->page, "POST", "/v1/workflows/desktop-reset/preview", body, previewed, job, NULL)) {
        page->busy = FALSE; sensitivity(page); status(page, "Veritabanı bağlantısı hazır değil."); job_free(job);
    }
    json_node_free(body);
}
/* desktop-reset/backup: a verified whole-store backup with no reset. */
static void backed_up(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *failure, gpointer data)
{
    (void)ctx; (void)data; ResetPage *page = page_of(owner);
    if (!page || page->disposed) return;
    page->busy = FALSE; sensitivity(page);
    JsonObject *result = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    const gchar *path = radios_json_string(result, "path", "");
    if (failure || !path[0]) {
        gchar *message = g_strdup_printf("Yedek alınamadı: %s", failure ? failure->message : "geçersiz yanıt");
        status(page, message); g_free(message); return;
    }
    gchar *message = g_strdup_printf("Yedek alındı: %s\nDosya: %" G_GINT64_FORMAT " · Medya nesnesi: %" G_GINT64_FORMAT " · İşlem sırası: %" G_GINT64_FORMAT,
        path, radios_json_int(result, "files", 0), radios_json_int(result, "media_objects", 0), radios_json_int(result, "sequence", 0));
    status(page, message); g_free(message);
}
static void backup_now(GtkButton *button, gpointer data)
{
    (void)button; ResetPage *page = data; if (page->busy) return;
    JsonNode *body = radios_data_body(page->ctx);
    page->busy = TRUE; sensitivity(page); status(page, "Veritabanının yedeği alınıyor…");
    if (!radios_data_request(page->ctx, page->page, "POST", "/v1/workflows/desktop-reset/backup", body, backed_up, NULL, NULL)) {
        page->busy = FALSE; sensitivity(page); status(page, "Veritabanı bağlantısı hazır değil; yedek alınmadı.");
    }
    json_node_free(body);
}
static void destroyed(GtkWidget *widget, gpointer data)
{ (void)widget; ResetPage *page = data; page->disposed = TRUE; if (page->timer) { g_source_remove(page->timer); page->timer = 0; } }
GtkWidget *radios_reset_ui_page_new(RadioSViewContext *ctx)
{
    ResetPage *page = g_new0(ResetPage, 1); page->ctx = ctx;
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16); gtk_container_set_border_width(GTK_CONTAINER(body), 30);
    gtk_widget_set_name(body, "preferences-reset-page");
    page->page = body; g_object_set_data_full(G_OBJECT(body), "radios-reset-page", page, g_free);
    g_signal_connect(body, "destroy", G_CALLBACK(destroyed), page);
    pack(body, text("Sıfırlama", "preferences-heading"), FALSE);
    GtkWidget *backups = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    page->backup = gtk_check_button_new_with_label("Sıfırlamadan önce veritabanının tam yedeğini al (önerilir)");
    gtk_widget_set_name(page->backup, "preferences-reset-backup");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->backup), TRUE); pack(backups, page->backup, TRUE);
    page->backup_now = gtk_button_new_with_label("Şimdi yedek al");
    gtk_widget_set_name(page->backup_now, "preferences-reset-backup-now"); gtk_widget_set_valign(page->backup_now, GTK_ALIGN_CENTER);
    g_signal_connect(page->backup_now, "clicked", G_CALLBACK(backup_now), page);
    pack(backups, page->backup_now, FALSE); pack(body, backups, FALSE);
    for (guint i = 0; i < G_N_ELEMENTS(actions); ++i) {
        GtkWidget *row = styled(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16), "preferences-section");
        GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        pack(texts, text(actions[i].title, "preferences-heading"), FALSE);
        pack(row, texts, TRUE);
        GtkWidget *button = gtk_button_new_with_label(i + 1 == G_N_ELEMENTS(actions) ? "Fabrika ayarlarına döndür…" : "Sıfırla…");
        gchar *name = g_strdup_printf("preferences-reset-%s", actions[i].scope); gtk_widget_set_name(button, name); g_free(name);
        gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
        if (i + 1 == G_N_ELEMENTS(actions)) styled(button, "destructive-action");
        g_object_set_data(G_OBJECT(button), "reset-action", GUINT_TO_POINTER(i));
        g_signal_connect(button, "clicked", G_CALLBACK(clicked), page);
        page->buttons[i] = button; pack(row, button, FALSE); pack(body, row, FALSE);
    }
    page->status = text("", "preferences-subtitle"); gtk_widget_set_name(page->status, "preferences-reset-status");
    gtk_label_set_selectable(GTK_LABEL(page->status), TRUE); pack(body, page->status, FALSE);
    tick(page); page->timer = g_timeout_add(500, tick, page);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_widget_set_name(scroll, "preferences-reset-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), body); return scroll;
}

/* The launcher rebuilds if needed and starts the data service. Inside the
 * Flatpak SDK sandbox it runs on the host, so it outlives this process. */
gboolean radios_reset_ui_restart(RadioSViewContext *ctx, GError **error)
{
    g_return_val_if_fail(ctx && ctx->config && ctx->config->project_root, FALSE);
    gchar *cmdline = NULL; gsize length = 0;
    GPtrArray *original = g_ptr_array_new_with_free_func(g_free);
    if (g_file_get_contents("/proc/self/cmdline", &cmdline, &length, NULL))
        for (gsize offset = 0, index = 0; offset < length; ++index) {
            gsize size = strnlen(cmdline + offset, length - offset);
            gchar *argument = g_strndup(cmdline + offset, size);
            offset += size + 1;
            /* The launcher passes its own --project-root. */
            if (!index || g_str_has_prefix(argument, "--project-root=")) { g_free(argument); continue; }
            if (g_str_equal(argument, "--project-root")) {
                g_free(argument); offset += offset < length ? strnlen(cmdline + offset, length - offset) + 1 : 0; continue;
            }
            g_ptr_array_add(original, argument);
        }
    g_free(cmdline);
    gchar *launcher = g_build_filename(ctx->config->project_root, "run-radios.sh", NULL);
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    gboolean sandbox = g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS);
    gchar *spawn = sandbox ? g_find_program_in_path("flatpak-spawn") : NULL;
    if (sandbox && spawn) {
        g_ptr_array_add(argv, spawn); g_ptr_array_add(argv, g_strdup("--host"));
        g_ptr_array_add(argv, g_strdup_printf("--directory=%s", ctx->config->project_root));
    } else g_free(spawn);
    g_ptr_array_add(argv, g_strdup("sh")); g_ptr_array_add(argv, g_strdup("-c"));
    g_ptr_array_add(argv, g_strdup("sleep 2; exec setsid \"$0\" \"$@\" > /dev/null 2>&1 < /dev/null"));
    g_ptr_array_add(argv, launcher);
    for (guint i = 0; i < original->len; ++i) g_ptr_array_add(argv, g_strdup(g_ptr_array_index(original, i)));
    g_ptr_array_add(argv, NULL);
    gboolean started = g_spawn_async(ctx->config->project_root, (gchar **)argv->pdata, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, error);
    g_ptr_array_unref(argv); g_ptr_array_unref(original);
    if (started && ctx->parent) gtk_window_close(ctx->parent);
    return started;
}
