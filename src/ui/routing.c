#include "radios/views.h"
#include "radios/widgets.h"
#include "radios/data.h"
#include "radios/audio_patchbay.h"
#include "radios/audio_ui.h"
#include "radios/routing_ui.h"
#include "radios/routing_graph_ui.h"
#include "radios/stream_metadata.h"
#include <math.h>
#include <string.h>

/* One named patchbay configuration of the station's "audio_routing" document. */
typedef struct { gchar *name; RadioSRoutingGraph *graph; GHashTable *passwords; } Config;
/* Widgets of the open settings panel; all NULL except those of its kind. */
typedef struct {
    guint id;
    GtkWidget *gain, *channel, *device, *push_to_talk, *as_microphone, *applications, *move, *virtual_info;
    GtkWidget *folder, *choose, *prefix, *split, *record_state, *record_time, *record_size, *record_file;
    GtkWidget *record_start, *record_pause, *record_split, *record_stop;
    GtkWidget *protocol, *host, *port, *mount, *stream_id, *username, *password, *bitrate;
    GtkWidget *password_show, *password_save, *stream_light;
    gint light_state; gboolean light_dim;
    GtkWidget *station, *genre, *description, *website, *public_listing;
    GtkWidget *stream_start, *stream_stop, *stream_state, *metadata, *metadata_send;
    GtkWidget *metadata_source, *metadata_player, *metadata_format, *metadata_source_state;
    GtkWidget *metadata_player_label, *metadata_format_label;
    GtkWidget *meter, *meter_label, *state, *remove, *name;
    guint inventory;
} Panel;
/* Automatic song text per stream connection. A failed title is retried with a
 * growing delay and a fixed budget; a rejected request (4xx) is not repeated. */
typedef struct {
    gchar text[1025], candidate[1025];
    gint64 next_attempt_us;
    guint64 generation;
    guint attempts;
} MetadataDispatch;
#define METADATA_RETRY_US (5 * G_TIME_SPAN_SECOND)
#define METADATA_RETRY_MAX_US (60 * G_TIME_SPAN_SECOND)
#define METADATA_ATTEMPTS 5u
typedef struct {
    RadioSViewContext *ctx;
    RadioSAudioPatchbay *patchbay;
    RadioSAudioPatchbaySnapshot *snapshot;
    RadioSDocument *document;
    RadioSDataClient *password_client;
    GtkWidget *page, *configs, *save, *save_as, *remove_config, *refresh, *run, *run_state, *status;
    GPtrArray *items;
    guint current;
    RadioSRoutingGraph graph;
    RadioSRoutingGraphUI *graph_ui;
    Panel panel;
    guint timer, inventory_hash, autosave;
    GHashTable *metadata_dispatch;
    gint64 metadata_poll_us;
    guint64 intent;
    gint start_action;
    guint start_node;
    gboolean authorizing, running, disposed, syncing, notice_active, engine_bound;
    gfloat meter_peak[2], meter_rms[2];
} RoutingPage;
enum { ACTION_RUN, ACTION_RECORD, ACTION_STREAM };
typedef struct {
    guint64 intent;
    gint action;
    guint node;
    gchar *password, *station_id, *user_id;
    RadioSDataClient *client;
} StartJob;

static void pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static void named(GtkWidget *widget, const gchar *name) { gtk_widget_set_name(widget, name); }
static void notice(RoutingPage *p, const gchar *text)
{
    if (p->disposed) return;
    gtk_label_set_text(GTK_LABEL(p->status), text); p->notice_active = TRUE;
    if (p->ctx->notify) p->ctx->notify(p->ctx->notify_data, text);
}
static JsonObject *context_object(JsonObject *parent, const gchar *name)
{
    JsonNode *node = parent ? json_object_get_member(parent, name) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static gboolean allowed(JsonObject *context, const gchar *permission)
{
    JsonObject *user = context_object(context, "user"), *station = context_object(context, "station");
    const gchar *uid = radios_json_string(user, "_id", ""), *sid = radios_json_string(station, "_id", "");
    JsonNode *permissions = context ? json_object_get_member(context, "permissions") : NULL;
    JsonArray *grants = permissions && JSON_NODE_HOLDS_ARRAY(permissions) ? json_node_get_array(permissions) : NULL;
    if (!uid[0] || !sid[0] || !grants || !g_str_equal(radios_json_string(user, "status", ""), "active")) return FALSE;
    for (guint i = 0; i < json_array_get_length(grants); ++i) {
        JsonNode *entry = json_array_get_element(grants, i);
        JsonObject *grant = JSON_NODE_HOLDS_OBJECT(entry) ? json_node_get_object(entry) : NULL;
        if (!grant || g_strcmp0(radios_json_string(grant, "user_id", ""), uid) ||
            g_strcmp0(radios_json_string(grant, "permission", ""), permission)) continue;
        JsonNode *scope = json_object_get_member(grant, "station_id");
        if (scope && (JSON_NODE_HOLDS_NULL(scope) || g_strcmp0(radios_json_string(grant, "station_id", ""), sid) == 0)) return TRUE;
    }
    return FALSE;
}
static const gchar *combo_id(GtkWidget *combo)
{ const gchar *id = combo ? gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)) : NULL; return id ? id : ""; }
static gdouble level_db(gfloat value) { return value > 0 && isfinite(value) ? MAX(-60, 20 * log10(value)) : -60; }

/* Configurations */
static void password_free(gpointer data)
{
    gchar *password = data; if (!password) return;
    gsize length = strlen(password); volatile gchar *wipe = password;
    while (length--) *wipe++ = 0;
    g_free(password);
}
static void config_free(gpointer data)
{
    Config *config = data; g_hash_table_unref(config->passwords);
    g_free(config->name); g_free(config->graph); g_free(config);
}
static Config *config_new(const gchar *name, const RadioSRoutingGraph *graph)
{
    Config *config = g_new0(Config, 1); config->name = g_strdup(name); config->graph = g_new0(RadioSRoutingGraph, 1);
    config->passwords = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, password_free);
    if (graph) *config->graph = *graph; else radios_routing_graph_default(config->graph);
    return config;
}
static Config *current_config(RoutingPage *p)
{ return p->current < p->items->len ? g_ptr_array_index(p->items, p->current) : NULL; }
static void store_current(RoutingPage *p)
{ Config *config = current_config(p); if (config) *config->graph = p->graph; }
static void password_identity_sync(RoutingPage *p)
{
    RadioSDataClient *current = radios_data_client_acquire(p->ctx);
    gboolean same = (!current && !p->password_client) || radios_data_client_same_identity(current, p->password_client);
    if (!same) {
        for (guint i = 0; i < p->items->len; ++i)
            g_hash_table_remove_all(((Config *)g_ptr_array_index(p->items, i))->passwords);
        gboolean syncing = p->syncing; p->syncing = TRUE;
        /* Session passwords belong to the previous identity. A password the
         * operator saved with the block is part of the configuration itself
         * and stays in the form. */
        const RadioSRoutingGraphNode *node = radios_routing_graph_find_const(&p->graph, p->panel.id);
        const gchar *saved = node && node->kind == RG_STREAM && node->password_saved ? node->password : "";
        if (p->panel.password) gtk_entry_set_text(GTK_ENTRY(p->panel.password), saved);
        if (p->panel.password_show) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->panel.password_show), FALSE);
        p->syncing = syncing;
        g_clear_pointer(&p->password_client, radios_data_client_unref);
        p->password_client = current; current = NULL;
    }
    if (current) radios_data_client_unref(current);
}
static void config_passwords_copy(Config *target, const Config *source)
{
    for (guint i = 0; i < target->graph->node_count; ++i) {
        const RadioSRoutingGraphNode *node = &target->graph->nodes[i];
        const RadioSRoutingGraphNode *old = radios_routing_graph_find_const(source->graph, node->id);
        const gchar *password = g_hash_table_lookup(source->passwords, GUINT_TO_POINTER(node->id));
        if (password && old && old->kind == RG_STREAM && node->kind == RG_STREAM &&
            old->protocol == node->protocol && old->port == node->port && old->stream_id == node->stream_id &&
            !g_strcmp0(old->host, node->host) && !g_strcmp0(old->mount, node->mount) && !g_strcmp0(old->username, node->username))
            g_hash_table_replace(target->passwords, GUINT_TO_POINTER(node->id), g_strdup(password));
    }
}
static void fill_configs(RoutingPage *p)
{
    p->syncing = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(p->configs));
    for (guint i = 0; i < p->items->len; ++i) {
        Config *config = g_ptr_array_index(p->items, i); gchar id[16]; g_snprintf(id, sizeof id, "%u", i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->configs), id, config->name);
    }
    gchar id[16]; g_snprintf(id, sizeof id, "%u", p->current);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->configs), id);
    gtk_widget_set_sensitive(p->remove_config, p->items->len > 1);
    p->syncing = FALSE;
}
static JsonNode *document_body(RoutingPage *p)
{
    store_current(p);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(node, object);
    json_object_set_int_member(object, "schema_version", 2);
    Config *active = current_config(p);
    json_object_set_string_member(object, "active", active ? active->name : "Varsayılan");
    JsonArray *configs = json_array_new();
    for (guint i = 0; i < p->items->len; ++i) {
        Config *config = g_ptr_array_index(p->items, i); JsonObject *item = json_object_new();
        json_object_set_string_member(item, "name", config->name);
        JsonObject *graph = radios_routing_graph_to_json(config->graph);
        if (!graph) { RadioSRoutingGraph empty = {0}; graph = radios_routing_graph_to_json(&empty); }
        json_object_set_object_member(item, "graph", graph); json_array_add_object_element(configs, item);
    }
    json_object_set_array_member(object, "configs", configs); return node;
}
static void apply_runtime(RoutingPage *p)
{
    if (!p->patchbay) return;
    GError *error = NULL;
    if (!radios_audio_patchbay_apply(p->patchbay, &p->graph, p->running, &error))
        notice(p, error ? error->message : "Yönlendirme uygulanamadı.");
    g_clear_error(&error);
}
static void panel_build(RoutingPage *p, guint id);
static gboolean panel_flush(RoutingPage *p);
static void panel_update(RoutingPage *p);
static void metadata_panel_update(RoutingPage *p);
/* Every edit reaches the database shortly after it is made; Kaydet saves at once. */
static gboolean autosave_due(gpointer data)
{
    RoutingPage *p = data; p->autosave = 0;
    if (p->disposed || !p->document || !radios_data_document_ready(p->document) ||
        !allowed(radios_data_context(p->ctx), "manage") || !panel_flush(p)) return G_SOURCE_REMOVE;
    JsonNode *node = document_body(p);
    radios_data_document_autosave(p->document, node);
    json_node_free(node); return G_SOURCE_REMOVE;
}
static void autosave_schedule(RoutingPage *p)
{
    if (p->disposed) return;
    if (p->autosave) g_source_remove(p->autosave);
    p->autosave = g_timeout_add(1500, autosave_due, p);
}
static void graph_changed(gpointer data)
{
    RoutingPage *p = data;
    if (p->disposed || p->syncing) return;
    store_current(p);
    Config *config = current_config(p);
    if (config) {
        GHashTableIter iter; gpointer key;
        g_hash_table_iter_init(&iter, config->passwords);
        while (g_hash_table_iter_next(&iter, &key, NULL)) {
            const RadioSRoutingGraphNode *node = radios_routing_graph_find_const(&p->graph, GPOINTER_TO_UINT(key));
            if (!node || node->kind != RG_STREAM) g_hash_table_iter_remove(&iter);
        }
    }
    if (p->document) radios_data_document_dirty(p->document, TRUE);
    autosave_schedule(p);
    apply_runtime(p); p->notice_active = FALSE;
}
static void selection_changed(guint id, gpointer data)
{ RoutingPage *p = data; if (!p->disposed) panel_build(p, id); }
static void load_config(RoutingPage *p, guint index)
{
    if (index >= p->items->len) return;
    g_hash_table_remove_all(p->metadata_dispatch);
    p->current = index; p->graph = *((Config *)g_ptr_array_index(p->items, index))->graph;
    radios_routing_graph_ui_select(p->graph_ui, 0); radios_routing_graph_ui_refresh(p->graph_ui);
    fill_configs(p); apply_runtime(p);
}
static void config_selected(GtkComboBox *combo, gpointer data)
{
    RoutingPage *p = data; (void)combo;
    if (p->syncing || p->disposed) return;
    guint64 index = 0;
    if (!g_ascii_string_to_unsigned(combo_id(p->configs), 10, 0, p->items->len - 1, &index, NULL) || index == p->current) return;
    store_current(p); load_config(p, (guint)index);
    Config *config = current_config(p);
    gchar *text = g_strdup_printf("“%s” yapılandırması yüklendi%s.", config->name,
        p->running ? " ve çalışan yönlendirmeye uygulandı" : "");
    notice(p, text); g_free(text);
    if (p->document) radios_data_document_dirty(p->document, TRUE);
    autosave_schedule(p);
}
static gboolean graph_equal(const RadioSRoutingGraph *first, const RadioSRoutingGraph *second)
{
    /* Compare persisted values, not struct padding or unused string tails. */
    JsonObject *a = radios_routing_graph_to_json(first), *b = radios_routing_graph_to_json(second);
    if (!a || !b) { if (a) json_object_unref(a); if (b) json_object_unref(b); return FALSE; }
    JsonNode *left = json_node_new(JSON_NODE_OBJECT), *right = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(left, a); json_node_take_object(right, b);
    gboolean same = json_node_equal(left, right);
    json_node_free(left); json_node_free(right); return same;
}
static void settings_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *o, gpointer data)
{
    (void)ctx; RoutingPage *p = data;
    if (p->disposed) return;
    password_identity_sync(p);
    GPtrArray *items = g_ptr_array_new_with_free_func(config_free); guint active = 0; GError *error = NULL;
    if (radios_json_int(o, "schema_version", 1) == 2 && json_object_has_member(o, "configs") &&
        JSON_NODE_HOLDS_ARRAY(json_object_get_member(o, "configs"))) {
        JsonArray *configs = json_object_get_array_member(o, "configs");
        const gchar *wanted = radios_json_string(o, "active", "");
        for (guint i = 0; i < json_array_get_length(configs); ++i) {
            JsonObject *item = json_array_get_object_element(configs, i);
            JsonNode *graph_node = item ? json_object_get_member(item, "graph") : NULL;
            RadioSRoutingGraph *graph = g_new0(RadioSRoutingGraph, 1);
            if (graph_node && JSON_NODE_HOLDS_OBJECT(graph_node) && radios_routing_graph_from_json(graph, json_node_get_object(graph_node), &error)) {
                if (!g_strcmp0(radios_json_string(item, "name", ""), wanted)) active = items->len;
                g_ptr_array_add(items, config_new(radios_json_string(item, "name", "Yapılandırma"), graph));
            }
            g_clear_error(&error); g_free(graph);
        }
    } else {
        RadioSRoutingGraph *graph = g_new0(RadioSRoutingGraph, 1); radios_routing_graph_from_legacy(graph, o);
        g_ptr_array_add(items, config_new("Varsayılan", graph)); g_free(graph);
    }
    if (!items->len) g_ptr_array_add(items, config_new("Varsayılan", NULL));
    for (guint i = 0; i < items->len; ++i) {
        Config *target = g_ptr_array_index(items, i);
        for (guint j = 0; j < p->items->len; ++j) {
            Config *source = g_ptr_array_index(p->items, j);
            if (!g_strcmp0(target->name, source->name)) { config_passwords_copy(target, source); break; }
        }
    }
    Config *next = g_ptr_array_index(items, active), *current = current_config(p);
    gboolean same_graph = graph_equal(&p->graph, next->graph);
    gboolean keep_panel = same_graph && current && !g_strcmp0(current->name, next->name) &&
        !g_object_get_data(G_OBJECT(owner), "radios-data-explicit-reload");
    gboolean same_choices = p->current == active && p->items->len == items->len;
    for (guint i = 0; same_choices && i < items->len; ++i)
        same_choices = !g_strcmp0(((Config *)g_ptr_array_index(p->items, i))->name,
            ((Config *)g_ptr_array_index(items, i))->name);
    guint selected = radios_routing_graph_ui_selected(p->graph_ui);
    g_ptr_array_unref(p->items); p->items = items; p->syncing = TRUE;
    p->current = active; p->graph = *((Config *)g_ptr_array_index(items, active))->graph;
    if (!same_graph) radios_routing_graph_ui_refresh(p->graph_ui);
    p->syncing = FALSE;
    if (!same_choices) fill_configs(p);
    /* Autosave acknowledgements return the graph already shown. Rebuilding
     * its form every time destroys keyboard focus, selection and open menus
     * while the operator is entering the source connection details. */
    if (radios_routing_graph_find(&p->graph, selected)) {
        if (!keep_panel) panel_build(p, selected);
    }
    else radios_routing_graph_ui_select(p->graph_ui, 0);
    apply_runtime(p);
    gchar *text = g_strdup_printf("%u yönlendirme yapılandırması veritabanından yüklendi. Etkin: “%s”.",
        p->items->len, current_config(p)->name);
    notice(p, text); g_free(text);
}
static void save_settings(GtkButton *button, gpointer data)
{
    (void)button; RoutingPage *p = data;
    if (!panel_flush(p)) { notice(p, "Blok ayarları geçersiz; alanları düzeltip yeniden kaydedin."); return; }
    JsonNode *node = document_body(p);
    if (p->document && radios_data_document_save(p->document, node)) notice(p, "Yönlendirme yapılandırmaları veritabanına kaydediliyor.");
    else notice(p, "Veritabanı kaydı hazır değil; değişiklikleriniz korunuyor.");
    json_node_free(node);
}
static gchar *ask_name(RoutingPage *p, const gchar *title, const gchar *initial)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, p->ctx->parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    named(dialog, "routing-config-name-dialog");
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    GtkWidget *entry = gtk_entry_new(); named(entry, "routing-config-name"); gtk_entry_set_max_length(GTK_ENTRY(entry), 80);
    gtk_entry_set_text(GTK_ENTRY(entry), initial); gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_box_pack_start(GTK_BOX(area), gtk_label_new("Yapılandırma adı"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(area), entry, FALSE, FALSE, 6); gtk_widget_show_all(dialog);
    gchar *result = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) result = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
    gtk_widget_destroy(dialog); return result;
}
static void save_as(GtkButton *button, gpointer data)
{
    (void)button; RoutingPage *p = data;
    gchar *initial = g_strdup_printf("%s (kopya)", current_config(p) ? current_config(p)->name : "Yapılandırma");
    gchar *name = ask_name(p, "Yapılandırmayı farklı kaydet", initial); g_free(initial);
    if (!name || p->disposed) { g_free(name); return; }
    gboolean unique = name[0] != 0 && g_utf8_strlen(name, -1) <= 80;
    for (guint i = 0; unique && i < p->items->len; ++i) unique = g_strcmp0(((Config *)g_ptr_array_index(p->items, i))->name, name) != 0;
    if (!unique || p->items->len >= 32) {
        notice(p, p->items->len >= 32 ? "En çok 32 yapılandırma saklanabilir." : "Yapılandırma adı boş olamaz ve başka bir yapılandırmayla aynı olamaz.");
        g_free(name); return;
    }
    store_current(p); Config *copy = config_new(name, &p->graph);
    Config *source = current_config(p); if (source) config_passwords_copy(copy, source);
    g_ptr_array_add(p->items, copy); p->current = p->items->len - 1;
    fill_configs(p); g_free(name); save_settings(NULL, p);
}
static void remove_config(GtkButton *button, gpointer data)
{
    (void)button; RoutingPage *p = data; Config *config = current_config(p);
    if (!config || p->items->len < 2) return;
    GtkWidget *dialog = gtk_message_dialog_new(p->ctx->parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "“%s” yapılandırması silinsin mi?", config->name);
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Vazgeç", GTK_RESPONSE_CANCEL, "Sil", GTK_RESPONSE_ACCEPT, NULL);
    named(dialog, "routing-config-remove-dialog");
    gint response = gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
    if (response != GTK_RESPONSE_ACCEPT || p->disposed) return;
    g_ptr_array_remove_index(p->items, p->current); load_config(p, 0); save_settings(NULL, p);
}
static void refresh(GtkButton *button, gpointer data)
{
    (void)button; RoutingPage *p = data;
    if (p->patchbay) radios_audio_patchbay_refresh(p->patchbay);
    p->inventory_hash = 0; p->notice_active = FALSE;
    if (p->document) radios_data_document_reload(p->document);
}

/* Starting needs a fresh operate grant; stopping never does. */
static void start_job_free(gpointer data)
{
    StartJob *job = data;
    if (job->client) radios_data_client_unref(job->client);
    if (job->password) { memset(job->password, 0, strlen(job->password)); g_free(job->password); }
    g_free(job->station_id); g_free(job->user_id); g_free(job);
}
static void run_action(RoutingPage *p, gint action, guint node, const gchar *password)
{
    GError *error = NULL; gboolean okay = TRUE;
    if (!p->running) { p->running = TRUE; apply_runtime(p); }
    if (action == ACTION_RECORD) okay = radios_audio_patchbay_record(p->patchbay, node, &error);
    if (action == ACTION_STREAM) okay = radios_audio_patchbay_stream_start(p->patchbay, node, password, &error);
    const RadioSRoutingGraphNode *block = radios_routing_graph_find(&p->graph, node);
    gchar *text = !okay ? g_strdup(error ? error->message : "Başlatılamadı.") :
        action == ACTION_RECORD ? g_strdup_printf("“%s” kayda başladı.", radios_routing_graph_node_title(block)) :
        action == ACTION_STREAM ? g_strdup_printf("“%s” sunucuya bağlanıyor…", radios_routing_graph_node_title(block)) :
        g_strdup("Yönlendirme çalışıyor: bloklar gerçek sesle bağlandı.");
    notice(p, text); g_free(text); g_clear_error(&error);
}
static void start_authorized(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *failure, gpointer data)
{
    StartJob *job = data;
    RoutingPage *p = g_object_get_data(G_OBJECT(owner), "routing-page-state");
    if (!p || p->disposed || job->intent != p->intent || !p->authorizing) return;
    p->authorizing = FALSE; radios_routing_graph_ui_busy(p->graph_ui, FALSE);
    RadioSDataClient *client = radios_data_client_acquire(ctx);
    gboolean current = ctx == p->ctx && client && client == job->client &&
        !g_strcmp0(radios_data_client_station_id(client), job->station_id) && !g_strcmp0(radios_data_client_user_id(client), job->user_id);
    if (client) radios_data_client_unref(client);
    if (!current) { notice(p, "Veritabanı oturumu değişti; yönlendirme başlatılmadı."); return; }
    JsonObject *context = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    JsonObject *user = context_object(context, "user"), *station = context_object(context, "station");
    if (failure || !context || g_strcmp0(radios_json_string(user, "_id", ""), job->user_id) ||
        g_strcmp0(radios_json_string(station, "_id", ""), job->station_id) || !allowed(context, "operate")) {
        notice(p, failure ? failure->message : "Ses yönlendirme için bu istasyonda güncel operate yetkisi gerekiyor."); return;
    }
    run_action(p, job->action, job->node, job->password);
}
static void request_start(RoutingPage *p, gint action, guint node, const gchar *password)
{
    if (!p->patchbay) { notice(p, "Ses yönlendirme için uygulamayı veritabanıyla açın."); return; }
    if (p->authorizing) return;
    if (p->running) { run_action(p, action, node, password); return; }
    RadioSDataClient *client = radios_data_client_acquire(p->ctx);
    if (!client) { notice(p, "Veritabanı bağlantısı hazır değil."); return; }
    StartJob *job = g_new0(StartJob, 1); job->client = client; job->action = action; job->node = node;
    job->password = g_strdup(password); job->station_id = g_strdup(radios_data_client_station_id(client));
    job->user_id = g_strdup(radios_data_client_user_id(client));
    if (!job->station_id || !*job->station_id || !job->user_id || !*job->user_id) {
        notice(p, "Veritabanı oturumu hazır değil."); start_job_free(job); return;
    }
    job->intent = ++p->intent; p->authorizing = TRUE; p->start_action = action; p->start_node = node;
    radios_routing_graph_ui_busy(p->graph_ui, TRUE);
    JsonNode *body = radios_data_body(p->ctx);
    gboolean accepted = radios_data_request(p->ctx, p->page, "POST", "/v1/workflows/desktop/context", body, start_authorized, job, start_job_free);
    json_node_free(body);
    if (!accepted) { p->authorizing = FALSE; radios_routing_graph_ui_busy(p->graph_ui, FALSE); notice(p, "Veritabanı bağlantısı hazır değil."); }
    else notice(p, "Ses yönlendirme yetkisi doğrulanıyor…");
}
static void run_clicked(GtkButton *button, gpointer data)
{
    (void)button; RoutingPage *p = data;
    if (p->running) {
        ++p->intent; p->authorizing = FALSE; radios_routing_graph_ui_busy(p->graph_ui, FALSE);
        p->running = FALSE; apply_runtime(p);
        notice(p, "Yönlendirme durduruldu; kayıtlar tamamlandı, yayınlar kapatıldı. Sanal arayüzler tanımlı kalır.");
    } else request_start(p, ACTION_RUN, 0, NULL);
}

/* Settings panel of the selected block. */
static RadioSRoutingGraphNode *panel_node(RoutingPage *p) { return radios_routing_graph_find(&p->graph, p->panel.id); }
static void stream_fields_update(Panel *w)
{
    if (!w->protocol) return;
    gint protocol = gtk_combo_box_get_active(GTK_COMBO_BOX(w->protocol));
    gboolean icecast = protocol == RG_STREAM_ICECAST;
    gtk_widget_set_sensitive(w->mount, icecast);
    gtk_widget_set_sensitive(w->username, TRUE);
    gtk_widget_set_sensitive(w->stream_id, protocol == RG_STREAM_SHOUTCAST2);
    const gchar *info = icecast ? "Icecast yayın hesabının kullanıcı adı (genellikle source)." :
        "Bu SHOUTcast bağlantısı yayın şifresiyle kurulur; kullanıcı adı gönderilmez. Yönetim paneli hesabı ayrıdır.";
    gtk_widget_set_tooltip_text(w->username, info);
}
static gboolean cancel_stream(RoutingPage *p, guint id)
{
    gboolean cancelled = p->authorizing && p->start_action == ACTION_STREAM && p->start_node == id;
    if (cancelled) {
        ++p->intent; p->authorizing = FALSE; radios_routing_graph_ui_busy(p->graph_ui, FALSE);
    }
    if (p->patchbay) {
        radios_audio_patchbay_snapshot(p->patchbay, p->snapshot);
        for (guint i = 0; i < p->snapshot->node_count; ++i)
            if (p->snapshot->nodes[i].id == id) cancelled |= p->snapshot->nodes[i].streaming;
        radios_audio_patchbay_stream_stop(p->patchbay, id);
        radios_audio_patchbay_snapshot(p->patchbay, p->snapshot);
    }
    return cancelled;
}
static gboolean panel_commit(RoutingPage *p, RadioSRoutingGraphNode *copy)
{
    GError *error = NULL;
    gboolean okay = radios_routing_graph_update(&p->graph, copy, &error);
    if (okay) {
        radios_routing_graph_ui_refresh(p->graph_ui); graph_changed(p);
        radios_routing_graph_ui_message(p->graph_ui, "Blok ayarı güncellendi.");
    } else radios_routing_graph_ui_message(p->graph_ui, error ? error->message : "Ayar geçersiz.");
    g_clear_error(&error);
    return okay;
}
static void entry_text(GtkWidget *entry, gchar *target, gsize size)
{ if (entry) g_strlcpy(target, gtk_entry_get_text(GTK_ENTRY(entry)), size); }
static gboolean metadata_fields_read(RoutingPage *p, RadioSRoutingGraphNode *copy)
{
    Panel *w = &p->panel;
    if (!w->metadata_source) return TRUE;
    const gchar *source = combo_id(w->metadata_source);
    copy->metadata_source = !g_strcmp0(source, "player") ? RG_METADATA_PLAYER :
        !g_strcmp0(source, "automation") ? RG_METADATA_AUTOMATION : RG_METADATA_MANUAL;
    guint64 deck = 0;
    if (!g_ascii_string_to_unsigned(combo_id(w->metadata_player), 10, 0, 31, &deck, NULL)) return FALSE;
    copy->metadata_player = (guint)deck;
    const gchar *format = gtk_entry_get_text(GTK_ENTRY(w->metadata_format));
    const gchar *manual = gtk_entry_get_text(GTK_ENTRY(w->metadata));
    if (!g_utf8_validate(format, -1, NULL) || strlen(format) >= sizeof copy->metadata_format ||
        (copy->metadata_source == RG_METADATA_MANUAL &&
         (!g_utf8_validate(manual, -1, NULL) || strlen(manual) >= sizeof copy->metadata_manual))) {
        notice(p, "Şarkı bilgisi veya biçim çok uzun; metni kısaltın."); return FALSE;
    }
    g_strlcpy(copy->metadata_format, format, sizeof copy->metadata_format);
    if (copy->metadata_source == RG_METADATA_MANUAL)
        g_strlcpy(copy->metadata_manual, manual, sizeof copy->metadata_manual);
    return TRUE;
}
static void metadata_changed(GtkWidget *widget, gpointer data)
{
    RoutingPage *p = data;
    if (p->syncing || p->disposed) return;
    RadioSRoutingGraphNode *node = panel_node(p);
    if (!node || node->kind != RG_STREAM) return;
    if (widget == p->panel.metadata_source) {
        gboolean manual = !g_strcmp0(combo_id(p->panel.metadata_source), "manual");
        p->syncing = TRUE;
        gtk_entry_set_text(GTK_ENTRY(p->panel.metadata), manual ? node->metadata_manual : "");
        p->syncing = FALSE;
    }
    RadioSRoutingGraphNode copy = *node;
    if (!metadata_fields_read(p, &copy)) return;
    if (memcmp(node, &copy, sizeof copy) && panel_commit(p, &copy))
        g_hash_table_remove(p->metadata_dispatch, GUINT_TO_POINTER(copy.id));
    metadata_panel_update(p);
}
static gboolean panel_apply(RoutingPage *p)
{
    if (p->syncing || p->disposed) return FALSE;
    RadioSRoutingGraphNode *node = panel_node(p); if (!node) return TRUE;
    RadioSRoutingGraphNode copy = *node; Panel *w = &p->panel;
    if (w->name) {
        const gchar *value = gtk_entry_get_text(GTK_ENTRY(w->name));
        g_strlcpy(copy.name, g_strcmp0(value, radios_routing_graph_kind_title(copy.kind)) ? value : "", sizeof copy.name);
        g_strstrip(copy.name);
    }
    if (w->gain) copy.gain_db = gtk_range_get_value(GTK_RANGE(w->gain));
    if (w->channel) copy.channel = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w->channel));
    if (w->device) {
        const gchar *id = combo_id(w->device);
        g_strlcpy(copy.device, g_str_has_prefix(id, "missing:") ? id + 8 : id, sizeof copy.device);
    }
    if (w->push_to_talk) copy.push_to_talk = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->push_to_talk));
    if (w->as_microphone) copy.as_microphone = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->as_microphone));
    entry_text(w->folder, copy.folder, sizeof copy.folder); entry_text(w->prefix, copy.prefix, sizeof copy.prefix);
    if (w->split) copy.split_minutes = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w->split));
    if (w->protocol) copy.protocol = (RadioSRoutingStreamProtocol)gtk_combo_box_get_active(GTK_COMBO_BOX(w->protocol));
    entry_text(w->host, copy.host, sizeof copy.host); entry_text(w->mount, copy.mount, sizeof copy.mount);
    entry_text(w->username, copy.username, sizeof copy.username);
    if (w->port) copy.port = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w->port));
    if (w->stream_id) copy.stream_id = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w->stream_id));
    if (w->bitrate) copy.bitrate_kbps = (guint)g_ascii_strtoull(combo_id(w->bitrate), NULL, 10);
    entry_text(w->station, copy.station, sizeof copy.station); entry_text(w->genre, copy.genre, sizeof copy.genre);
    entry_text(w->description, copy.description, sizeof copy.description); entry_text(w->website, copy.website, sizeof copy.website);
    if (w->public_listing) copy.public_listing = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->public_listing));
    if (w->password_save) {
        /* Only a ticked "Şifreyi kaydet" moves the password into the block (and
         * so into the saved document); otherwise the block carries none. */
        copy.password_saved = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w->password_save));
        if (copy.password_saved && w->password) entry_text(w->password, copy.password, sizeof copy.password);
        else copy.password[0] = 0;
    }
    if (!metadata_fields_read(p, &copy)) return FALSE;
    stream_fields_update(w);
    return !memcmp(node, &copy, sizeof copy) || panel_commit(p, &copy);
}
static void panel_changed(GtkWidget *widget, gpointer data)
{
    RoutingPage *p = data;
    if (p->syncing || p->disposed) return;
    Panel *w = &p->panel;
    if (widget == w->password && w->password) {
        Config *config = current_config(p); const gchar *password = gtk_entry_get_text(GTK_ENTRY(w->password));
        if (config) {
            if (*password) g_hash_table_replace(config->passwords, GUINT_TO_POINTER(w->id), g_strdup(password));
            else g_hash_table_remove(config->passwords, GUINT_TO_POINTER(w->id));
        }
    }
    GtkWidget *settings[] = {w->protocol, w->host, w->port, w->mount, w->stream_id, w->username,
        w->password, w->bitrate, w->station, w->genre, w->description, w->website, w->public_listing};
    gboolean cancelled = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(settings); ++i)
        if (widget && widget == settings[i]) { cancelled = cancel_stream(p, w->id); break; }
    panel_apply(p);
    if (cancelled) notice(p, "Yayın ayarları değişti; bağlantı ve yeniden deneme durduruldu. Yeni bilgilerle Yayını başlat'a basın.");
    panel_update(p);
}
static gboolean panel_flush(RoutingPage *p)
{
    GtkWidget *spins[] = {p->panel.channel, p->panel.split, p->panel.port, p->panel.stream_id};
    for (guint i = 0; i < G_N_ELEMENTS(spins); ++i)
        if (spins[i]) gtk_spin_button_update(GTK_SPIN_BUTTON(spins[i]));
    return panel_apply(p);
}
static GtkWidget *section(GtkWidget *box, const gchar *text)
{
    GtkWidget *label = radios_ui_label(text, "routing-graph-section"); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), "routing-graph-section"); pack(box, label, FALSE); return label;
}
static GtkWidget *caption(GtkWidget *box, const gchar *text)
{
    GtkWidget *label = radios_ui_label(text, "routing-graph-caption"); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(label), 34); pack(box, label, FALSE); return label;
}
static GtkWidget *field(RoutingPage *p, GtkWidget *box, const gchar *label, GtkWidget *widget, const gchar *id, const gchar *signal)
{
    if (label) section(box, label);
    named(widget, id); pack(box, widget, FALSE);
    if (signal) g_signal_connect(widget, signal, G_CALLBACK(panel_changed), p);
    return widget;
}
static GtkWidget *text_field(RoutingPage *p, GtkWidget *box, const gchar *label, const gchar *value, gint limit, const gchar *id)
{
    GtkWidget *entry = gtk_entry_new(); gtk_entry_set_max_length(GTK_ENTRY(entry), limit); gtk_entry_set_text(GTK_ENTRY(entry), value);
    return field(p, box, label, entry, id, "changed");
}
static GtkWidget *spin_field(RoutingPage *p, GtkWidget *box, const gchar *label, gdouble low, gdouble high, guint value, const gchar *id)
{
    GtkWidget *spin = gtk_spin_button_new_with_range(low, high, 1); gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
    return field(p, box, label, spin, id, "value-changed");
}
static GtkWidget *button(RoutingPage *p, GtkWidget *box, const gchar *text, const gchar *id, GCallback callback, const gchar *style)
{
    GtkWidget *widget = radios_ui_button(text); named(widget, id);
    if (style) gtk_style_context_add_class(gtk_widget_get_style_context(widget), style);
    g_signal_connect(widget, "clicked", callback, p); pack(box, widget, FALSE); return widget;
}
static const gchar *device_label(RoutingPage *p, const gchar *name, gboolean input)
{
    const RadioSPatchDevice *list = input ? p->snapshot->inputs : p->snapshot->outputs;
    guint count = input ? p->snapshot->input_count : p->snapshot->output_count;
    for (guint i = 0; i < count; ++i) if (g_str_equal(list[i].name, name)) return list[i].description;
    return NULL;
}
static void device_choices(RoutingPage *p)
{
    RadioSRoutingGraphNode *node = panel_node(p); Panel *w = &p->panel;
    if (!node || !w->device) return;
    gboolean input = node->kind != RG_OUTPUT;
    p->syncing = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(w->device));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w->device), "", input ? "Giriş aygıtı seç…" : "Çıkış aygıtı seç…");
    if (node->kind == RG_MICROPHONE)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w->device), RADIOS_ROUTING_TALK_MICROPHONE, "Konuşma mikrofonu (bas-konuş · Ayarlar › Ses)");
    const RadioSPatchDevice *list = input ? p->snapshot->inputs : p->snapshot->outputs;
    guint count = input ? p->snapshot->input_count : p->snapshot->output_count; gboolean found = !node->device[0] ||
        !g_strcmp0(node->device, RADIOS_ROUTING_TALK_MICROPHONE);
    for (guint i = 0; i < count; ++i) {
        gchar *label = list[i].monitor ? g_strdup_printf("Çıkış monitörü · %s", list[i].description) : g_strdup(list[i].description);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w->device), list[i].name, label); g_free(label);
        found |= g_str_equal(list[i].name, node->device);
    }
    if (!found) {
        gchar *id = g_strdup_printf("missing:%s", node->device), *label = g_strdup_printf("Kullanılamıyor · %s", node->device);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w->device), id, label);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(w->device), id); g_free(id); g_free(label);
    } else gtk_combo_box_set_active_id(GTK_COMBO_BOX(w->device), node->device);
    p->syncing = FALSE;
}
static void application_choices(RoutingPage *p)
{
    Panel *w = &p->panel; if (!w->applications) return;
    gchar *selected = g_strdup(combo_id(w->applications));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(w->applications));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w->applications), "", p->snapshot->application_count ? "Ses çalan uygulama seç…" : "Ses çalan uygulama yok");
    for (guint i = 0; i < p->snapshot->application_count; ++i) {
        const RadioSPatchApplication *app = &p->snapshot->applications[i];
        gchar id[16]; g_snprintf(id, sizeof id, "%u", app->index);
        gchar *label = g_strdup_printf("%s%s%s", app->name, app->binary[0] ? " · " : "", app->binary);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w->applications), id, label); g_free(label);
    }
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(w->applications), selected)) gtk_combo_box_set_active_id(GTK_COMBO_BOX(w->applications), "");
    g_free(selected);
}
static guint inventory_hash(const RadioSAudioPatchbaySnapshot *s)
{
    GString *text = g_string_new(NULL);
    for (guint i = 0; i < s->output_count; ++i) g_string_append_printf(text, "o%s|%s\n", s->outputs[i].name, s->outputs[i].description);
    for (guint i = 0; i < s->input_count; ++i) g_string_append_printf(text, "i%s|%s\n", s->inputs[i].name, s->inputs[i].description);
    for (guint i = 0; i < s->application_count; ++i) g_string_append_printf(text, "a%u|%s\n", s->applications[i].index, s->applications[i].name);
    guint hash = g_str_hash(text->str); g_string_free(text, TRUE); return hash ? hash : 1;
}
static void choose_folder(GtkButton *widget, gpointer data)
{
    (void)widget; RoutingPage *p = data; Panel *w = &p->panel; if (!w->folder) return;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Kayıt klasörü", p->ctx->parent, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Seç", GTK_RESPONSE_ACCEPT, NULL);
    const gchar *current = gtk_entry_get_text(GTK_ENTRY(w->folder));
    const gchar *start = current[0] ? current : p->ctx->config->recording_root;
    if (start && g_file_test(start, G_FILE_TEST_IS_DIR)) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), start);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT && !p->disposed && p->panel.folder) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (path) gtk_entry_set_text(GTK_ENTRY(p->panel.folder), path);
        g_free(path);
    }
    gtk_widget_destroy(dialog);
}
static void record_start(GtkButton *widget, gpointer data)
{ (void)widget; RoutingPage *p = data; request_start(p, ACTION_RECORD, p->panel.id, NULL); }
static void record_pause(GtkButton *widget, gpointer data)
{
    (void)widget; RoutingPage *p = data; GError *error = NULL;
    const RadioSPatchNodeSnapshot *s = NULL;
    for (guint i = 0; i < p->snapshot->node_count; ++i) if (p->snapshot->nodes[i].id == p->panel.id) s = &p->snapshot->nodes[i];
    if (!radios_audio_patchbay_record_pause(p->patchbay, p->panel.id, !(s && s->paused), &error))
        notice(p, error ? error->message : "Kayıt duraklatılamadı.");
    g_clear_error(&error);
}
static void record_split(GtkButton *widget, gpointer data)
{
    (void)widget; RoutingPage *p = data; GError *error = NULL;
    if (!radios_audio_patchbay_record_split(p->patchbay, p->panel.id, &error)) notice(p, error ? error->message : "Yeni kayıt dosyası açılamadı.");
    else notice(p, "Kayıt yeni bir WAV dosyasında sürüyor.");
    g_clear_error(&error);
}
static void record_stop(GtkButton *widget, gpointer data)
{
    (void)widget; RoutingPage *p = data;
    radios_audio_patchbay_record_stop(p->patchbay, p->panel.id); notice(p, "Kayıt durduruldu; WAV dosyası tamamlanıyor.");
}
static void stream_start(GtkButton *widget, gpointer data)
{
    (void)widget; RoutingPage *p = data;
    password_identity_sync(p);
    if (!panel_flush(p)) { notice(p, "Yayın ayarları geçersiz; sunucu alanlarını düzeltin."); return; }
    request_start(p, ACTION_STREAM, p->panel.id, p->panel.password ? gtk_entry_get_text(GTK_ENTRY(p->panel.password)) : "");
}
static void stream_stop(GtkButton *widget, gpointer data)
{
    (void)widget; RoutingPage *p = data;
    cancel_stream(p, p->panel.id); panel_update(p); notice(p, "İnternet yayını durduruldu.");
}
static void metadata_send(GtkWidget *widget, gpointer data)
{
    (void)widget; RoutingPage *p = data; GError *error = NULL;
    RadioSRoutingGraphNode *node = panel_node(p);
    if (!node || !p->patchbay || !p->panel.metadata) return;
    RadioSStreamMetadataSnapshot current = {0};
    const gchar *title = gtk_entry_get_text(GTK_ENTRY(p->panel.metadata));
    if (node->metadata_source != RG_METADATA_MANUAL) {
        if (!radios_stream_metadata_resolve(p->ctx, node->metadata_source,
                node->metadata_player, node->metadata_format, &current)) {
            notice(p, "Çalan parçanın şarkı bilgisi bekleniyor."); return;
        }
        title = current.text;
    }
    if (!title[0] || strlen(title) > RADIOS_STREAM_METADATA_TEXT_BYTES) {
        notice(p, "Gönderilecek şarkı bilgisini yazın; uzun metni kısaltın."); return;
    }
    if (radios_audio_patchbay_stream_metadata(p->patchbay, p->panel.id, title, &error)) notice(p, "Şarkı bilgisi sunucuya gönderiliyor.");
    else notice(p, error ? error->message : "Şarkı bilgisi gönderilemedi.");
    g_clear_error(&error);
}
static void move_application(GtkButton *widget, gpointer data)
{
    (void)widget; RoutingPage *p = data; GError *error = NULL; guint64 index = 0;
    if (!g_ascii_string_to_unsigned(combo_id(p->panel.applications), 10, 0, G_MAXUINT32, &index, NULL)) {
        notice(p, "Önce ses çalan bir uygulama seçin."); return;
    }
    if (radios_audio_patchbay_move_application(p->patchbay, (guint32)index, p->panel.id, &error))
        notice(p, "Uygulamanın sesi sanal arayüze yönlendirildi; arayüzün çıkışını istediğiniz bloğa bağlayın.");
    else notice(p, error ? error->message : "Uygulama yönlendirilemedi.");
    g_clear_error(&error);
}
static void remove_block(GtkButton *widget, gpointer data)
{
    (void)widget; RoutingPage *p = data; guint id = p->panel.id;
    if (!id || !radios_routing_graph_remove(&p->graph, id, NULL)) return;
    radios_routing_graph_ui_select(p->graph_ui, 0); radios_routing_graph_ui_refresh(p->graph_ui); graph_changed(p);
    radios_routing_graph_ui_message(p->graph_ui, "Blok ve bağlantıları kaldırıldı.");
}
static gboolean meter_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    RoutingPage *p = data;
    gdouble width = gtk_widget_get_allocated_width(widget), height = gtk_widget_get_allocated_height(widget);
    gdouble bar = MIN(26, width / 5), top = 10, span = height - 24, left = width * .2, right = width * .62;
    for (guint channel = 0; channel < 2; ++channel) {
        gdouble x = channel ? right : left;
        cairo_set_source_rgba(cr, 0, .32, .40, .58); cairo_rectangle(cr, x, top, bar, span); cairo_fill(cr);
        gdouble level = (level_db(p->meter_peak[channel]) + 60) / 60;
        cairo_set_source_rgb(cr, level > .9 ? .96 : level > .75 ? .95 : .08, level > .9 ? .33 : level > .75 ? .78 : .79, level > .9 ? .28 : level > .75 ? .2 : .55);
        cairo_rectangle(cr, x, top + span * (1 - level), bar, span * level); cairo_fill(cr);
        gdouble average = (level_db(p->meter_rms[channel]) + 60) / 60;
        cairo_set_source_rgba(cr, 1, 1, 1, .92); cairo_set_line_width(cr, 2);
        cairo_move_to(cr, x - 2, top + span * (1 - average)); cairo_line_to(cr, x + bar + 2, top + span * (1 - average)); cairo_stroke(cr);
    }
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10); cairo_set_source_rgba(cr, 1, 1, 1, .9);
    const gint ticks[] = {0, -6, -12, -24, -36, -60};
    for (guint i = 0; i < G_N_ELEMENTS(ticks); ++i) {
        gchar label[16]; g_snprintf(label, sizeof label, "%d", ticks[i]);
        cairo_text_extents_t extents; cairo_text_extents(cr, label, &extents);
        cairo_move_to(cr, width * .5 - extents.width / 2 + bar / 2 - 4, top + span * -ticks[i] / 60 + 4); cairo_show_text(cr, label);
    }
    cairo_move_to(cr, left + bar / 2 - 3, height - 2); cairo_show_text(cr, "L");
    cairo_move_to(cr, right + bar / 2 - 3, height - 2); cairo_show_text(cr, "R");
    return FALSE;
}
static void password_visibility(GtkToggleButton *toggle, gpointer data)
{
    RoutingPage *p = data;
    if (!p->disposed && p->panel.password)
        gtk_entry_set_visibility(GTK_ENTRY(p->panel.password), gtk_toggle_button_get_active(toggle));
}
/* Saving or unsaving the password changes only what the document stores; the
 * running stream and its reconnects continue with the password in memory. */
static void password_save_toggled(GtkToggleButton *toggle, gpointer data)
{
    RoutingPage *p = data; (void)toggle;
    if (p->syncing || p->disposed) return;
    if (panel_apply(p)) {
        gboolean saved = p->panel.password_save && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->panel.password_save));
        notice(p, saved ? "Yayın şifresi bu blokla birlikte veritabanına kaydedilecek." :
            "Yayın şifresi veritabanından kaldırılıyor; yalnız bu oturumda tutulur.");
    }
    panel_update(p);
}
static void lights_style(GtkWidget *widget)
{
    static gsize installed = 0;
    if (!g_once_init_enter(&installed)) return;
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
        ".routing-light { font-size: 1.15em; }"
        ".routing-light-off { color: #4a5560; }"
        ".routing-light-connecting { color: #f2c230; }"
        ".routing-light-on { color: #3ddc84; }"
        ".routing-light-error { color: #ff5252; }"
        ".routing-light-dim { opacity: 0.25; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(widget), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    g_once_init_leave(&installed, 1);
}
static void panel_build(RoutingPage *p, guint id)
{
    password_identity_sync(p);
    GtkWidget *body = radios_routing_graph_ui_settings(p->graph_ui);
    GList *children = gtk_container_get_children(GTK_CONTAINER(body));
    for (GList *item = children; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(children);
    memset(&p->panel, 0, sizeof p->panel); p->panel.id = id;
    RadioSRoutingGraphNode *node = panel_node(p); if (!node) return;
    Panel *w = &p->panel; p->syncing = TRUE;
    caption(body, radios_routing_graph_kind_title(node->kind));
    w->name = text_field(p, body, "Blok adı", radios_routing_graph_node_title(node), 80, "routing-block-name");
    w->state = caption(body, ""); named(w->state, "routing-block-state");
    if (node->kind != RG_METER) {
        section(body, radios_routing_graph_has_output(node->kind) ? "Çıkış kazancı · dB" : "Giriş kazancı · dB");
        w->gain = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -60, 12, .5);
        gtk_range_set_value(GTK_RANGE(w->gain), node->gain_db); gtk_scale_set_digits(GTK_SCALE(w->gain), 1);
        gtk_scale_set_value_pos(GTK_SCALE(w->gain), GTK_POS_RIGHT); field(p, body, NULL, w->gain, "routing-block-gain", "value-changed");
    }
    switch (node->kind) {
    case RG_MIXER:
        w->channel = spin_field(p, body, "Mikser kanalı", 1, 32, node->channel, "routing-mixer-channel");
        break;
    case RG_INPUT: case RG_MICROPHONE: case RG_OUTPUT:
        w->device = gtk_combo_box_text_new();
        field(p, body, node->kind == RG_OUTPUT ? "Çıkış aygıtı" : node->kind == RG_MICROPHONE ? "Mikrofon" : "Giriş aygıtı",
            w->device, "routing-block-device", NULL);
        device_choices(p); g_signal_connect(w->device, "changed", G_CALLBACK(panel_changed), p);
        if (node->kind == RG_MICROPHONE) {
            w->push_to_talk = gtk_check_button_new_with_label("Bas-konuş: yalnız mikrofon açıkken geçir");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->push_to_talk), node->push_to_talk);
            field(p, body, NULL, w->push_to_talk, "routing-microphone-ptt", "toggled");
        }
        break;
    case RG_VIRTUAL: {
        w->virtual_info = caption(body, ""); named(w->virtual_info, "routing-virtual-info");
        w->as_microphone = gtk_check_button_new_with_label("Diğer uygulamalara mikrofon olarak da sun");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->as_microphone), node->as_microphone);
        field(p, body, NULL, w->as_microphone, "routing-virtual-microphone", "toggled");
        section(body, "Bir uygulamanın sesini buraya yönlendir");
        w->applications = gtk_combo_box_text_new(); named(w->applications, "routing-virtual-applications"); pack(body, w->applications, FALSE);
        application_choices(p);
        w->move = button(p, body, "Buraya yönlendir", "routing-virtual-move", G_CALLBACK(move_application), NULL);
        break;
    }
    case RG_MAIN:
        break;
    case RG_RECORDER: {
        w->record_state = radios_ui_label("HAZIR", "routing-record-state"); named(w->record_state, "routing-recorder-state");
        gtk_style_context_add_class(gtk_widget_get_style_context(w->record_state), "routing-record-state");
        gtk_label_set_xalign(GTK_LABEL(w->record_state), 0); pack(body, w->record_state, FALSE);
        w->record_time = caption(body, "00:00:00"); named(w->record_time, "routing-recorder-time");
        w->record_size = caption(body, "0.00 MB · Parça 1"); named(w->record_size, "routing-recorder-size");
        GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
        w->record_start = button(p, actions, "Kaydı başlat", "routing-record-start", G_CALLBACK(record_start), "routing-primary");
        w->record_stop = button(p, actions, "Durdur", "routing-record-stop", G_CALLBACK(record_stop), "routing-danger");
        pack(body, actions, FALSE);
        actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
        w->record_pause = button(p, actions, "Duraklat", "routing-record-pause", G_CALLBACK(record_pause), NULL);
        w->record_split = button(p, actions, "Böl", "routing-record-split", G_CALLBACK(record_split), NULL);
        pack(body, actions, FALSE);
        w->record_file = caption(body, "Henüz kayıt yok"); named(w->record_file, "routing-current-file");
        gtk_label_set_line_wrap(GTK_LABEL(w->record_file), FALSE); gtk_label_set_ellipsize(GTK_LABEL(w->record_file), PANGO_ELLIPSIZE_MIDDLE);
        section(body, "Kayıt klasörü");
        GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
        w->folder = gtk_entry_new(); gtk_entry_set_text(GTK_ENTRY(w->folder), node->folder);
        gtk_entry_set_placeholder_text(GTK_ENTRY(w->folder), p->ctx->config->recording_root ? p->ctx->config->recording_root : "Tam klasör yolu");
        named(w->folder, "routing-record-folder"); g_signal_connect(w->folder, "changed", G_CALLBACK(panel_changed), p); pack(row, w->folder, TRUE);
        w->choose = button(p, row, "Seç…", "routing-choose-folder", G_CALLBACK(choose_folder), NULL); pack(body, row, FALSE);
        w->prefix = text_field(p, body, "Dosya adı öneki", node->prefix, 80, "routing-record-prefix");
        w->split = spin_field(p, body, "Otomatik böl · dakika (0 kapalı)", 0, 1440, node->split_minutes, "routing-record-split-minutes");
        break;
    }
    case RG_STREAM: {
        GtkWidget *state_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
        w->stream_light = radios_ui_label("●", "routing-light"); named(w->stream_light, "routing-stream-light");
        gtk_style_context_add_class(gtk_widget_get_style_context(w->stream_light), "routing-light-off");
        gtk_widget_set_valign(w->stream_light, GTK_ALIGN_START); gtk_widget_set_tooltip_text(w->stream_light, "Kapalı");
        lights_style(w->stream_light); pack(state_row, w->stream_light, FALSE);
        w->stream_state = radios_ui_label("KAPALI", "routing-record-state"); named(w->stream_state, "routing-stream-state");
        gtk_style_context_add_class(gtk_widget_get_style_context(w->stream_state), "routing-record-state");
        gtk_label_set_xalign(GTK_LABEL(w->stream_state), 0); gtk_label_set_line_wrap(GTK_LABEL(w->stream_state), TRUE); pack(state_row, w->stream_state, TRUE);
        pack(body, state_row, FALSE); w->light_state = -1;
        GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
        w->stream_start = button(p, actions, "Yayını başlat", "routing-stream-start", G_CALLBACK(stream_start), "routing-primary");
        w->stream_stop = button(p, actions, "Yayını durdur", "routing-stream-stop", G_CALLBACK(stream_stop), "routing-danger");
        pack(body, actions, FALSE);
        w->protocol = gtk_combo_box_text_new();
        const gchar *protocols[] = {"Icecast 2", "SHOUTcast 1 (eski)", "SHOUTcast 2"};
        for (guint i = 0; i < G_N_ELEMENTS(protocols); ++i) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w->protocol), protocols[i]);
        gtk_combo_box_set_active(GTK_COMBO_BOX(w->protocol), node->protocol);
        field(p, body, "Sunucu türü", w->protocol, "routing-stream-protocol", "changed");
        w->host = text_field(p, body, "Sunucu adresi", node->host, 255, "routing-stream-host");
        gtk_entry_set_placeholder_text(GTK_ENTRY(w->host), "ornek.radyo.net ya da 192.168.1.10");
        w->port = spin_field(p, body, "Port", 1, 65535, node->port, "routing-stream-port");
        w->mount = text_field(p, body, "Bağlama noktası (Icecast)", node->mount, 255, "routing-stream-mount");
        w->stream_id = spin_field(p, body, "Akış numarası (SHOUTcast 2)", 1, 999, node->stream_id, "routing-stream-id");
        w->username = text_field(p, body, "Kullanıcı adı", node->username, 128, "routing-stream-user");
        section(body, "Yayın şifresi (Stream Password)");
        w->password = gtk_entry_new(); named(w->password, "routing-stream-password"); gtk_entry_set_visibility(GTK_ENTRY(w->password), FALSE);
        gtk_entry_set_input_purpose(GTK_ENTRY(w->password), GTK_INPUT_PURPOSE_PASSWORD);
        gtk_entry_set_max_length(GTK_ENTRY(w->password), 1024);
        Config *config = current_config(p);
        const gchar *password = node->password_saved && node->password[0] ? node->password :
            config ? g_hash_table_lookup(config->passwords, GUINT_TO_POINTER(id)) : NULL;
        if (password) gtk_entry_set_text(GTK_ENTRY(w->password), password);
        gtk_entry_set_placeholder_text(GTK_ENTRY(w->password), "Yayın şifresi");
        gtk_widget_set_tooltip_text(w->password, "Şifreyi kaydet işaretli değilse şifre yalnız bu oturumda tutulur; uygulama kapanınca silinir.");
        g_signal_connect(w->password, "changed", G_CALLBACK(panel_changed), p);
        pack(body, w->password, FALSE);
        GtkWidget *password_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 12, NULL);
        w->password_show = gtk_check_button_new_with_label("Şifreyi göster"); named(w->password_show, "routing-stream-password-show");
        g_signal_connect(w->password_show, "toggled", G_CALLBACK(password_visibility), p); pack(password_row, w->password_show, FALSE);
        w->password_save = gtk_check_button_new_with_label("Şifreyi kaydet"); named(w->password_save, "routing-stream-password-save");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->password_save), node->password_saved);
        gtk_widget_set_tooltip_text(w->password_save, "İşaretliyse şifre bu blokla veritabanına kaydedilir; yeniden yükleme ve yeniden bağlanmada korunur.");
        g_signal_connect(w->password_save, "toggled", G_CALLBACK(password_save_toggled), p); pack(password_row, w->password_save, FALSE);
        pack(body, password_row, FALSE);
        w->bitrate = gtk_combo_box_text_new();
        const gchar *rates[] = {"64", "96", "128", "160", "192", "256", "320"};
        for (guint i = 0; i < G_N_ELEMENTS(rates); ++i) {
            gchar *label = g_strdup_printf("%s kbps", rates[i]); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w->bitrate), rates[i], label); g_free(label);
        }
        gchar rate[16]; g_snprintf(rate, sizeof rate, "%u", node->bitrate_kbps); gtk_combo_box_set_active_id(GTK_COMBO_BOX(w->bitrate), rate);
        field(p, body, "MP3 bit hızı", w->bitrate, "routing-stream-bitrate", "changed");
        w->station = text_field(p, body, "İstasyon adı", node->station, 160, "routing-stream-station");
        w->genre = text_field(p, body, "Tür", node->genre, 160, "routing-stream-genre");
        w->description = text_field(p, body, "Açıklama", node->description, 256, "routing-stream-description");
        w->website = text_field(p, body, "Web adresi", node->website, 256, "routing-stream-website");
        w->public_listing = gtk_check_button_new_with_label("Herkese açık dizinde listele");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w->public_listing), node->public_listing);
        field(p, body, NULL, w->public_listing, "routing-stream-public", "toggled");
        section(body, "Şarkı bilgisi");
        w->metadata_source = gtk_combo_box_text_new(); named(w->metadata_source, "routing-stream-metadata-source");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w->metadata_source), "manual", "Elle yaz");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w->metadata_source), "player", "Player'dan al");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w->metadata_source), "automation", "Otomasyondan al");
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(w->metadata_source), radios_routing_metadata_source_name(node->metadata_source));
        section(body, "Bilgi kaynağı"); pack(body, w->metadata_source, FALSE);
        g_signal_connect(w->metadata_source, "changed", G_CALLBACK(metadata_changed), p);
        w->metadata_player_label = section(body, "Player");
        w->metadata_player = gtk_combo_box_text_new(); named(w->metadata_player, "routing-stream-metadata-player");
        for (guint i = 0; i < 32; ++i) {
            gchar deck[16], label[24]; g_snprintf(deck, sizeof deck, "%u", i); g_snprintf(label, sizeof label, "Player %u", i + 1);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(w->metadata_player), deck, label);
        }
        gchar deck[16]; g_snprintf(deck, sizeof deck, "%u", node->metadata_player);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(w->metadata_player), deck); pack(body, w->metadata_player, FALSE);
        g_signal_connect(w->metadata_player, "changed", G_CALLBACK(metadata_changed), p);
        w->metadata_format_label = section(body, "Biçim");
        w->metadata_format = gtk_entry_new(); named(w->metadata_format, "routing-stream-metadata-format");
        gtk_entry_set_max_length(GTK_ENTRY(w->metadata_format), 255); gtk_entry_set_text(GTK_ENTRY(w->metadata_format), node->metadata_format);
        gtk_entry_set_placeholder_text(GTK_ENTRY(w->metadata_format), "{artist} - {title}"); pack(body, w->metadata_format, FALSE);
        gtk_widget_set_tooltip_text(w->metadata_format, "{artist}: sanatçı, {title}: şarkı adı. Parça değiştiğinde otomatik güncellenir.");
        g_signal_connect(w->metadata_format, "changed", G_CALLBACK(metadata_changed), p);
        gtk_widget_set_no_show_all(w->metadata_player_label, TRUE); gtk_widget_set_no_show_all(w->metadata_player, TRUE);
        gtk_widget_set_no_show_all(w->metadata_format_label, TRUE); gtk_widget_set_no_show_all(w->metadata_format, TRUE);
        w->metadata = gtk_entry_new(); named(w->metadata, "routing-stream-metadata"); gtk_entry_set_max_length(GTK_ENTRY(w->metadata), 1024);
        gtk_entry_set_text(GTK_ENTRY(w->metadata), node->metadata_source == RG_METADATA_MANUAL ? node->metadata_manual : "");
        gtk_entry_set_placeholder_text(GTK_ENTRY(w->metadata), "Sanatçı - Şarkı");
        g_signal_connect(w->metadata, "changed", G_CALLBACK(metadata_changed), p);
        g_signal_connect(w->metadata, "activate", G_CALLBACK(metadata_send), p); pack(body, w->metadata, FALSE);
        w->metadata_send = button(p, body, "Gönder", "routing-stream-metadata-send", G_CALLBACK(metadata_send), NULL);
        w->metadata_source_state = caption(body, ""); named(w->metadata_source_state, "routing-stream-metadata-source-state");
        stream_fields_update(w);
        break;
    }
    case RG_METER:
        w->meter = gtk_drawing_area_new(); gtk_widget_set_size_request(w->meter, 220, 170); named(w->meter, "routing-meter");
        atk_object_set_name(gtk_widget_get_accessible(w->meter), "Stereo tepe ve RMS seviye ölçer");
        g_signal_connect(w->meter, "draw", G_CALLBACK(meter_draw), p); pack(body, w->meter, FALSE);
        w->meter_label = caption(body, "Tepe −60 · RMS −60 dBFS"); named(w->meter_label, "routing-meter-value");
        break;
    default: break;
    }
    w->remove = button(p, body, "Bloğu kaldır", "routing-graph-delete-node", G_CALLBACK(remove_block), "routing-danger");
    w->inventory = p->inventory_hash;
    radios_routing_graph_ui_style(p->graph_ui, body); gtk_widget_show_all(body);
    p->syncing = FALSE;
    metadata_panel_update(p);
}

/* Live status */
static const RadioSPatchNodeSnapshot *node_snapshot(RoutingPage *p, guint id)
{
    for (guint i = 0; i < p->snapshot->node_count; ++i) if (p->snapshot->nodes[i].id == id) return &p->snapshot->nodes[i];
    return NULL;
}
/* Every stream is observed even when another block's panel is open. Only the
 * selected options/manual draft are persisted; live song text stays in memory. */
static gboolean metadata_rejected(const RadioSAudioOutputSnapshot *output)
{
    glong status = output->metadata_http_status;
    return status >= 400 && status < 500 && status != 408 && status != 429;
}
/* TRUE when automatic sending has given up on the server's current failed title. */
static gboolean metadata_exhausted(const MetadataDispatch *sent, const RadioSAudioOutputSnapshot *output)
{
    return sent && output->metadata_state == RADIOS_AUDIO_METADATA_FAILED &&
        sent->generation == output->generation && !g_strcmp0(sent->text, output->metadata_title) &&
        (sent->attempts >= METADATA_ATTEMPTS || metadata_rejected(output));
}
static void metadata_tick(RoutingPage *p)
{
    gint64 now = g_get_monotonic_time();
    if (now < p->metadata_poll_us) return;
    p->metadata_poll_us = now + 500 * G_TIME_SPAN_MILLISECOND;
    for (guint i = 0; i < p->graph.node_count; ++i) {
        const RadioSRoutingGraphNode *node = &p->graph.nodes[i];
        gpointer key = GUINT_TO_POINTER(node->id);
        const RadioSPatchNodeSnapshot *live = node_snapshot(p, node->id);
        if (node->kind != RG_STREAM || node->metadata_source == RG_METADATA_MANUAL ||
            !p->patchbay || !live || !live->streaming || live->output_state != RADIOS_AUDIO_OUTPUT_ACTIVE) {
            g_hash_table_remove(p->metadata_dispatch, key); continue;
        }
        RadioSStreamMetadataSnapshot song;
        if (!radios_stream_metadata_resolve(p->ctx, node->metadata_source,
                node->metadata_player, node->metadata_format, &song)) continue;
        RadioSAudioOutputSnapshot output;
        if (!radios_audio_patchbay_stream_metadata_status(p->patchbay, node->id, &output) ||
            output.state != RADIOS_AUDIO_OUTPUT_ACTIVE) continue;
        MetadataDispatch *sent = g_hash_table_lookup(p->metadata_dispatch, key);
        if (!sent) {
            sent = g_new0(MetadataDispatch, 1); g_hash_table_insert(p->metadata_dispatch, key, sent);
        }
        gboolean same = output.metadata_sequence && !g_strcmp0(output.metadata_title, song.text);
        if (same && (output.metadata_state == RADIOS_AUDIO_METADATA_PENDING ||
                     output.metadata_state == RADIOS_AUDIO_METADATA_SUCCESS)) {
            sent->candidate[0] = '\0'; continue;
        }
        if (sent->generation != output.generation) {
            /* A reconnected stream starts without a title: resend at once. */
            sent->generation = output.generation; sent->attempts = 0; sent->next_attempt_us = 0;
            sent->candidate[0] = '\0'; g_strlcpy(sent->text, song.text, sizeof sent->text);
        } else if (g_strcmp0(sent->text, song.text)) {
            /* A changed title must be seen on two polls in a row, so a source
             * that flickers between titles cannot flood the server. */
            if (g_strcmp0(sent->candidate, song.text)) {
                g_strlcpy(sent->candidate, song.text, sizeof sent->candidate); continue;
            }
            g_strlcpy(sent->text, song.text, sizeof sent->text); sent->attempts = 0; sent->next_attempt_us = 0;
        } else {
            sent->candidate[0] = '\0';
            if (metadata_exhausted(sent, &output) || now < sent->next_attempt_us) continue;
        }
        sent->candidate[0] = '\0';
        ++sent->attempts;
        sent->next_attempt_us = now + MIN(METADATA_RETRY_US << MIN(sent->attempts - 1, 4u), METADATA_RETRY_MAX_US);
        GError *error = NULL;
        radios_audio_patchbay_stream_metadata(p->patchbay, node->id, song.text, &error);
        g_clear_error(&error);
    }
}
static void metadata_panel_update(RoutingPage *p)
{
    Panel *w = &p->panel; RadioSRoutingGraphNode *node = panel_node(p);
    if (!node || !w->metadata_source) return;
    gboolean manual = node->metadata_source == RG_METADATA_MANUAL;
    gboolean player = node->metadata_source == RG_METADATA_PLAYER;
    gtk_widget_set_visible(w->metadata_player_label, player); gtk_widget_set_visible(w->metadata_player, player);
    gtk_widget_set_visible(w->metadata_format_label, !manual); gtk_widget_set_visible(w->metadata_format, !manual);
    gtk_editable_set_editable(GTK_EDITABLE(w->metadata), manual);
    const RadioSPatchNodeSnapshot *live = node_snapshot(p, node->id);
    gboolean active = live && live->streaming && live->output_state == RADIOS_AUDIO_OUTPUT_ACTIVE;
    RadioSStreamMetadataSnapshot song = {0};
    gboolean valid = manual ? gtk_entry_get_text(GTK_ENTRY(w->metadata))[0] != '\0' :
        radios_stream_metadata_resolve(p->ctx, node->metadata_source, node->metadata_player, node->metadata_format, &song);
    if (!manual) {
        gboolean syncing = p->syncing; p->syncing = TRUE;
        if (g_strcmp0(gtk_entry_get_text(GTK_ENTRY(w->metadata)), song.text)) gtk_entry_set_text(GTK_ENTRY(w->metadata), song.text);
        p->syncing = syncing;
    }
    valid &= strlen(gtk_entry_get_text(GTK_ENTRY(w->metadata))) <= RADIOS_STREAM_METADATA_TEXT_BYTES;
    gtk_widget_set_sensitive(w->metadata_send, active && valid);
    RadioSAudioOutputSnapshot output = {0};
    gboolean observed = active && p->patchbay && radios_audio_patchbay_stream_metadata_status(p->patchbay, node->id, &output);
    /* Report the server's answer for the text shown, manual or automatic. */
    gboolean result = observed && output.metadata_sequence &&
        !g_strcmp0(output.metadata_title, gtk_entry_get_text(GTK_ENTRY(w->metadata)));
    gchar *failed = NULL;
    if (result && output.metadata_state == RADIOS_AUDIO_METADATA_FAILED) {
        const MetadataDispatch *sent = g_hash_table_lookup(p->metadata_dispatch, GUINT_TO_POINTER(node->id));
        failed = g_strdup_printf("Şarkı bilgisi gönderilemedi%s%s%s", output.metadata_error[0] ? ": " : "",
            output.metadata_error, !manual && !metadata_exhausted(sent, &output) ? " · yeniden denenecek" : "");
    }
    const gchar *state = failed ? failed :
        result && output.metadata_state == RADIOS_AUDIO_METADATA_PENDING ? "Şarkı bilgisi sunucuya gönderiliyor…" :
        result && output.metadata_state == RADIOS_AUDIO_METADATA_SUCCESS ? "Şarkı bilgisi gönderildi." :
        manual ? "Metni yazıp Gönder'e basın." :
        !valid ? "Çalan parçanın şarkı ve sanatçı bilgisi bekleniyor." :
        !active ? "Yayın başladığında şarkı ve sanatçı otomatik gönderilecek." :
        "Şarkı ve sanatçı parça değiştikçe otomatik güncellenir.";
    gtk_label_set_text(GTK_LABEL(w->metadata_source_state), state);
    g_free(failed);
}
static gchar *duration(guint64 frames)
{
    guint64 seconds = frames / 48000;
    return g_strdup_printf("%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT, seconds / 3600, seconds / 60 % 60, seconds % 60);
}
static void node_detail(RoutingPage *p, const RadioSRoutingGraphNode *n, const RadioSPatchNodeSnapshot *s, gchar *out, gsize size)
{
    if (s && s->state == RADIOS_PATCH_FAILED && s->error[0]) { g_strlcpy(out, s->error, size); return; }
    switch (n->kind) {
    case RG_MIXER: g_snprintf(out, size, "Kanal %u", n->channel); break;
    case RG_INPUT: case RG_OUTPUT: {
        const gchar *label = n->device[0] ? device_label(p, n->device, n->kind == RG_INPUT) : NULL;
        g_strlcpy(out, label ? label : n->device[0] ? n->device : "Aygıt seçilmedi", size); break;
    }
    case RG_MICROPHONE: {
        const gchar *label = !g_strcmp0(n->device, RADIOS_ROUTING_TALK_MICROPHONE) ? "Konuşma mikrofonu" :
            n->device[0] ? device_label(p, n->device, TRUE) : "Mikrofon seçilmedi";
        g_snprintf(out, size, "%s%s", label ? label : n->device, n->push_to_talk ? " · bas-konuş" : ""); break;
    }
    case RG_VIRTUAL: g_snprintf(out, size, "RadioS · %s", radios_routing_graph_node_title(n)); break;
    case RG_MAIN: g_strlcpy(out, "Ana yayın karışımı", size); break;
    case RG_RECORDER:
        if (s && s->recording) {
            gchar *time = duration(s->total_frames);
            g_snprintf(out, size, "%s %s · parça %u", s->paused ? "Ⅱ DURAKLATILDI" : "● KAYIT", time, s->segment); g_free(time);
        } else g_strlcpy(out, "WAV · 48 kHz stereo", size);
        break;
    case RG_STREAM:
        if (s && s->streaming) {
            /* "YAYINDA" only once the server accepted the source; a retrying
             * stream says so instead of looking connected. */
            RadioSAudioOutputSnapshot output = {0};
            gboolean accepted = p->patchbay && radios_audio_patchbay_stream_metadata_status(p->patchbay, n->id, &output) &&
                output.state == RADIOS_AUDIO_OUTPUT_ACTIVE && output.response_confirmed;
            gboolean failed = s->output_state == RADIOS_AUDIO_OUTPUT_FAILED || s->output_state == RADIOS_AUDIO_OUTPUT_STOPPED;
            g_snprintf(out, size, "%s · %s:%u", failed ? "HATA · yeniden bağlanıyor" : accepted ? "● YAYINDA" : "BAĞLANIYOR",
                n->host, n->port);
        } else g_snprintf(out, size, "%s · %s", radios_routing_stream_protocol_name(n->protocol), n->host[0] ? n->host : "sunucu yok");
        break;
    case RG_METER:
        g_snprintf(out, size, "Tepe %.1f dBFS", s ? level_db(MAX(s->peak[0], s->peak[1])) : -60.0); break;
    default: out[0] = 0;
    }
}
static void panel_update(RoutingPage *p)
{
    Panel *w = &p->panel; RadioSRoutingGraphNode *node = panel_node(p);
    if (!node) return;
    const RadioSPatchNodeSnapshot *s = node_snapshot(p, node->id);
    if (w->inventory != p->inventory_hash) {
        w->inventory = p->inventory_hash; if (w->device) device_choices(p);
        if (w->applications) application_choices(p);
    }
    static const gchar *const states[] = {"Kapalı", "Hazırlanıyor…", "Etkin", "Hata"};
    gchar *state = g_strdup_printf("Durum: %s%s%s", states[s ? s->state : 0], s && s->error[0] ? " · " : "", s && s->error[0] ? s->error : "");
    gtk_label_set_text(GTK_LABEL(w->state), state); g_free(state);
    if (w->virtual_info) {
        gchar *text = s && s->sink[0] ? g_strdup_printf("Diğer uygulamalarda çıkış: “RadioS · %s” (%s)\nKayıt kaynağı: %s",
            radios_routing_graph_node_title(node), s->sink, s->source) : g_strdup("Sanal aygıt ses sunucusunda oluşturuluyor…");
        gtk_label_set_text(GTK_LABEL(w->virtual_info), text); g_free(text);
    }
    if (w->record_state) {
        gboolean recording = s && s->recording;
        gtk_label_set_text(GTK_LABEL(w->record_state), !recording ? (s && s->state == RADIOS_PATCH_FAILED ? "HATA" : "HAZIR") :
            s->paused ? "DURAKLATILDI" : "● KAYIT");
        gchar *time = duration(s ? s->total_frames : 0); gtk_label_set_text(GTK_LABEL(w->record_time), time); g_free(time);
        gchar *size = g_strdup_printf("%.2f MB · Parça %u", (s ? s->bytes : 0) / 1048576., MAX(1u, s ? s->segment : 1));
        gtk_label_set_text(GTK_LABEL(w->record_size), size); g_free(size);
        gtk_label_set_text(GTK_LABEL(w->record_file), s && s->path[0] ? s->path : "Henüz kayıt yok");
        gtk_widget_set_tooltip_text(w->record_file, s && s->path[0] ? s->path : NULL);
        gtk_widget_set_sensitive(w->record_start, p->patchbay && !recording && !p->authorizing);
        gtk_widget_set_sensitive(w->record_stop, recording); gtk_widget_set_sensitive(w->record_pause, recording);
        gtk_widget_set_sensitive(w->record_split, recording);
        gtk_button_set_label(GTK_BUTTON(w->record_pause), recording && s->paused ? "Devam et" : "Duraklat");
        gtk_widget_set_sensitive(w->folder, !recording); gtk_widget_set_sensitive(w->choose, !recording); gtk_widget_set_sensitive(w->prefix, !recording);
    }
    if (w->stream_state) {
        gboolean streaming = s && s->streaming; gchar *text;
        /* The worker snapshot says whether the server actually accepted the
         * source; a started encoder is still "connecting" until then. */
        RadioSAudioOutputSnapshot output = {0};
        gboolean observed = streaming && p->patchbay && radios_audio_patchbay_stream_metadata_status(p->patchbay, node->id, &output);
        gboolean accepted = observed && output.state == RADIOS_AUDIO_OUTPUT_ACTIVE && output.response_confirmed;
        gboolean failed = streaming && (s->state == RADIOS_PATCH_FAILED || s->output_state == RADIOS_AUDIO_OUTPUT_FAILED ||
            s->output_state == RADIOS_AUDIO_OUTPUT_STOPPED);
        gint light = !streaming ? 0 : failed ? 3 : accepted ? 2 : 1;
        if (!streaming) text = g_strdup(s && s->state == RADIOS_PATCH_FAILED && s->error[0] ? s->error : "KAPALI");
        else if (accepted && s->connected_us) {
            gchar *time = duration((guint64)((g_get_monotonic_time() - s->connected_us) / 1000000) * 48000);
            gchar *again = s->reconnects ? g_strdup_printf(" · %u kez yeniden bağlandı", s->reconnects) : g_strdup("");
            text = g_strdup_printf("● YAYINDA %s · HTTP %ld · %.2f MB gönderildi%s", time, output.http_status, s->sent_bytes / 1048576., again);
            g_free(time); g_free(again);
        } else if (failed)
            text = g_strdup_printf("HATA · %s\nYeniden bağlanıyor (%u. deneme, 5 saniyede bir); sunucu bilgilerini veya şifreyi düzeltebilirsiniz.",
                s->error[0] ? s->error : output.error, s->reconnects + 1);
        else text = g_strdup_printf("BAĞLANIYOR… · %s%s%s", observed && output.state == RADIOS_AUDIO_OUTPUT_ACTIVE ?
            "sunucu yanıtı bekleniyor" : "sunucuya bağlanılıyor", s->error[0] ? " · " : "", s->error);
        gtk_label_set_text(GTK_LABEL(w->stream_state), text);
        if (w->stream_light) {
            static const gchar *const classes[] = {"routing-light-off", "routing-light-connecting", "routing-light-on", "routing-light-error"};
            GtkStyleContext *style = gtk_widget_get_style_context(w->stream_light);
            if (light != w->light_state) {
                for (guint i = 0; i < G_N_ELEMENTS(classes); ++i) gtk_style_context_remove_class(style, classes[i]);
                gtk_style_context_add_class(style, classes[light]); w->light_state = light;
                if (w->light_dim) { gtk_style_context_remove_class(style, "routing-light-dim"); w->light_dim = FALSE; }
            }
            gchar *tooltip = gtk_widget_get_tooltip_text(w->stream_light);
            if (g_strcmp0(tooltip, text)) gtk_widget_set_tooltip_text(w->stream_light, text);
            g_free(tooltip);
            /* Connecting and error blink at 1 Hz on the monotonic clock. */
            gboolean dim = (light == 1 || light == 3) && (g_get_monotonic_time() / (G_TIME_SPAN_SECOND / 2)) % 2 == 1;
            if (dim != w->light_dim) {
                if (dim) gtk_style_context_add_class(style, "routing-light-dim"); else gtk_style_context_remove_class(style, "routing-light-dim");
                w->light_dim = dim;
            }
        }
        g_free(text);
        gtk_widget_set_sensitive(w->stream_start, p->patchbay && !streaming && !p->authorizing);
        gtk_widget_set_sensitive(w->stream_stop, streaming || (p->authorizing && p->start_action == ACTION_STREAM && p->start_node == w->id));
        metadata_panel_update(p);
        GtkWidget *settings[] = {w->protocol, w->host, w->port, w->bitrate};
        for (guint i = 0; i < G_N_ELEMENTS(settings); ++i) gtk_widget_set_sensitive(settings[i], TRUE);
        stream_fields_update(w);
    }
    if (w->meter) {
        for (guint c = 0; c < 2; ++c) { p->meter_peak[c] = s ? s->peak[c] : 0; p->meter_rms[c] = s ? s->rms[c] : 0; }
        gchar *text = g_strdup_printf("Tepe %.1f · RMS %.1f dBFS", level_db(MAX(p->meter_peak[0], p->meter_peak[1])),
            level_db(MAX(p->meter_rms[0], p->meter_rms[1])));
        gtk_label_set_text(GTK_LABEL(w->meter_label), text); g_free(text); gtk_widget_queue_draw(w->meter);
    }
}
static gboolean tick(gpointer data)
{
    RoutingPage *p = data;
    if (p->disposed) return G_SOURCE_REMOVE;
    password_identity_sync(p);
    if (p->patchbay && !p->engine_bound) {
        RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(p->ctx);
        if (engine) { radios_audio_patchbay_set_engine(p->patchbay, engine); radios_audio_engine_unref(engine); p->engine_bound = TRUE; }
    }
    if (p->patchbay) radios_audio_patchbay_snapshot(p->patchbay, p->snapshot);
    metadata_tick(p);
    guint hash = inventory_hash(p->snapshot); if (hash != p->inventory_hash) p->inventory_hash = hash;
    RadioSRoutingNodeStatus status[RADIOS_ROUTING_GRAPH_MAX_NODES]; guint count = 0, active = 0, failed = 0;
    for (guint i = 0; i < p->graph.node_count; ++i) {
        const RadioSRoutingGraphNode *n = &p->graph.nodes[i]; const RadioSPatchNodeSnapshot *s = node_snapshot(p, n->id);
        RadioSRoutingNodeStatus *out = &status[count++]; memset(out, 0, sizeof *out); out->id = n->id;
        out->state = s ? (RadioSRoutingNodeState)s->state : RG_STATUS_IDLE;
        for (guint c = 0; c < 2; ++c) out->level[c] = s ? (gfloat)((level_db(s->peak[c]) + 60) / 60) : 0;
        node_detail(p, n, s, out->detail, sizeof out->detail);
        active += out->state == RG_STATUS_ACTIVE; failed += out->state == RG_STATUS_FAILED;
    }
    radios_routing_graph_ui_status(p->graph_ui, status, count);
    JsonObject *context = radios_data_context(p->ctx);
    gtk_button_set_label(GTK_BUTTON(p->run), p->running ? "■ Yönlendirmeyi durdur" : "▶ Yönlendirmeyi başlat");
    gtk_widget_set_sensitive(p->run, p->patchbay && !p->authorizing && (p->running || allowed(context, "operate")));
    gchar *state = !p->patchbay ? g_strdup("ÇEVRİMDIŞI") : p->authorizing ? g_strdup("YETKİ DENETİMİ") :
        !p->snapshot->connected ? g_strdup("SES SUNUCUSU BEKLENİYOR") :
        !p->running ? g_strdup("KAPALI") : p->snapshot->busy ? g_strdup("HAZIRLANIYOR") :
        failed ? g_strdup_printf("ÇALIŞIYOR · %u hata", failed) : g_strdup_printf("ÇALIŞIYOR · %u blok etkin", active);
    gtk_label_set_text(GTK_LABEL(p->run_state), state); g_free(state);
    gtk_widget_set_sensitive(p->save, allowed(context, "manage") && p->document && radios_data_document_ready(p->document));
    gtk_widget_set_sensitive(p->save_as, gtk_widget_get_sensitive(p->save));
    gtk_widget_set_sensitive(p->remove_config, gtk_widget_get_sensitive(p->save) && p->items->len > 1);
    panel_update(p);
    if (!p->notice_active && !p->authorizing) {
        const gchar *message = !p->patchbay ? "Ses yönlendirme için uygulamayı veritabanıyla açın." :
            p->snapshot->error[0] ? p->snapshot->error :
            !p->running ? "" : "Yönlendirme çalışıyor.";
        gtk_label_set_text(GTK_LABEL(p->status), message);
    }
    return G_SOURCE_CONTINUE;
}
static void destroyed(GtkWidget *widget, gpointer data)
{
    RoutingPage *p = data; p->disposed = TRUE; ++p->intent;
    if (p->ctx && p->ctx->parent && g_object_get_data(G_OBJECT(p->ctx->parent), "radios-routing-page") == p)
        g_object_set_data(G_OBJECT(p->ctx->parent), "radios-routing-page", NULL);
    if (p->timer) { g_source_remove(p->timer); p->timer = 0; }
    if (p->autosave) { g_source_remove(p->autosave); p->autosave = 0; }
    g_object_set_data(G_OBJECT(widget), "routing-patchbay", NULL);
    if (p->patchbay) { radios_audio_patchbay_unref(p->patchbay); p->patchbay = NULL; }
}
static void state_free(gpointer data)
{
    RoutingPage *p = data; g_free(p->snapshot); if (p->items) g_ptr_array_unref(p->items);
    if (p->password_client) radios_data_client_unref(p->password_client);
    if (p->metadata_dispatch) g_hash_table_unref(p->metadata_dispatch);
    g_free(p);
}
static GtkWidget *find_widget(GtkWidget *widget, const gchar *name)
{
    if (g_strcmp0(gtk_widget_get_name(widget), name) == 0) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find_widget(item->data, name);
    g_list_free(children); return found;
}
gboolean radios_ui_routing_output_snapshot(RadioSViewContext *ctx, RadioSRoutingOutputSnapshot *out)
{
    if (!out) return FALSE;
    memset(out, 0, sizeof *out);
    /* Header strips ask several times a second: use the page registered on
     * the window instead of walking the whole widget tree each time. */
    RoutingPage *p = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-routing-page") : NULL;
    if (!p) {
        GtkWidget *page = ctx && ctx->parent ? find_widget(GTK_WIDGET(ctx->parent), "routing-workspace") : NULL;
        p = page ? g_object_get_data(G_OBJECT(page), "routing-page-state") : NULL;
    }
    if (!p || p->disposed || !p->patchbay) return FALSE;
    out->available = TRUE; out->running = p->running;
    /* tick() owns the meter-consuming patchbay snapshot. Inspect its stable
     * node identities, then obtain fresh nondestructive stream worker state. */
    for (guint i = 0; p->snapshot && i < p->snapshot->node_count; ++i) {
        RadioSPatchNodeSnapshot *node = &p->snapshot->nodes[i];
        if (node->kind != RG_STREAM) continue;
        ++out->streams; RadioSAudioOutputSnapshot stream = {0};
        gboolean observed = radios_audio_patchbay_stream_metadata_status(p->patchbay, node->id, &stream);
        if (node->streaming) out->reconnects += node->reconnects;
        if (observed && stream.state == RADIOS_AUDIO_OUTPUT_ACTIVE && stream.response_confirmed) ++out->active;
        else if ((observed && (stream.state == RADIOS_AUDIO_OUTPUT_STARTING || stream.state == RADIOS_AUDIO_OUTPUT_ACTIVE)) ||
                 node->state == RADIOS_PATCH_STARTING) ++out->pending;
        if ((observed && (stream.state == RADIOS_AUDIO_OUTPUT_FAILED || (node->streaming && stream.state == RADIOS_AUDIO_OUTPUT_STOPPED))) ||
            node->state == RADIOS_PATCH_FAILED) {
            ++out->failed;
            if (!out->error[0]) g_strlcpy(out->error, observed && stream.error[0] ? stream.error : node->error, sizeof out->error);
        }
        if (observed && stream.metadata_state == RADIOS_AUDIO_METADATA_FAILED) {
            ++out->metadata_failed;
            if (!out->metadata_error[0]) g_strlcpy(out->metadata_error, stream.metadata_error, sizeof out->metadata_error);
        }
        if (observed && !out->target[0] && stream.target[0]) g_strlcpy(out->target, stream.target, sizeof out->target);
    }
    return TRUE;
}
void radios_ui_routing_open(RadioSViewContext *ctx, const gchar *provider_name)
{
    if (!ctx || !ctx->parent) return;
    GtkWidget *root = GTK_WIDGET(ctx->parent);
    GtkWidget *tab = find_widget(root, "profile-routing-tab");
    if (GTK_IS_BUTTON(tab)) gtk_button_clicked(GTK_BUTTON(tab));
    GtkWidget *page = find_widget(root, "routing-workspace");
    RoutingPage *p = page ? g_object_get_data(G_OBJECT(page), "routing-page-state") : NULL;
    if (!p || p->disposed) return;
    if (p->patchbay) radios_audio_patchbay_refresh(p->patchbay);
    guint id = 0;
    for (guint i = 0; i < p->graph.node_count && !id; ++i) if (p->graph.nodes[i].kind == RG_VIRTUAL) id = p->graph.nodes[i].id;
    const gchar *provider = provider_name && *provider_name ? provider_name : "Resmî müzik";
    if (!id && (id = radios_routing_graph_ui_add(p->graph_ui, RG_VIRTUAL))) {
        gchar *label = g_strdup_printf("%s sesi", provider); radios_routing_graph_rename(&p->graph, id, label, NULL); g_free(label);
        radios_routing_graph_ui_refresh(p->graph_ui); graph_changed(p);
    }
    radios_routing_graph_ui_select(p->graph_ui, id);
    gchar *message = g_strdup_printf("%s oynatıcısında parçayı çalın; “Buraya yönlendir” ile sesini sanal arayüze alın ve arayüzün "
        "çıkışını bir Mikser girişi ya da MAIN yayını bloğuna bağlayın.", provider);
    notice(p, message); g_free(message);
}

GtkWidget *radios_routing_new(RadioSViewContext *ctx)
{
    RoutingPage *p = g_new0(RoutingPage, 1); p->ctx = ctx;
    p->metadata_dispatch = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    p->items = g_ptr_array_new_with_free_func(config_free); g_ptr_array_add(p->items, config_new("Varsayılan", NULL));
    p->graph = *((Config *)g_ptr_array_index(p->items, 0))->graph;
    p->snapshot = g_new0(RadioSAudioPatchbaySnapshot, 1);
    p->page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "routing-workspace"); named(p->page, "routing-workspace");
    g_object_set_data_full(G_OBJECT(p->page), "routing-page-state", p, state_free);
    g_signal_connect(p->page, "destroy", G_CALLBACK(destroyed), p);
    if (ctx->parent) g_object_set_data(G_OBJECT(ctx->parent), "radios-routing-page", p);
    GtkWidget *toolbar = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "routing-toolbar");
    GtkWidget *titles = radios_ui_box(GTK_ORIENTATION_VERTICAL, 5, NULL);
    pack(titles, radios_ui_label("Ses yönlendirme", "routing-title"), FALSE);
    pack(toolbar, titles, TRUE);
    GtkWidget *configs = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, NULL);
    pack(configs, radios_ui_label("Yapılandırma", "routing-subtitle"), FALSE);
    p->configs = gtk_combo_box_text_new(); named(p->configs, "routing-config-select"); gtk_widget_set_size_request(p->configs, 200, -1);
    g_signal_connect(p->configs, "changed", G_CALLBACK(config_selected), p); pack(configs, p->configs, FALSE);
    pack(toolbar, configs, FALSE);
    p->save = radios_ui_button("Kaydet"); named(p->save, "routing-settings-save");
    gtk_widget_set_tooltip_text(p->save, "Tüm yönlendirme yapılandırmalarını veritabanına kaydet");
    g_signal_connect(p->save, "clicked", G_CALLBACK(save_settings), p); pack(toolbar, p->save, FALSE);
    p->save_as = radios_ui_button("Farklı kaydet…"); named(p->save_as, "routing-config-save-as");
    g_signal_connect(p->save_as, "clicked", G_CALLBACK(save_as), p); pack(toolbar, p->save_as, FALSE);
    p->remove_config = radios_ui_button("Sil"); named(p->remove_config, "routing-config-remove");
    gtk_widget_set_tooltip_text(p->remove_config, "Seçili yapılandırmayı sil");
    g_signal_connect(p->remove_config, "clicked", G_CALLBACK(remove_config), p); pack(toolbar, p->remove_config, FALSE);
    p->refresh = radios_ui_button("Yenile"); named(p->refresh, "routing-refresh");
    gtk_widget_set_tooltip_text(p->refresh, "Aygıtları yeniden tara ve yapılandırmaları veritabanından yükle");
    g_signal_connect(p->refresh, "clicked", G_CALLBACK(refresh), p); pack(toolbar, p->refresh, FALSE);
    GtkWidget *run = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, NULL);
    p->run = radios_ui_button("▶ Yönlendirmeyi başlat"); named(p->run, "routing-run");
    g_signal_connect(p->run, "clicked", G_CALLBACK(run_clicked), p); pack(run, p->run, FALSE);
    p->run_state = radios_ui_label("KAPALI", "routing-subtitle"); named(p->run_state, "routing-run-state"); pack(run, p->run_state, FALSE);
    pack(toolbar, run, FALSE);
    pack(p->page, toolbar, FALSE);
    p->graph_ui = radios_routing_graph_ui_new(&p->graph, ctx->config->assets_root, graph_changed, selection_changed, p);
    pack(p->page, radios_routing_graph_ui_widget(p->graph_ui), TRUE);
    p->status = radios_ui_label("", "routing-status"); named(p->status, "routing-status");
    gtk_label_set_line_wrap(GTK_LABEL(p->status), TRUE); gtk_label_set_xalign(GTK_LABEL(p->status), 0); pack(p->page, p->status, FALSE);
    fill_configs(p);
    if (ctx->data) {
        p->patchbay = radios_audio_patchbay_new(NULL);
        radios_audio_patchbay_set_recording_root(p->patchbay, ctx->config->recording_root);
        g_object_set_data(G_OBJECT(p->page), "routing-patchbay", p->patchbay);
        apply_runtime(p);
        p->document = radios_data_document_new(ctx, p->page, "audio_routing", "Ses yönlendirme", settings_loaded, p);
    }
    tick(p); p->timer = g_timeout_add(100, tick, p); return p->page;
}
