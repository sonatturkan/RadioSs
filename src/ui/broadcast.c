#include "radios/broadcast_header.h"
#include "radios/views.h"
#include "radios/widgets.h"
#include "radios/catalogue_playlist.h"
#include "radios/automation_calendar.h"
#include "radios/editors.h"
#include "radios/data.h"
#include "radios/media_ui.h"
#include "radios/audio_ui.h"
#include "radios/playout_ui.h"
#include "radios/copyright_policy.h"
#include "radios/programme_lifecycle.h"
#include "radios/header_navigation.h"
#include "radios/programme_model.h"
#include "radios/programme_queue.h"
#include "radios/local_event_ui.h"
#include "radios/local_event_prepare.h"
#include "radios/event_playout.h"
#include "radios/live_control.h"
#include "radios/player_loader.h"
#include "radios/player_rotation.h"
#include "radios/station_time.h"
#include "radios/player_transport.h"
#include "radios/player_waveform.h"
#include "radios/media.h"
#include "radios/player_binding.h"
#include "radios/player_workspace.h"
#include "radios/player_tree_ui.h"
#include "radios/automation_workspace_ui.h"
#include "radios/player_scope_execution.h"
#include "radios/schema_track_picker.h"
#include "radios/platform.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Where catalogue content goes (design: "Seçili içeriğin sonrasına ekle",
 * "Seçili içeriği değiştir", "Event sonuna ekle"). */
typedef enum { QUEUE_INSERT, QUEUE_REPLACE, QUEUE_APPEND } QueueMode;

typedef struct BroadcastView BroadcastView;
/* Kılavuz 2026-10-18 §06/§08: per-programme view counts and refresh interval. */
typedef struct { guint event_fetch, event_show, content_fetch, content_show, refresh_seconds; } ProgrammeView;
/* Accepted-order index of the shown programme for O(1) tick lookups. */
typedef struct { gchar *id, *title, *last_leaf; gint64 duration_ms; JsonObject *block; } FlowBlock;
typedef struct { FlowBlock *block; gchar *title; gint64 duration_ms; gboolean last; } FlowLeaf;
typedef struct { gchar *id; JsonObject *block, *playback; gboolean known, active, held, label_known; RadioSLiveLabel label; } LiveBlock;
typedef struct { gchar *end_rule, *previous, *next; gint64 duration_ms, bound_utc_us; } LiveStep;
typedef struct {
    BroadcastView *view;
    RadioSViewContext context;
    RadioSDocument *document;
    JsonNode *config, *run_config;
    gchar *selected;
    guint slot, transition;
    guint64 hint_epoch;
    guint hint_count;
    gdouble overlap;
    gboolean initialized;
} PlayerQueue;

struct BroadcastView {
    RadioSViewContext *ctx;
    GtkWidget *players;
    GtkWidget *player_count;
    GtkWidget *player_tree, *player_selection_title, *players_empty;
    GtkWidget *queue_stack, *scope_panel, *editor_caption;
    RadioSAutomationScope live_scope;
    gboolean live_scope_set, scope_mirroring;
    guint topology_timer;
    gchar *player_topology_key;
    GtkWidget *query;
    GtkWidget *right_stack, *catalog_content, *playlist_panel;
    guint player_sequence;
    GtkWidget *page, *queue_table, *catalog_table, *catalog_count, *catalog_field, *catalog_edit;
    GtkWidget *catalog_insert, *catalog_replace, *catalog_status, *catalog_create_playlist;
    GtkWidget *catalog_title, *catalog_details[4], *catalog_music, *queue_count, *queue_duration, *placement;
    gboolean catalog_unified, catalog_pending;
    GCancellable *catalog_cancel;
    JsonNode *catalog_result;
    gchar *catalog_error;
    gboolean catalog_switching;
    QueueMode queue_mode;
    JsonNode *catalog_records;
    JsonNode *programme_library_records;
    guint catalogue_intent, programme_library_generation;
    gboolean catalogue_busy;
    const gchar *catalog_kind;
    guint catalog_generation, catalog_selection_generation;
    RadioSDocument *programme_document, *channel_document;
    JsonNode *programme_config;
    gchar *selected_event;
    gboolean loading, rendering;
    GtkWidget *transition_mode, *transition_overlap, *playout_status, *schedule;
    GtkWidget *policy_status, *policy_gear;
    RadioSDocument *policy_document;
    RadioSDataClient *policy_client;
    JsonNode *policy;
    gboolean policy_active, policy_dirty;
    RadioSPlayerWeekly *general_automation;
    GtkWidget *general_status, *general_enable;
    JsonNode *manual_general_config;
    gboolean general_projection, general_enabled;
    GtkWidget *transport[5];
    GtkWidget *programme_title, *programme_date, *timing_title, *timing_target, *timing_policy;
    guint playout_timer;
    guint history_timer, history_count;
    GtkWidget *queue_player;
    PlayerQueue queues[33];
    RadioSViewContext lifecycle_context;
    guint queue_slot, queue_timer;
    gboolean queue_switching;
    GtkWidget *history_dialog, *history_table;
    gchar *history_latest;
    GCancellable *local_event_cancel;
    GWeakRef local_event_dialog;
    gchar *local_event_target, *local_event_target_document_id;
    RadioSDocument *local_event_target_document;
    JsonNode *local_event_target_baseline;
    guint local_event_target_slot;
    gint64 local_event_target_revision;
    GHashTable *event_overrides; /* current general programme only; no template edits */
    GHashTable *live_schema_jobs; /* event id -> GCancellable of its live schema preparation */
    /* Kılavuz 2026-10-18 · yayın sırası görünümü */
    ProgrammeView view_settings;
    guint view_refresh_timer, view_refresh_seconds;
    GHashTable *flow_leaves, *flow_blocks, *live_steps;
    GPtrArray *live_blocks;
    guint64 flow_index_serial, config_serial;
    gchar *transition_source, *transition_target, *ending_event, *talk_event, *event_run_origin;
    gchar *live_step_on_air, *plain_talk; /* live step the flow waits at; event whose R holds the studio microphone */
    GHashTable *run_removed; /* contents/events × took out of the running general flow (U7) */
    GHashTable *live_steps_done; /* live steps the outer order completed in this run */
    gint64 live_stop_retry_us; /* last stop of a completed step's returning schema run */
    gchar *overlay_leaf, *overlay_title, *overlay_id;
    RadioSPlayoutContentState overlay_state;
    gboolean feed_dirty; /* rows rebuilt: re-send every row state on the next tick */
    gboolean disposed;
};

static void programme_render(BroadcastView *view);
static void programme_save(GtkButton *button_widget, gpointer data);
static void broadcast_refresh(RadioSViewContext *ctx, GtkWidget *owner);
static void programme_transport(GtkButton *button, gpointer data);
static gboolean programme_playout_tick(gpointer data);
static void catalog_actions_update(BroadcastView *view);
static void playlist_refresh(BroadcastView *view);
static gboolean playlist_drop(BroadcastView *view, const gchar *payload, RadioSProgrammePlacement placement);
static void playlist_open(BroadcastView *view, const gchar *id);
static void queue_selector_refresh(BroadcastView *view);
static gboolean players_reconcile(gpointer data);
static void player_card_add(BroadcastView *view, guint deck);
static void player_scope_selected(const RadioSAutomationScope *scope, gpointer data);
static void queue_scope_selected(const RadioSAutomationScope *scope, gpointer data);
static void scope_edit_requested(const RadioSAutomationScope *scope, gpointer data);
static JsonObject *catalog_object(JsonObject *object, const gchar *key);
static void policy_submit(BroadcastView *view);
static void policy_changed(BroadcastView *view);
static void policy_apply(RadioSViewContext *, GtkWidget *, JsonObject *, gpointer);
static JsonNode *queue_empty(guint slot);
static gboolean general_mode(BroadcastView *view);
static gboolean general_controls(BroadcastView *view);
static void general_queue_apply(JsonObject *programme, gboolean running, gpointer data);
static void general_status_update(BroadcastView *view);
static void local_event_open(GtkButton *control, gpointer value);
static void local_event_open_for_block(GtkButton *control, gpointer value);
static JsonObject *programme_event_playback_value(GtkWidget *dialog);
static gboolean programme_event_start(BroadcastView *view, const gchar *id, JsonObject *playback, GError **error);
static JsonObject *programme_event_configuration(BroadcastView *view, const gchar *id, GError **error);
static void programme_view_load(BroadcastView *view);
static void programme_view_apply(BroadcastView *view);
static void flow_index_clear(BroadcastView *view);
static void live_step_free(gpointer value);
static void live_step_record(BroadcastView *view, JsonObject *previous, JsonObject *hint, JsonObject *next);
static void live_step_finished(BroadcastView *view, const gchar *event_id);
static void live_event_run_stop(BroadcastView *view, const gchar *event_id);
static void add_class(GtkWidget *widget, const gchar *name);
static void pack(GtkWidget *box, GtkWidget *child, gboolean expand);

static RadioSViewContext *programme_context(BroadcastView *view)
{ return view->queue_slot ? &view->queues[view->queue_slot].context : view->ctx; }

static void queue_binding(BroadcastView *view, JsonObject *config)
{
    if (view->queue_slot) {
        json_object_set_int_member(config, "queue_player", view->queue_slot);
        json_object_set_int_member(config, "queue_transition", MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(view->transition_mode))));
        json_object_set_double_member(config, "queue_overlap", 0);
    } else {
        json_object_remove_member(config, "queue_player");
        json_object_remove_member(config, "queue_transition");
        json_object_remove_member(config, "queue_overlap");
    }
}

static JsonNode *programme_copy(JsonNode *node)
{
    JsonGenerator *generator = json_generator_new(); json_generator_set_root(generator, node);
    gchar *text = json_generator_to_data(generator, NULL); JsonParser *parser = json_parser_new();
    JsonNode *copy = json_parser_load_from_data(parser, text, -1, NULL) ? json_node_copy(json_parser_get_root(parser)) : NULL;
    g_free(text); g_object_unref(generator); g_object_unref(parser); return copy;
}
static void programme_media_picked(RadioSViewContext *ctx, GtkWidget *dialog, JsonObject *detail, gpointer data)
{
    (void)data; JsonObject *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version");
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "broadcast-status");
    if (!media || !version || !g_str_equal(radios_json_string(media, "station_id", ""), radios_data_station_id(ctx)) ||
        !json_object_get_boolean_member(version, "verified") || !json_object_get_boolean_member(version, "object_available")) {
        gtk_label_set_text(GTK_LABEL(status), "Erişilebilir ve doğrulanmış kütüphane medyası seçin."); return;
    }
    JsonObject *binding = json_object_new(); const gchar *keys[] = {"media_id", "version_id", "media_file_id", "media_sha256"};
    const gchar *values[] = {radios_json_string(media, "_id", ""), radios_json_string(version, "_id", ""),
        radios_json_string(version, "media_file_id", ""), radios_json_string(version, "sha256", "")};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) json_object_set_string_member(binding, keys[i], values[i]);
    g_object_set_data_full(G_OBJECT(dialog), "programme-media-binding", binding, (GDestroyNotify)json_object_unref);
    const gchar *metadata[] = {"title", "artist", "album"}, *fields[] = {"programme-item-title", "programme-item-artist", "programme-item-album"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i)
        gtk_entry_set_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), fields[i])), radios_json_string(media, metadata[i], ""));
    gint64 *milliseconds = g_new(gint64, 1); *milliseconds = radios_json_int(version, "duration_ms", 0);
    gchar *duration = g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, *milliseconds / 60000, (*milliseconds / 1000) % 60);
    g_object_set_data_full(G_OBJECT(dialog), "programme-original-duration-ms", milliseconds, g_free);
    g_object_set_data_full(G_OBJECT(dialog), "programme-original-duration", g_strdup(duration), g_free);
    GtkWidget *input = g_object_get_data(G_OBJECT(dialog), "programme-item-duration"); gtk_entry_set_text(GTK_ENTRY(input), duration); gtk_widget_set_sensitive(input, FALSE); g_free(duration);
    const gchar *kinds[] = {"music", "announcement", "advertisement", "jingle", "program", "sweeper", "news", "effect", "voice_track", "other"};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) if (g_str_equal(kinds[i], radios_json_string(media, "kind", "")))
        gtk_combo_box_set_active(GTK_COMBO_BOX(g_object_get_data(G_OBJECT(dialog), "programme-item-type")), (gint)i);
    gtk_label_set_text(GTK_LABEL(status), "Doğrulanmış medya seçildi. Kaydet ile program öğesine bağlayın.");
}
static void programme_media_open(GtkButton *button, gpointer data)
{
    (void)button; GtkWidget *dialog = data; BroadcastView *view = g_object_get_data(G_OBJECT(dialog), "programme-view");
    if (!view->disposed) radios_media_library_pick(view->ctx, dialog, programme_media_picked, NULL);
}

static void programme_notice(BroadcastView *view, const gchar *message)
{ if (view->ctx->notify) view->ctx->notify(view->ctx->notify_data, message); }

static RadioSProgrammeTree *programme_tree(BroadcastView *view)
{ return radios_programme_tree_new(json_node_get_object(view->programme_config)); }

/* Playback index of the selected content, or of the selected event's first content. */
static gint programme_selected(BroadcastView *view)
{
    if (!view->selected_event || !view->programme_config) return -1;
    RadioSProgrammeTree *tree = programme_tree(view);
    gint index = radios_programme_tree_leaf_index(tree, view->selected_event);
    radios_programme_tree_free(tree); return index;
}
/* The selected flow item: its event and content (-1 for the event itself). */
static gboolean programme_item(BroadcastView *view, RadioSProgrammeTree *tree, guint *block, gint *leaf)
{ return view->selected_event && radios_programme_tree_find(tree, view->selected_event, block, leaf); }
static gboolean programme_leaf_selected(BroadcastView *view)
{
    if (!view->selected_event || !view->programme_config) return FALSE;
    RadioSProgrammeTree *tree = programme_tree(view); guint block = 0; gint leaf = -1;
    gboolean selected = programme_item(view, tree, &block, &leaf) && leaf >= 0;
    radios_programme_tree_free(tree); return selected;
}

static gboolean catalogue_save(BroadcastView *view, JsonNode *next);
/* Inserts leaves (the tree takes references) at placement; a new event takes
 * kind, else the shared kind of its contents ("program" when mixed). Returns its index. */
static guint programme_place(RadioSProgrammeTree *tree, RadioSProgrammePlacement placement, GPtrArray *leaves,
    const gchar *kind, const gchar *title)
{
    guint block = placement.block >= 0 ? (guint)placement.block : G_MAXUINT, position = placement.position;
    if (block >= tree->blocks->len) {
        if (!kind) {
            kind = leaves->len ? radios_json_string(g_ptr_array_index(leaves, 0), "kind", "music") : "music";
            for (guint i = 1; i < leaves->len; ++i)
                if (g_strcmp0(kind, radios_json_string(g_ptr_array_index(leaves, i), "kind", "music"))) kind = "program";
        }
        block = MIN(position, tree->blocks->len);
        radios_programme_tree_add_block(tree, block, kind, title);
        position = 0;
    }
    for (guint i = 0; i < leaves->len; ++i)
        radios_programme_tree_insert(tree, block, position + i, json_object_ref(g_ptr_array_index(leaves, i)));
    return block;
}
/* Saves an edited tree (consumed) as the draft; select stays selected. */
static gboolean programme_commit(BroadcastView *view, RadioSProgrammeTree *tree, const gchar *select)
{
    gchar *keep = g_strdup(select);
    JsonNode *next = radios_programme_tree_config(tree, json_node_get_object(view->programme_config));
    radios_programme_tree_free(tree);
    if (keep && g_strcmp0(keep, view->selected_event)) { g_free(view->selected_event); view->selected_event = g_strdup(keep); }
    g_free(keep);
    return catalogue_save(view, next);
}
/* Where catalogue content goes in the current mode: after the selected
 * content, at the end of the selected event, or as a new event at the end. */
static RadioSProgrammePlacement programme_target(BroadcastView *view, RadioSProgrammeTree *tree)
{
    RadioSProgrammePlacement placement = {-1, tree->blocks->len}; guint block = 0; gint leaf = -1;
    if (!programme_item(view, tree, &block, &leaf)) return placement;
    placement.block = (gint)block;
    placement.position = view->queue_mode == QUEUE_INSERT && leaf >= 0 ? (guint)leaf + 1 :
        ((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, block))->leaves->len;
    return placement;
}

static void placement_update(BroadcastView *view);
static void queue_selected(gpointer data, const gchar *id)
{
    BroadcastView *view = data;
    if (view->disposed) return;
    if (g_strcmp0(view->selected_event, id)) { g_free(view->selected_event); view->selected_event = g_strdup(id); }
    placement_update(view); catalog_actions_update(view);
}

static gboolean programme_can_change(BroadcastView *view)
{
    if (!view->queue_slot && general_mode(view)) {
        programme_notice(view, "Genel yayın sırası takvimden hazırlanır; program içeriğini Takvim/Program bölümünden düzenleyin."); return FALSE;
    }
    if (!view->programme_document || !radios_data_connected(view->ctx) || !radios_data_document_ready(view->programme_document)) {
        programme_notice(view, "Program kaydı ve veri bağlantısı hazır değil; seçiminiz ve girdileriniz korunuyor."); return FALSE;
    }
    return TRUE;
}

RadioSDocument *radios_broadcast_programme_document(RadioSViewContext *ctx)
{
    if (!ctx || !ctx->parent) return NULL;
    BroadcastView *view = g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context");
    return view && !view->disposed ? view->programme_document : NULL;
}

GtkWidget *radios_broadcast_programme_lifecycle_new(RadioSViewContext *ctx)
{
    BroadcastView *view = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context") : NULL;
    if (!view || view->disposed) return NULL;
    /* The panel reads the view's document slot and queue context: it must not
     * outlive the Broadcast page that owns them. */
    GtkWidget *panel = radios_programme_lifecycle_new(&view->lifecycle_context, view->page, &view->programme_document);
    g_signal_connect_object(view->page, "destroy", G_CALLBACK(gtk_widget_destroy), panel, G_CONNECT_SWAPPED);
    return panel;
}

gchar *radios_broadcast_programme_label(RadioSViewContext *ctx)
{
    BroadcastView *view = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context") : NULL;
    if (!view || view->disposed || !view->programme_config) return NULL;
    const gchar *title = radios_json_string(json_node_get_object(view->programme_config), "title", "");
    gchar *queue = NULL;
    if (view->queue_slot) {
        const RadioSPlayerDefinition *definition = view->ctx->players ?
            radios_player_topology_player_const(radios_player_workspace_topology(view->ctx->players), view->queue_slot - 1) : NULL;
        queue = definition && *definition->name ? g_strdup_printf("%s yayın sırası", definition->name) :
            g_strdup_printf("Oynatıcı %u yayın sırası", view->queue_slot);
    } else queue = g_strdup(general_mode(view) ? "Genel yayın sırası (takvimden)" : "Genel yayın sırası");
    gchar *label = g_strdup_printf("%s · %s", queue, *title ? title : "Adsız program");
    g_free(queue); return label;
}

gboolean radios_broadcast_append_playlist(RadioSViewContext *ctx, JsonArray *tracks, GError **error)
{ return radios_broadcast_append_event(ctx, tracks, NULL, error); }


/* Verified tracks become one new event at the end of the flow. */
static gboolean append_tracks(RadioSViewContext *ctx, JsonArray *tracks, const RadioSProgrammePlacement *placement,
    const gchar *title, GError **error);
gboolean radios_broadcast_append_event(RadioSViewContext *ctx, JsonArray *tracks, const gchar *title, GError **error)
{ return append_tracks(ctx, tracks, NULL, title, error); }
/* Verified tracks (prepare rows) enter the flow at placement, or as one new
 * event at the end when placement is NULL. */
static gboolean append_tracks(RadioSViewContext *ctx, JsonArray *tracks, const RadioSProgrammePlacement *placement,
    const gchar *title, GError **error)
{
    BroadcastView *view = g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context");
    if (!view || view->disposed || !programme_can_change(view)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Program kaydı ve veri bağlantısı hazır değil."); return FALSE;
    }
    if (!tracks || !json_array_get_length(tracks)) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Aktarılacak doğrulanmış öğe seçilmedi."); return FALSE; }
    RadioSProgrammeTree *tree = programme_tree(view);
    if (radios_programme_tree_leaf_count(tree) + json_array_get_length(tracks) > 5000) {
        radios_programme_tree_free(tree); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Program en fazla 5000 öğe içerebilir."); return FALSE;
    }
    GPtrArray *leaves = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
    const gchar *binding_keys[] = {"media_id", "version_id", "media_file_id", "media_sha256"};
    for (guint i = 0; i < json_array_get_length(tracks); ++i) {
        JsonNode *row_node = json_array_get_element(tracks, i);
        JsonObject *row = row_node && JSON_NODE_HOLDS_OBJECT(row_node) ? json_node_get_object(row_node) : NULL;
        if (!row) {
            g_ptr_array_unref(leaves); radios_programme_tree_free(tree);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Playlist doğrulama sonucu geçersiz."); return FALSE;
        }
        JsonObject *metadata = catalog_object(row, "metadata"); if (!metadata) metadata = row;
        JsonNode *detail_node = json_object_get_member(row, "detail"); JsonObject *detail = detail_node && JSON_NODE_HOLDS_OBJECT(detail_node) ? json_node_get_object(detail_node) : NULL;
        JsonObject *media = detail ? json_object_get_object_member(detail, "media") : NULL, *version = detail ? json_object_get_object_member(detail, "version") : NULL;
        const gchar *binding[] = {media ? radios_json_string(media, "_id", "") : radios_json_string(row, "media_id", ""),
            version ? radios_json_string(version, "_id", "") : radios_json_string(row, "version_id", ""),
            version ? radios_json_string(version, "media_file_id", "") : radios_json_string(row, "media_file_id", ""),
            version ? radios_json_string(version, "sha256", "") : radios_json_string(row, "media_sha256", "")};
        gboolean valid = g_uuid_string_is_valid(binding[0]) && g_uuid_string_is_valid(binding[1]) && g_uuid_string_is_valid(binding[2]) && strlen(binding[3]) == 64;
        for (guint j = 0; valid && binding[3][j]; ++j) valid = g_ascii_isdigit(binding[3][j]) || (binding[3][j] >= 'a' && binding[3][j] <= 'f');
        if (json_object_has_member(row, "eligible") && !json_object_get_boolean_member(row, "eligible")) valid = FALSE;
        if (media && !g_str_equal(radios_json_string(media, "station_id", ""), radios_data_station_id(ctx))) valid = FALSE;
        if (version && (!json_object_get_boolean_member(version, "verified") || !json_object_get_boolean_member(version, "object_available"))) valid = FALSE;
        if (!valid) {
            g_ptr_array_unref(leaves); radios_programme_tree_free(tree);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Seçilen öğelerden biri doğrulanmış medyaya bağlı değil; önce kütüphaneden medya seçin."); return FALSE;
        }
        JsonObject *event = json_object_new(); gchar *id = g_uuid_string_random(); json_object_set_string_member(event, "id", id); g_free(id);
        const gchar *keys[] = {"title", "artist", "album"}; const glong limits[] = {160,120,120};
        for (guint j = 0; j < G_N_ELEMENTS(keys); ++j) {
            const gchar *value = radios_json_string(metadata, keys[j], ""); gchar *text = g_utf8_substring(value, 0, MIN(g_utf8_strlen(value, -1), limits[j]));
            json_object_set_string_member(event, keys[j], text); g_free(text);
        }
        const gchar *kind = radios_json_string(media, "kind", radios_json_string(metadata, "kind", "music"));
        json_object_set_string_member(event, "kind", radios_programme_kind_valid(kind) ? kind : "music");
        json_object_set_int_member(event, "duration_ms", version ? radios_json_int(version, "duration_ms", 0) : radios_json_int(metadata, "duration_ms", 0));
        json_object_set_string_member(event, "start_time", ""); json_object_set_string_member(event, "version_label", "Taslak");
        json_object_set_string_member(event, "primary_file", ""); json_object_set_string_member(event, "backup_file", ""); json_object_set_string_member(event, "note", "");
        for (guint j = 0; j < G_N_ELEMENTS(binding_keys); ++j) json_object_set_string_member(event, binding_keys[j], binding[j]);
        g_ptr_array_add(leaves, event);
    }
    RadioSProgrammePlacement end = {-1, tree->blocks->len};
    RadioSProgrammePlacement target = placement ? *placement : end;
    if (target.block >= 0) programme_place(tree, target, leaves, NULL, title);
    else {
        /* Incoming music and station-ID runs are separate events, while
         * adjacent songs from one incoming block remain together. */
        for (guint start = 0; start < leaves->len;) {
            const gchar *kind = radios_json_string(g_ptr_array_index(leaves, start), "kind", "music");
            GPtrArray *segment = g_ptr_array_new(); guint stop = start;
            while (stop < leaves->len && !g_strcmp0(kind, radios_json_string(g_ptr_array_index(leaves, stop), "kind", "music")))
                g_ptr_array_add(segment, g_ptr_array_index(leaves, stop++));
            programme_place(tree, target, segment, kind, title);
            ++target.position; g_ptr_array_unref(segment); start = stop;
        }
    }
    gchar *first = placement && placement->block >= 0 ? g_strdup(radios_json_string(g_ptr_array_index(leaves, 0), "id", "")) : NULL;
    g_ptr_array_unref(leaves);
    gboolean queued = programme_commit(view, tree, first); g_free(first);
    if (!queued) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Program kaydı tamamlanamadı; mevcut sıra korunuyor.");
    return queued;
}

static BroadcastView *broadcast_context(RadioSViewContext *ctx)
{ return g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context"); }
static gchar *field_value(GtkWidget *dialog, const gchar *id)
{
    GtkWidget *field = g_object_get_data(G_OBJECT(dialog), id);
    if (!field) return g_strdup("");
    if (GTK_IS_TEXT_VIEW(field)) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(field)); GtkTextIter a, b;
        gtk_text_buffer_get_bounds(buffer, &a, &b); return gtk_text_buffer_get_text(buffer, &a, &b, FALSE);
    }
    return g_strdup(GTK_IS_ENTRY(field) ? gtk_entry_get_text(GTK_ENTRY(field)) : "");
}
static void object_field(JsonObject *object, const gchar *key, GtkWidget *dialog, const gchar *id)
{ gchar *value = field_value(dialog, id); json_object_set_string_member(object, key, value); g_free(value); }
static void programme_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; (void)owner; PlayerQueue *queue = data; BroadcastView *view = queue->view;
    if (view->disposed) return;
    if (queue->slot && radios_json_int(config, "queue_player", 0) != queue->slot) {
        programme_notice(view, "Oynatıcı sırasının kayıt kimliği uyuşmuyor; kayıt yüklenmedi."); return;
    }
    if (!queue->slot) {
        g_clear_pointer(&view->manual_general_config, json_node_free);
        view->manual_general_config = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(view->manual_general_config, radios_programme_clone_object(config));
        if (general_mode(view)) return;
    }
    g_clear_pointer(&queue->config, json_node_free);
    queue->config = json_node_new(JSON_NODE_OBJECT); json_node_set_object(queue->config, config);
    if (queue->slot) { queue->transition = (guint)radios_json_int(config, "queue_transition", 0); queue->overlap = 0; }
    if (queue->slot != view->queue_slot) return;
    g_clear_pointer(&view->programme_config, json_node_free);
    view->programme_config = programme_copy(queue->config);
    view->queue_switching = TRUE;
    gtk_combo_box_set_active(GTK_COMBO_BOX(view->transition_mode), (gint)queue->transition);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->transition_overlap), queue->overlap);
    view->queue_switching = FALSE;
    programme_render(view);
}
static void broadcast_free(gpointer data)
{
    BroadcastView *view = data;
    g_clear_pointer(&view->programme_config, json_node_free); g_clear_pointer(&view->catalog_records, json_node_free);
    g_clear_pointer(&view->programme_library_records, json_node_free); g_clear_object(&view->catalog_cancel);
    g_clear_pointer(&view->catalog_result, json_node_free); g_clear_pointer(&view->catalog_error, g_free);
    for (guint index = 0; index < G_N_ELEMENTS(view->queues); ++index) {
        g_clear_pointer(&view->queues[index].config, json_node_free);
        g_clear_pointer(&view->queues[index].run_config, json_node_free); g_free(view->queues[index].selected);
    }
    g_clear_pointer(&view->manual_general_config, json_node_free);
    g_clear_pointer(&view->policy, json_node_free);
    g_clear_pointer(&view->policy_client, radios_data_client_unref);
    g_free(view->player_topology_key);
    g_clear_object(&view->local_event_cancel); g_weak_ref_clear(&view->local_event_dialog);
    g_free(view->local_event_target); g_free(view->local_event_target_document_id);
    g_hash_table_unref(view->event_overrides);
    g_clear_pointer(&view->live_schema_jobs, g_hash_table_unref);
    g_clear_pointer(&view->local_event_target_baseline, json_node_free);
    flow_index_clear(view); g_clear_pointer(&view->live_steps, g_hash_table_unref);
    g_free(view->transition_source); g_free(view->transition_target); g_free(view->ending_event); g_free(view->talk_event);
    g_free(view->live_step_on_air); g_free(view->plain_talk); g_clear_pointer(&view->run_removed, g_hash_table_unref); g_clear_pointer(&view->live_steps_done, g_hash_table_unref);
    g_free(view->event_run_origin); g_free(view->overlay_leaf); g_free(view->overlay_title); g_free(view->overlay_id);
    g_free(view->selected_event); g_free(view->history_latest); g_free(view);
}
static void programme_render(BroadcastView *view)
{
    if (!view->queue_table || !view->programme_config) return;
    view->rendering = TRUE;
    JsonObject *config = json_node_get_object(view->programme_config);
    JsonArray *events = json_object_get_array_member(config, "events");
    if (view->programme_title) {
        gtk_label_set_text(GTK_LABEL(view->programme_title), radios_json_string(config, "title", ""));
        gtk_widget_set_tooltip_text(view->programme_title, radios_json_string(config, "title", ""));
    }
    if (view->programme_date) gtk_label_set_text(GTK_LABEL(view->programme_date), radios_json_string(config, "date", ""));
    if (view->timing_target) {
        JsonObject *timing = json_object_has_member(config, "timing") ? json_object_get_object_member(config, "timing") : NULL;
        gchar *target = g_strdup_printf("Hedef saat  %s", radios_json_string(timing, "target_time", "—:—")); gtk_label_set_text(GTK_LABEL(view->timing_target), target); g_free(target);
        const gchar *title = "Kesin saatli olay  —";
        for (guint i = 0; timing && events && i < json_array_get_length(events); ++i) {
            JsonObject *event = json_array_get_object_element(events, i);
            if (g_str_equal(radios_json_string(timing, "event_id", ""), radios_json_string(event, "id", ""))) title = radios_json_string(event, "title", "");
        }
        gtk_label_set_text(GTK_LABEL(view->timing_title), title);
        gchar *policy = timing ? g_strdup_printf("Davranış  %s · %.0f sn", g_str_equal(radios_json_string(timing, "policy", "cut"), "cut") ? "Hedef saatte kes" : "Bitmesini bekle", radios_json_int(timing, "max_wait_ms", 0) / 1000.0) : g_strdup("Davranış  —");
        gtk_label_set_text(GTK_LABEL(view->timing_policy), policy); g_free(policy);
    }
    RadioSProgrammeTree *tree = radios_programme_tree_new(config); gint64 total = 0;
    if (!view->queue_slot && general_mode(view)) {
        GHashTableIter overrides; gpointer origin;
        g_hash_table_iter_init(&overrides, view->event_overrides);
        while (g_hash_table_iter_next(&overrides, &origin, NULL))
            if (!radios_programme_tree_find(tree, origin, NULL, NULL)) g_hash_table_iter_remove(&overrides);
    }
    if (view->selected_event && !radios_programme_tree_find(tree, view->selected_event, NULL, NULL)) g_clear_pointer(&view->selected_event, g_free);
    for (guint i = 0; i < tree->blocks->len; ++i) total += radios_programme_block_duration(g_ptr_array_index(tree->blocks, i));
    if (view->queue_count) {
        gchar *count = g_strdup_printf("%u event · %u içerik", tree->blocks->len, radios_programme_tree_leaf_count(tree));
        gtk_label_set_text(GTK_LABEL(view->queue_count), count); g_free(count);
    }
    if (view->queue_duration) {
        gint64 seconds = (total + 500) / 1000;
        gchar *duration = !total ? g_strdup("—") : seconds >= 3600 ?
            g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, seconds / 3600, seconds / 60 % 60, seconds % 60) :
            g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, seconds / 60, seconds % 60);
        gtk_label_set_text(GTK_LABEL(view->queue_duration), duration); g_free(duration);
    }
    radios_programme_tree_free(tree);
    ++view->config_serial; programme_view_load(view); programme_view_apply(view);
    radios_programme_queue_render(view->queue_table, config, view->selected_event);
    view->rendering = FALSE;
    placement_update(view);
    catalog_actions_update(view);
}

static gboolean general_mode(BroadcastView *view)
{
    return view->general_enabled && view->general_automation != NULL;
}
static gboolean general_controls(BroadcastView *view)
{
    if (!general_mode(view)) return FALSE;
    RadioSPlayerWeeklySnapshot schedule = {0}; radios_player_weekly_get_snapshot(view->general_automation, &schedule);
    if (schedule.armed) return TRUE;
    /* Another native view may own an accepted MAIN run. Its transport remains
     * available while the unarmed scheduler only observes future programmes. */
    RadioSPlayoutSnapshot run = {0}; radios_playout_ui_get_snapshot(view->ctx, &run);
    return run.state == RADIOS_PLAYOUT_IDLE || run.state == RADIOS_PLAYOUT_FINISHED || run.state == RADIOS_PLAYOUT_ERROR;
}
static void general_queue_apply(JsonObject *programme, gboolean running, gpointer data)
{
    BroadcastView *view = data; if (view->disposed || !view->general_enabled) return;
    RadioSPlayerWeeklySnapshot schedule = {0};
    radios_player_weekly_get_snapshot(view->general_automation, &schedule);
    JsonNode *next = programme ? json_node_new(JSON_NODE_OBJECT) : NULL;
    if (next) json_node_take_object(next, radios_programme_clone_object(programme));
    else next = queue_empty(0);
    /* U7: what × took out of the running flow stays out of its projection;
     * a new or stopped run shows the calendar's content again. */
    if (!running && view->run_removed) g_hash_table_remove_all(view->run_removed);
    if (running && view->run_removed && g_hash_table_size(view->run_removed)) {
        RadioSProgrammeTree *tree = radios_programme_tree_new(json_node_get_object(next));
        GHashTableIter removed; gpointer key; gboolean changed_tree = FALSE; g_hash_table_iter_init(&removed, view->run_removed);
        while (g_hash_table_iter_next(&removed, &key, NULL)) changed_tree |= radios_programme_tree_remove(tree, key);
        if (changed_tree) { JsonNode *filtered = radios_programme_tree_config(tree, json_node_get_object(next)); json_node_free(next); next = filtered; }
        radios_programme_tree_free(tree);
    }
    gboolean changed = !view->queues[0].config || !json_node_equal(next, view->queues[0].config);
    view->general_projection = programme != NULL;
    g_clear_pointer(&view->queues[0].config, json_node_free); view->queues[0].config = next;
    if (running) {
        g_clear_pointer(&view->queues[0].run_config, json_node_free);
        view->queues[0].run_config = programme_copy(next);
    } else if (changed && !schedule.armed) g_clear_pointer(&view->queues[0].run_config, json_node_free);
    if (!view->queue_slot && (changed || running)) {
        g_clear_pointer(&view->programme_config, json_node_free); view->programme_config = programme_copy(next);
        g_clear_pointer(&view->selected_event, g_free);
        radios_programme_queue_reset_progress(view->queue_table); programme_render(view);
        radios_programme_queue_set_run_config(view->queue_table, running ? json_node_get_object(next) : NULL);
    }
    general_status_update(view);
}
static void general_status_update(BroadcastView *view)
{
    if (!view->general_status || !view->general_automation || view->disposed) return;
    RadioSPlayerWeeklySnapshot schedule = {0}; radios_player_weekly_get_snapshot(view->general_automation, &schedule);
    gint64 start = schedule.current_event_name[0] ? schedule.start_utc_us : schedule.next_start_utc_us;
    gchar *when = NULL;
    if (start > 0) {
        GDateTime *utc = g_date_time_new_from_unix_utc(start / G_USEC_PER_SEC);
        GTimeZone *zone = radios_time_zone_new(radios_station_timezone(view->ctx));
        GDateTime *local = utc && zone ? g_date_time_to_timezone(utc, zone) : NULL;
        when = local ? g_date_time_format(local, "%d.%m %H:%M") : g_strdup("");
        if (local) g_date_time_unref(local);
        if (utc) g_date_time_unref(utc);
        if (zone) g_time_zone_unref(zone);
    }
    const gchar *name = schedule.current_event_name[0] ? schedule.current_event_name : schedule.next_event_name;
    gchar *text = schedule.error[0] ? g_strdup_printf("Otomasyon: %s", schedule.error) : name[0] ?
        g_strdup_printf("%s · %s%s%s", schedule.armed ? schedule.paused ? "Otomasyon duraklatıldı" : "Sürekli otomasyon" : schedule.current_event_name[0] ? "Yayındaki program" : "Sıradaki program",
            name, when && *when ? " · " : "", when ? when : "") :
        g_strdup(schedule.armed ? "Sürekli otomasyon · Gelecek program bekleniyor" : "Takvimde gelecek program aranıyor");
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(view->general_status)), text)) gtk_label_set_text(GTK_LABEL(view->general_status), text);
    g_free(text); g_free(when);
    gtk_widget_set_visible(view->general_status, !view->queue_slot && view->general_enabled);
}
static void general_enabled_changed(GtkToggleButton *button, gpointer data)
{
    BroadcastView *view = data; if (view->disposed) return;
    view->general_enabled = gtk_toggle_button_get_active(button);
    if (view->general_enabled) {
        if (!view->general_automation) view->general_automation = radios_player_weekly_new_general(view->ctx, view->page);
        g_object_set_data(G_OBJECT(view->page), "radios-general-programme-driver", view->general_automation);
        radios_player_weekly_general_set_queue(view->general_automation, general_queue_apply, view);
        radios_player_weekly_general_refresh(view->general_automation);
    } else {
        g_object_set_data(G_OBJECT(view->page), "radios-general-programme-driver", NULL);
        if (view->general_automation) radios_player_weekly_free(view->general_automation);
        view->general_automation = NULL; view->general_projection = FALSE;
        gtk_widget_hide(view->general_status);
        g_clear_pointer(&view->queues[0].config, json_node_free);
        view->queues[0].config = view->manual_general_config ? programme_copy(view->manual_general_config) : queue_empty(0);
        g_clear_pointer(&view->queues[0].run_config, json_node_free);
        if (!view->queue_slot) {
            g_clear_pointer(&view->programme_config, json_node_free); view->programme_config = programme_copy(view->queues[0].config);
            g_clear_pointer(&view->selected_event, g_free);
            radios_programme_queue_reset_progress(view->queue_table); programme_render(view);
        }
    }
    general_status_update(view); catalog_actions_update(view);
}
static void placement_update(BroadcastView *view)
{
    if (!view->placement || !view->programme_config) return;
    RadioSProgrammeTree *tree = programme_tree(view); guint block = 0; gint leaf = -1; gchar *text;
    if (programme_item(view, tree, &block, &leaf)) {
        const gchar *mode = view->queue_mode == QUEUE_REPLACE ? "Seçili içeriği değiştir" :
            view->queue_mode == QUEUE_INSERT && leaf >= 0 ? "Seçili içeriğin sonrasına ekle" : "Event sonuna ekle";
        text = g_strdup_printf("Event: %s · %s", radios_json_string(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, block))->block, "title", ""), mode);
    } else text = g_strdup(view->queue_mode == QUEUE_REPLACE ? "Seçili içeriği değiştir" : "Sona yeni event olarak ekle");
    gtk_label_set_text(GTK_LABEL(view->placement), text); g_free(text);
    radios_programme_tree_free(tree);
}
static void programme_save(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; BroadcastView *view = data;
    if (view && !view->queue_slot && general_mode(view)) return;
    if (view && view->programme_config) queue_binding(view, json_node_get_object(view->programme_config));
    if (view && view->programme_document) radios_data_document_save(view->programme_document, view->programme_config);
}
/* Starts the programme flow at a playback index (▶ or a row's S). */
static void programme_start(BroadcastView *view, guint start)
{
    RadioSViewContext *context = programme_context(view);
    if (view->disposed || !context->playout) return;
    if (!view->queue_slot && general_mode(view)) {
        GError *error = NULL;
        if (!radios_player_weekly_start_general(view->general_automation, &error))
            programme_notice(view, error ? error->message : "Takvim otomasyonu başlatılamadı.");
        g_clear_error(&error); general_status_update(view); return;
    }
    GError *error = NULL; JsonObject *config = json_node_get_object(view->programme_config);
    guint mode = (guint)MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(view->transition_mode)));
    if (mode == 4) mode = RADIOS_AUDIO_RUN_CROSSFADE;
    gdouble overlap = gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->transition_overlap));
    gboolean accepted = radios_playout_ui_start_programme(context, view->page, config, start, mode, overlap, &error);
    if (accepted) {
        PlayerQueue *queue = &view->queues[view->queue_slot];
        queue->hint_epoch = 0; queue->hint_count = 0;
        g_clear_pointer(&queue->run_config, json_node_free);
        queue->run_config = programme_copy(view->programme_config);
        radios_programme_queue_reset_progress(view->queue_table);
        radios_programme_queue_set_run_config(view->queue_table, json_node_get_object(queue->run_config));
    } else programme_notice(view, error ? error->message : "Yayın sırası komutu kabul edilmedi.");
    g_clear_error(&error);
}
/* Keep accepted event ownership per runner, including scheduled starts from
 * other native pages. */
void radios_broadcast_queue_run_config(RadioSViewContext *ctx, JsonObject *config)
{
    if (!ctx || !ctx->parent || !ctx->playout) return;
    BroadcastView *view = g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context");
    if (!view || view->disposed) return;
    guint slot = G_MAXUINT;
    if (ctx->playout == view->ctx->playout) slot = 0;
    else for (guint i = 1; i < G_N_ELEMENTS(view->queues); ++i)
        if (ctx->playout == view->queues[i].context.playout) { slot = i; break; }
    if (slot == G_MAXUINT) return;
    PlayerQueue *queue = &view->queues[slot];
    queue->hint_epoch = 0; queue->hint_count = 0;
    g_clear_pointer(&queue->run_config, json_node_free);
    if (config) {
        JsonNode *input = json_node_new(JSON_NODE_OBJECT); json_node_set_object(input, config);
        queue->run_config = programme_copy(input); json_node_free(input);
    }
    if (slot == view->queue_slot) {
        radios_programme_queue_reset_progress(view->queue_table);
        radios_programme_queue_set_run_config(view->queue_table, queue->run_config ? json_node_get_object(queue->run_config) : NULL);
    }
}
static void programme_transport(GtkButton *button, gpointer data)
{
    BroadcastView *view = data; RadioSViewContext *context = programme_context(view); if (view->disposed || !context->playout) return;
    guint action = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "programme-transport-action")); GError *error = NULL;
    const RadioSPlayoutAction flow_actions[] = {RADIOS_PLAYOUT_PREVIOUS, RADIOS_PLAYOUT_RESUME, RADIOS_PLAYOUT_PAUSE, RADIOS_PLAYOUT_STOP, RADIOS_PLAYOUT_NEXT};
    if (action == 3) radios_event_playout_cancel(view->ctx, view->queue_slot ? view->queue_slot - 1 : G_MAXUINT);
    if (!view->queue_slot && general_controls(view)) {
        gboolean accepted = radios_player_weekly_general_action(view->general_automation, flow_actions[action], &error);
        if (accepted && action == 3) radios_programme_queue_reset_progress(view->queue_table);
        if (!accepted) programme_notice(view, error ? error->message : "Otomasyon komutu kabul edilmedi.");
        g_clear_error(&error); general_status_update(view); return;
    }
    RadioSPlayoutSnapshot snapshot = {0}; radios_playout_ui_get_snapshot(context, &snapshot); gboolean accepted;
    if (action == 1 && snapshot.state == RADIOS_PLAYOUT_PAUSED) accepted = radios_playout_ui_action(context, RADIOS_PLAYOUT_RESUME, &error);
    else if (action == 1) { programme_start(view, (guint)MAX(0, programme_selected(view))); return; }
    else {
        const RadioSPlayoutAction actions[] = {RADIOS_PLAYOUT_PREVIOUS, RADIOS_PLAYOUT_RESUME, RADIOS_PLAYOUT_PAUSE, RADIOS_PLAYOUT_STOP, RADIOS_PLAYOUT_NEXT};
        accepted = radios_playout_ui_action(context, actions[action], &error);
    }
    if (accepted && action == 3) radios_programme_queue_reset_progress(view->queue_table);
        else if (accepted && (action == 0 || action == 4)) radios_programme_queue_set_progress(view->queue_table, NULL, NULL, 0, 0, FALSE);
    else if (!accepted) programme_notice(view, error ? error->message : "Yayın sırası komutu kabul edilmedi.");
    g_clear_error(&error);
}
/* Shared header controls follow the broadcast view's selected queue. */
static BroadcastView *navigation_view(RadioSViewContext *ctx)
{
    if (!ctx || !ctx->parent) return NULL;
    BroadcastView *view = g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context");
    return view && !view->disposed && view->programme_config ? view : NULL;
}

gboolean radios_broadcast_navigation_queue_active(RadioSViewContext *ctx)
{
    BroadcastView *view = navigation_view(ctx);
    if (!view) return FALSE;
    RadioSViewContext *context = programme_context(view);
    if (!context->playout) return FALSE;
    if (!view->queue_slot) {
        RadioSPlayerWeeklySnapshot schedule = {0};
        if (general_mode(view) && radios_player_weekly_get_snapshot(view->general_automation, &schedule) && schedule.armed) return TRUE;
    }
    RadioSAudioRunSnapshot run = {0};
    return radios_playout_ui_navigation_snapshot(context, &run) && run.state != RADIOS_AUDIO_RUN_IDLE &&
        run.state != RADIOS_AUDIO_RUN_FINISHED && run.state != RADIOS_AUDIO_RUN_ERROR;
}

gboolean radios_broadcast_navigation_general_continuous(RadioSViewContext *ctx)
{
    /* Only a general queue that shows the calendar's programme (now or next)
     * takes the header's Play; without one the assigned programme runs as
     * its cluster's own flow. */
    BroadcastView *view = navigation_view(ctx);
    if (!view || view->queue_slot || !general_mode(view) || !view->general_projection || !view->programme_config) return FALSE;
    JsonNode *events = json_object_get_member(json_node_get_object(view->programme_config), "events");
    return events && JSON_NODE_HOLDS_ARRAY(events) && json_array_get_length(json_node_get_array(events)) > 0;
}

gboolean radios_broadcast_navigation_queue_playable(RadioSViewContext *ctx)
{
    BroadcastView *view = navigation_view(ctx);
    if (!view) return FALSE;
    RadioSViewContext *context = programme_context(view);
    if (!context->playout) return FALSE;
    if (!view->queue_slot && general_controls(view)) return TRUE;
    RadioSAudioRunSnapshot run = {0};
    if (radios_playout_ui_navigation_snapshot(context, &run) &&
        (run.state == RADIOS_AUDIO_RUN_PAUSED || run.state == RADIOS_AUDIO_RUN_PLAYING ||
         run.state == RADIOS_AUDIO_RUN_OVERLAP || run.state == RADIOS_AUDIO_RUN_MANUAL_WAIT)) return TRUE;
    JsonObject *config = json_node_get_object(view->programme_config);
    JsonArray *events = json_object_has_member(config, "events") ? json_object_get_array_member(config, "events") : NULL;
    return events && json_array_get_length(events) > 0;
}

gboolean radios_broadcast_navigation_available(RadioSViewContext *ctx, guint action)
{
    BroadcastView *view = navigation_view(ctx);
    if (!view || action > 5) return FALSE;
    RadioSViewContext *context = programme_context(view);
    if (!context->playout) return FALSE;
    if (!view->queue_slot && general_controls(view)) {
        RadioSPlayerWeeklySnapshot schedule = {0}; radios_player_weekly_get_snapshot(view->general_automation, &schedule);
        if (action == 1) return radios_data_connected(context);
        if (action == 2) return schedule.armed && !schedule.paused;
        if (action == 3) return schedule.armed;
        if (schedule.paused || !schedule.armed) return FALSE;
    }
    RadioSPlayoutSnapshot snapshot = {0};
    radios_playout_ui_get_snapshot(context, &snapshot);
    RadioSAudioRunSnapshot run = {0};
    gboolean running = radios_playout_ui_navigation_snapshot(context, &run);
    gboolean navigating = running && (run.state == RADIOS_AUDIO_RUN_PLAYING ||
        run.state == RADIOS_AUDIO_RUN_OVERLAP || run.state == RADIOS_AUDIO_RUN_MANUAL_WAIT);
    switch (action) {
    case 0: return navigating && run.index > 0;
    case 1: return radios_data_connected(context) && snapshot.state != RADIOS_PLAYOUT_PREPARING;
    case 2: return running && (run.state == RADIOS_AUDIO_RUN_PLAYING || run.state == RADIOS_AUDIO_RUN_OVERLAP);
    case 3: return snapshot.state != RADIOS_PLAYOUT_IDLE && snapshot.state != RADIOS_PLAYOUT_FINISHED &&
        snapshot.state != RADIOS_PLAYOUT_ERROR;
    case 4: return navigating && run.index + 1 < run.count;
    case 5: return radios_playout_ui_can_next_playlist(context);
    }
    return FALSE;
}

void radios_broadcast_navigation_status(RadioSViewContext *ctx,
    gboolean *previous, gboolean *next, gboolean *playlist)
{
    if (previous) *previous = radios_broadcast_navigation_available(ctx, 0);
    if (next) *next = radios_broadcast_navigation_available(ctx, 4);
    if (playlist) *playlist = radios_broadcast_navigation_available(ctx, 5);
}

gboolean radios_broadcast_navigation_action(RadioSViewContext *ctx, guint action, GError **error)
{
    BroadcastView *view = navigation_view(ctx);
    if (!view || action > 5 || !radios_broadcast_navigation_available(ctx, action)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Seçili yayın sırası için bu komut şu an kullanılamıyor.");
        return FALSE;
    }
    RadioSViewContext *context = programme_context(view);
    if (action == 3) radios_event_playout_cancel(view->ctx, view->queue_slot ? view->queue_slot - 1 : G_MAXUINT);
    if (!view->queue_slot && general_controls(view) && action < 5) {
        const RadioSPlayoutAction actions[] = {RADIOS_PLAYOUT_PREVIOUS, RADIOS_PLAYOUT_RESUME, RADIOS_PLAYOUT_PAUSE, RADIOS_PLAYOUT_STOP, RADIOS_PLAYOUT_NEXT};
        gboolean accepted = radios_player_weekly_general_action(view->general_automation, actions[action], error);
        if (accepted && action == 3) radios_programme_queue_reset_progress(view->queue_table);
        general_status_update(view); return accepted;
    }
    if (action == 5) {
        gboolean accepted = radios_playout_ui_next_playlist(context, error);
        if (accepted) radios_programme_queue_set_progress(view->queue_table, NULL, NULL, 0, 0, FALSE);
        return accepted;
    }
    /* Play uses the existing selection/prepare/resume workflow, including its
     * asynchronous validation and operator error reporting. */
    if (action == 1) {
        RadioSAudioRunSnapshot run = {0};
        /* Pause changes the engine synchronously; the UI snapshot may still
         * describe PLAYING until its next poll. Resume that same owned run. */
        gboolean owned = radios_playout_ui_navigation_snapshot(context, &run);
        if (owned && run.state == RADIOS_AUDIO_RUN_PAUSED)
            return radios_playout_ui_action(context, RADIOS_PLAYOUT_RESUME, error);
        /* A run already on air (a second press after a delayed resume, or a
         * press during a wait) is never restarted from its first row. */
        if (owned && run.state != RADIOS_AUDIO_RUN_IDLE && run.state != RADIOS_AUDIO_RUN_FINISHED && run.state != RADIOS_AUDIO_RUN_ERROR)
            return TRUE;
        programme_transport(GTK_BUTTON(view->transport[1]), view);
        return TRUE;
    }
    const RadioSPlayoutAction actions[] = {
        RADIOS_PLAYOUT_PREVIOUS, RADIOS_PLAYOUT_RESUME, RADIOS_PLAYOUT_PAUSE,
        RADIOS_PLAYOUT_STOP, RADIOS_PLAYOUT_NEXT
    };
    gboolean accepted = radios_playout_ui_action(context, actions[action], error);
    if (accepted && action == 3) radios_programme_queue_reset_progress(view->queue_table);
    else if (accepted && (action == 0 || action == 4))
        radios_programme_queue_set_progress(view->queue_table, NULL, NULL, 0, 0, FALSE);
    return accepted;
}
/* U3 (C10) header playlist picker: the same selected queue owner,
 * availability and event-list reset as action 5 ("Sonraki playlist"). Under
 * the general driver's controls only the driver's own run is listed/jumped. */
static gboolean navigation_playlist_owner(BroadcastView *view, GError **error)
{
    RadioSPlayerWeeklySnapshot schedule = {0};
    if (view->queue_slot || !general_controls(view) ||
        radios_player_weekly_header_snapshot(view->general_automation, &schedule)) return TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Genel program bu yayının sahibi değil.");
    return FALSE;
}
JsonArray *radios_broadcast_navigation_playlists(RadioSViewContext *ctx)
{
    BroadcastView *view = navigation_view(ctx);
    if (!view || !radios_broadcast_navigation_available(ctx, 5) || !navigation_playlist_owner(view, NULL)) return NULL;
    return radios_playout_ui_playlist_choices(programme_context(view));
}
gboolean radios_broadcast_navigation_jump_playlist(RadioSViewContext *ctx, guint64 epoch, guint index,
    const gchar *entry_id, GError **error)
{
    BroadcastView *view = navigation_view(ctx);
    if (!view || !radios_broadcast_navigation_available(ctx, 5)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Seçili yayın sırası için bu komut şu an kullanılamıyor.");
        return FALSE;
    }
    if (!navigation_playlist_owner(view, error)) return FALSE;
    gboolean accepted = radios_playout_ui_jump_playlist(programme_context(view), epoch, index, entry_id, error);
    if (accepted) radios_programme_queue_set_progress(view->queue_table, NULL, NULL, 0, 0, FALSE);
    return accepted;
}


/* Overlay the accepted plan's jingle schedule on matching draft rows only.
 * This transient copy is never saved back to the programme document. */
static void programme_runtime_hints(BroadcastView *view)
{
    PlayerQueue *queue = &view->queues[view->queue_slot];
    if (!queue->run_config) return;
    RadioSAudioRunSnapshot run = {0};
    RadioSViewContext *ctx = programme_context(view);
    if (!radios_playout_ui_navigation_snapshot(ctx, &run) || !run.count ||
        run.state == RADIOS_AUDIO_RUN_IDLE || run.state == RADIOS_AUDIO_RUN_FINISHED || run.state == RADIOS_AUDIO_RUN_ERROR ||
        (queue->hint_epoch == run.epoch && queue->hint_count == run.count)) return;
    JsonNode *order = radios_playout_ui_plan_snapshot(ctx); if (!order) return;
    JsonArray *entries = json_object_get_array_member(json_node_get_object(order), "entries");
    /* A-12/A-18: one pass over the accepted plan, one over the draft, so a
     * 2000-content programme stays O(n) per plan change. The first plan row of
     * an entry id wins, as before. A 4c live_step hint on a media row binds
     * the live event that follows it (A-19). */
    GHashTable *by_entry = g_hash_table_new(g_str_hash, g_str_equal);
    g_clear_pointer(&view->live_steps, g_hash_table_unref);
    view->live_steps = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, live_step_free);
    for (guint j = 0; entries && j < json_array_get_length(entries); ++j) {
        JsonObject *entry = json_array_get_object_element(entries, j);
        const gchar *id = radios_json_string(entry, "entry_id", "");
        if (*id && !g_hash_table_contains(by_entry, id)) g_hash_table_insert(by_entry, (gpointer)id, entry);
        JsonObject *step = catalog_object(entry, "live_step");
        if (step) live_step_record(view, entry, step, j + 1 < json_array_get_length(entries) ? json_array_get_object_element(entries, j + 1) : NULL);
    }
    JsonObject *accepted = radios_programme_clone_object(json_node_get_object(queue->run_config));
    JsonArray *events = json_object_get_array_member(accepted, "events");
    for (guint i = 0; events && i < json_array_get_length(events); ++i) {
        JsonObject *leaf = json_array_get_object_element(events, i);
        json_object_remove_member(leaf, "planned_jingles"); json_object_remove_member(leaf, "planned_jingle");
        JsonObject *entry = g_hash_table_lookup(by_entry, radios_json_string(leaf, "id", ""));
        if (!entry) continue;
        JsonNode *hints = json_object_get_member(entry, "planned_jingles");
        if (hints && JSON_NODE_HOLDS_ARRAY(hints))
            json_object_set_member(leaf, "planned_jingles", radios_programme_clone(hints));
        gint64 player = radios_json_int(entry, "player", 0);
        if (player >= 1 && player <= 32) json_object_set_int_member(leaf, "player", player);
    }
    g_hash_table_unref(by_entry);
    queue->hint_epoch = run.epoch; queue->hint_count = run.count;
    radios_programme_queue_set_run_config(view->queue_table, accepted);
    json_object_unref(accepted); json_node_free(order);
}
static void programme_event_runs_tick(BroadcastView *view)
{
    JsonNode *snapshot = radios_event_playout_display_snapshot(view->ctx);
    JsonObject *data = snapshot && JSON_NODE_HOLDS_OBJECT(snapshot) ? json_node_get_object(snapshot) : NULL;
    JsonNode *node = data ? json_object_get_member(data, "runs") : NULL;
    JsonArray *runs = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
    radios_programme_queue_set_event_runs(view->queue_table, runs);
    /* The sounding independent event run is on air for L/R and the header. */
    const gchar *origin = NULL;
    for (guint i = 0; runs && !origin && i < json_array_get_length(runs); ++i) {
        JsonObject *run = json_array_get_object_element(runs, i); gint64 state = radios_json_int(run, "state", RADIOS_PLAYOUT_IDLE);
        if (state == RADIOS_PLAYOUT_PLAYING || state == RADIOS_PLAYOUT_OVERLAP || state == RADIOS_PLAYOUT_PAUSED)
            origin = radios_json_string(run, "origin_block_id", "");
    }
    if (origin && !*origin) origin = NULL;
    if (g_strcmp0(origin, view->event_run_origin)) { g_free(view->event_run_origin); view->event_run_origin = g_strdup(origin); }
    JsonArray *completed = radios_event_playout_take_completed(view->ctx);
    for (guint i = 0; completed && i < json_array_get_length(completed); ++i) {
        JsonObject *receipt = json_array_get_object_element(completed, i);
        gint64 source = radios_json_int(receipt, "source", -1);
        if (source >= 0 && source < 32) radios_programme_queue_complete_event_run(view->queue_table,
            radios_json_string(receipt, "origin_block_id", ""), (guint)source);
    }
    if (completed) json_array_unref(completed);
    g_clear_pointer(&snapshot, json_node_free);
}
/* ---- Kılavuz 2026-10-18 · yayın sırası görünümü ------------------------- */
/* Per-programme counts (PDF §06 "Programa göre event ayarları") and the
 * refresh interval (§08). They are saved as the programme's top-level "view"
 * object; the read-only calendar projection of the general queue keeps them
 * per programme title in the "Mevcut program" document ("view_by_title"). */
#define PROGRAMME_VIEW_DEFAULTS {12, 8, 50, 20, 5}
static const struct { const gchar *key, *label, *id; guint low, high; } programme_view_fields[] = {
    {"event_fetch", "Event çekme adedi", "programme-view-event-fetch", 1, 500},
    {"event_show", "Event gösterme adedi", "programme-view-event-show", 1, 200},
    {"content_fetch", "İçerik çekme adedi", "programme-view-content-fetch", 1, 500},
    {"content_show", "İçerik gösterme adedi", "programme-view-content-show", 1, 500},
    {"refresh_seconds", "Yenilenme (sn)", "programme-view-refresh", 1, 60},
};
static guint *programme_view_slot(ProgrammeView *settings, guint field)
{
    guint *slots[] = {&settings->event_fetch, &settings->event_show, &settings->content_fetch, &settings->content_show, &settings->refresh_seconds};
    return slots[field];
}
static JsonArray *catalog_array(JsonObject *object, const gchar *key)
{ JsonNode *node = object ? json_object_get_member(object, key) : NULL; return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL; }
static gboolean programme_view_read(JsonObject *object, ProgrammeView *settings)
{
    if (!object) return FALSE;
    ProgrammeView parsed = PROGRAMME_VIEW_DEFAULTS;
    for (guint i = 0; i < G_N_ELEMENTS(programme_view_fields); ++i) {
        if (!json_object_has_member(object, programme_view_fields[i].key)) return FALSE;
        gint64 value = radios_json_int(object, programme_view_fields[i].key, -1);
        if (value < programme_view_fields[i].low || value > programme_view_fields[i].high) return FALSE;
        *programme_view_slot(&parsed, i) = (guint)value;
    }
    if (parsed.event_fetch < parsed.event_show || parsed.content_fetch < parsed.content_show) return FALSE;
    *settings = parsed; return TRUE;
}
static JsonObject *programme_view_object(const ProgrammeView *settings)
{
    JsonObject *object = json_object_new(); ProgrammeView copy = *settings;
    for (guint i = 0; i < G_N_ELEMENTS(programme_view_fields); ++i)
        json_object_set_int_member(object, programme_view_fields[i].key, *programme_view_slot(&copy, i));
    return object;
}
static gboolean programme_view_general(BroadcastView *view)
{ return !view->queue_slot && general_mode(view); }
static void programme_view_load(BroadcastView *view)
{
    ProgrammeView settings = PROGRAMME_VIEW_DEFAULTS; gboolean found = FALSE;
    JsonObject *config = view->programme_config ? json_node_get_object(view->programme_config) : NULL;
    if (programme_view_general(view)) {
        JsonObject *document = view->manual_general_config ? json_node_get_object(view->manual_general_config) : NULL;
        found = programme_view_read(catalog_object(catalog_object(document, "view_by_title"), radios_json_string(config, "title", "")), &settings);
    }
    if (!found) programme_view_read(catalog_object(config, "view"), &settings);
    view->view_settings = settings;
}
static gboolean programme_view_refresh_tick(gpointer data);
static void programme_view_apply(BroadcastView *view)
{
    if (!view->queue_table || view->disposed) return;
    const ProgrammeView *settings = &view->view_settings;
    radios_programme_queue_set_window(view->queue_table, settings->event_show, settings->content_show);
    radios_playout_ui_set_programme_window(programme_context(view), settings->event_fetch, settings->content_fetch);
    if (view->view_refresh_timer && view->view_refresh_seconds == settings->refresh_seconds) return;
    if (view->view_refresh_timer) g_source_remove(view->view_refresh_timer);
    view->view_refresh_seconds = settings->refresh_seconds;
    view->view_refresh_timer = g_timeout_add_seconds(settings->refresh_seconds, programme_view_refresh_tick, view);
}
/* Poll saved changes without turning a timer into an operator reload.
 * Progress/completion already move the visible window in the playout tick;
 * rebuilding unchanged rows would also release a held push-to-talk button. */
static gboolean programme_view_refresh_tick(gpointer data)
{
    BroadcastView *view = data;
    if (view->disposed) { view->view_refresh_timer = 0; return G_SOURCE_REMOVE; }
    if (view->programme_document && !view->catalogue_busy)
        radios_data_document_refresh(view->programme_document);
    return G_SOURCE_CONTINUE;
}
static void programme_view_status(GtkWidget *dialog, const gchar *message)
{ GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "programme-view-status"); if (status) gtk_label_set_text(GTK_LABEL(status), message); }
/* The counts apply to this session at once; the record follows when the
 * programme can be saved (the service may still reject the new fields). */
static gboolean programme_view_save(BroadcastView *view, const ProgrammeView *settings, gchar **message)
{
    view->view_settings = *settings; programme_view_apply(view);
    const gchar *title = radios_json_string(json_node_get_object(view->programme_config), "title", "");
    if (programme_view_general(view)) {
        /* The session copy of the "Mevcut program" document keeps the setting
         * even when the service refuses it, so later renders find it. */
        JsonNode *node = view->manual_general_config ? programme_copy(view->manual_general_config) : queue_empty(0);
        JsonObject *document = json_node_get_object(node), *previous = catalog_object(document, "view_by_title");
        JsonObject *table = previous ? radios_programme_clone_object(previous) : json_object_new();
        json_object_remove_member(table, title);
        while (json_object_get_size(table) >= 64) {
            GList *members = json_object_get_members(table);
            if (!members) break;
            json_object_remove_member(table, members->data); g_list_free(members);
        }
        json_object_set_object_member(table, title, programme_view_object(settings));
        json_object_set_object_member(document, "view_by_title", table);
        g_clear_pointer(&view->manual_general_config, json_node_free); view->manual_general_config = node;
        if (!view->queues[0].document || !radios_data_document_save(view->queues[0].document, node)) {
            *message = g_strdup("Görünüm ayarı bu oturumda uygulandı; genel yayın sırası kaydı gönderilemedi."); return FALSE;
        }
        *message = g_strdup_printf("Görünüm ayarı uygulandı; \"%s\" için kaydediliyor.", title); return TRUE;
    }
    /* The draft carries the setting locally when the record cannot be saved;
     * the next accepted programme save then includes it. */
    JsonNode *next = programme_copy(view->programme_config);
    json_object_set_object_member(json_node_get_object(next), "view", programme_view_object(settings));
    gboolean ready = programme_can_change(view), saved = ready && catalogue_save(view, programme_copy(next));
    if (saved) json_node_free(next);
    else { g_clear_pointer(&view->programme_config, json_node_free); view->programme_config = next; }
    *message = g_strdup(saved ? "Görünüm ayarı uygulandı ve programa kaydediliyor." : ready ?
        "Görünüm ayarı bu oturumda uygulandı; program kaydı gönderilemedi." :
        "Görünüm ayarı bu oturumda uygulandı; program kaydı hazır olmadığından kaydedilmedi.");
    return saved;
}
static void programme_view_dialog_response(GtkDialog *dialog, gint response, gpointer data)
{
    GtkWidget *owner = data; BroadcastView *view = g_object_get_data(G_OBJECT(owner), "radios-broadcast-view");
    if (response != GTK_RESPONSE_ACCEPT || !view || view->disposed) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    ProgrammeView settings = PROGRAMME_VIEW_DEFAULTS;
    for (guint i = 0; i < G_N_ELEMENTS(programme_view_fields); ++i) {
        GtkSpinButton *spin = g_object_get_data(G_OBJECT(dialog), programme_view_fields[i].id);
        gtk_spin_button_update(spin);
        *programme_view_slot(&settings, i) = (guint)MAX(0, gtk_spin_button_get_value_as_int(spin));
    }
    if (settings.event_fetch < settings.event_show) {
        programme_view_status(GTK_WIDGET(dialog), "Event çekme adedi, event gösterme adedinden küçük olamaz; kaydedilmedi."); return;
    }
    if (settings.content_fetch < settings.content_show) {
        programme_view_status(GTK_WIDGET(dialog), "İçerik çekme adedi, içerik gösterme adedinden küçük olamaz; kaydedilmedi."); return;
    }
    if (view->queue_slot + 1 != GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(dialog), "programme-view-slot"))) {
        programme_view_status(GTK_WIDGET(dialog), "Yayın sırası değişti; ayarları yeniden açın."); return;
    }
    gchar *message = NULL; programme_view_save(view, &settings, &message);
    programme_notice(view, message); g_free(message);
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void programme_view_settings_clicked(GtkButton *control, gpointer data)
{
    (void)control; BroadcastView *view = data; if (view->disposed) return;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Yayın sırası görünümü", view->ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(dialog, "programme-view-settings-dialog"); add_class(dialog, "radios"); add_class(dialog, "programme-dialog");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 300);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_box_set_spacing(GTK_BOX(box), 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(grid), 12); gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    ProgrammeView current = view->view_settings;
    for (guint i = 0; i < G_N_ELEMENTS(programme_view_fields); ++i) {
        GtkWidget *label = radios_ui_label(programme_view_fields[i].label, "form-label");
        GtkWidget *spin = gtk_spin_button_new_with_range(programme_view_fields[i].low, programme_view_fields[i].high, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), *programme_view_slot(&current, i));
        /* radios_ui_label wraps by default; a wrapping grid label asks for its
         * height at the narrowest width and leaves the dialog half empty. */
        gtk_label_set_line_wrap(GTK_LABEL(label), FALSE);
        gtk_widget_set_name(spin, programme_view_fields[i].id); gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_mnemonic_widget(GTK_LABEL(label), spin); gtk_widget_set_hexpand(label, TRUE);
        gtk_grid_attach(GTK_GRID(grid), label, 0, (gint)i, 1, 1); gtk_grid_attach(GTK_GRID(grid), spin, 1, (gint)i, 1, 1);
        g_object_set_data(G_OBJECT(dialog), programme_view_fields[i].id, spin);
    }
    pack(box, grid, FALSE);
    GtkWidget *status = radios_ui_label("", "ld-status"); gtk_widget_set_name(status, "programme-view-status");
    gtk_label_set_line_wrap(GTK_LABEL(status), TRUE); gtk_label_set_xalign(GTK_LABEL(status), 0); pack(box, status, FALSE);
    g_object_set_data(G_OBJECT(dialog), "programme-view-status", status);
    g_object_set_data(G_OBJECT(dialog), "programme-view-slot", GUINT_TO_POINTER(view->queue_slot + 1));
    g_signal_connect_object(dialog, "response", G_CALLBACK(programme_view_dialog_response), view->page, 0);
    g_signal_connect_object(view->page, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    gtk_widget_show_all(dialog);
}

/* ---- Flow index: events, contents and live bindings of the shown programme */
static void flow_block_free(gpointer value)
{ FlowBlock *block = value; g_free(block->id); g_free(block->title); g_free(block->last_leaf); if (block->block) json_object_unref(block->block); g_free(block); }
static void flow_leaf_free(gpointer value) { FlowLeaf *leaf = value; g_free(leaf->title); g_free(leaf); }
static void live_block_free(gpointer value)
{ LiveBlock *block = value; g_free(block->id); if (block->block) json_object_unref(block->block); if (block->playback) json_object_unref(block->playback); g_free(block); }
static void live_step_free(gpointer value) { LiveStep *step = value; g_free(step->end_rule); g_free(step->previous); g_free(step->next); g_free(step); }
static void flow_index_clear(BroadcastView *view)
{
    g_clear_pointer(&view->flow_leaves, g_hash_table_unref); g_clear_pointer(&view->flow_blocks, g_hash_table_unref);
    g_clear_pointer(&view->live_blocks, g_ptr_array_unref);
}
static LiveBlock *live_block_find(GPtrArray *blocks, const gchar *id)
{
    for (guint i = 0; blocks && id && i < blocks->len; ++i) {
        LiveBlock *block = g_ptr_array_index(blocks, i); if (g_str_equal(block->id, id)) return block;
    }
    return NULL;
}
/* The live binding of an event. In the calendar projection an operator
 * override (its materialized programme's first block) wins over the template. */
static JsonObject *live_source_block(BroadcastView *view, RadioSProgrammeBlock *block, JsonObject **playback)
{
    JsonObject *override = !view->queue_slot ? g_hash_table_lookup(view->event_overrides, radios_json_string(block->block, "id", "")) : NULL;
    JsonArray *blocks = catalog_array(catalog_object(override, "programme"), "blocks");
    *playback = catalog_object(override ? override : block->block, "playback");
    return blocks && json_array_get_length(blocks) ? json_array_get_object_element(blocks, 0) : block->block;
}
static void flow_index_ensure(BroadcastView *view)
{
    if (view->flow_leaves && view->flow_index_serial == view->config_serial) return;
    flow_index_clear(view); view->feed_dirty = TRUE;
    view->flow_blocks = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, flow_block_free);
    view->flow_leaves = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, flow_leaf_free);
    view->live_blocks = g_ptr_array_new_with_free_func(live_block_free);
    RadioSProgrammeTree *tree = view->programme_config ? programme_tree(view) : NULL;
    for (guint i = 0; tree && i < tree->blocks->len; ++i) {
        RadioSProgrammeBlock *block = g_ptr_array_index(tree->blocks, i);
        FlowBlock *entry = g_new0(FlowBlock, 1);
        entry->id = g_strdup(radios_json_string(block->block, "id", "")); entry->title = g_strdup(radios_json_string(block->block, "title", ""));
        entry->duration_ms = radios_programme_block_duration(block); entry->block = json_object_ref(block->block);
        if (block->leaves->len) entry->last_leaf = g_strdup(radios_json_string(g_ptr_array_index(block->leaves, block->leaves->len - 1), "id", ""));
        g_hash_table_replace(view->flow_blocks, entry->id, entry);
        for (guint j = 0; j < block->leaves->len; ++j) {
            JsonObject *content = g_ptr_array_index(block->leaves, j); FlowLeaf *leaf = g_new0(FlowLeaf, 1);
            leaf->block = entry; leaf->last = j + 1 == block->leaves->len; leaf->title = g_strdup(radios_json_string(content, "title", ""));
            leaf->duration_ms = MAX(0, radios_json_int(content, "duration_ms", 0));
            g_hash_table_replace(view->flow_leaves, g_strdup(radios_json_string(content, "id", "")), leaf);
        }
        JsonObject *playback = NULL, *source = live_source_block(view, block, &playback);
        if (!radios_live_control_available(source)) continue;
        LiveBlock *live = g_new0(LiveBlock, 1);
        live->id = g_strdup(entry->id); live->block = json_object_ref(source); live->playback = playback ? json_object_ref(playback) : NULL;
        g_ptr_array_add(view->live_blocks, live);
    }
    if (tree) radios_programme_tree_free(tree);
    view->flow_index_serial = view->config_serial;
}
static void live_step_record(BroadcastView *view, JsonObject *previous, JsonObject *hint, JsonObject *next)
{
    const gchar *block = radios_json_string(hint, "block_id", ""); if (!*block) return;
    LiveStep *step = g_new0(LiveStep, 1);
    step->end_rule = g_strdup(radios_json_string(hint, "end_rule", "schema")); step->duration_ms = radios_json_int(hint, "duration_ms", 0);
    step->previous = g_strdup(radios_json_string(previous, "entry_id", ""));
    step->next = g_strdup(radios_json_string(next, "entry_id", ""));
    /* The accepted timeline places the next row after the step's wait; the
     * wait itself runs from the previous row's actual end (playout_ui.c). */
    step->bound_utc_us = radios_json_int(next, "planned_utc_us", 0);
    g_hash_table_replace(view->live_steps, g_strdup(block), step);
}
/* The flow holds at a live step: both end rules wait in MANUAL_WAIT from
 * the previous row's actual end (duration: until live_wait_until_utc_us,
 * manual: until L closes or ▶|). */
static gboolean live_step_waiting(const RadioSPlayoutSnapshot *snapshot)
{
    return snapshot->state == RADIOS_PLAYOUT_WAITING || snapshot->state == RADIOS_PLAYOUT_MANUAL_WAIT ||
        snapshot->state == RADIOS_PLAYOUT_SCHEDULED;
}
static gboolean live_step_at(const LiveStep *step, const RadioSPlayoutSnapshot *snapshot)
{ return !g_strcmp0(snapshot->entry_id, step->previous) || !g_strcmp0(snapshot->entry_id, step->next); }
/* A-19: a duration step approaches its end when its runtime wait
 * (live_wait_until_utc_us) is near; the flow moving on clears the warning.
 * Without a transition region of its own a talk step warns over its last
 * ten seconds. "manual" waits for L. */
#define LIVE_STEP_ENDING_US (10 * G_USEC_PER_SEC)
static const gchar *live_step_ending(BroadcastView *view, const RadioSPlayoutSnapshot *snapshot, const gchar *ending)
{
    if (!view->live_steps || !live_step_waiting(snapshot) || snapshot->live_wait_until_utc_us <= 0) return ending;
    gint64 left = snapshot->live_wait_until_utc_us - g_get_real_time(), zone = MAX((gint64)snapshot->overlap_ms * 1000, (gint64)LIVE_STEP_ENDING_US);
    GHashTableIter iterator; gpointer key, value; g_hash_table_iter_init(&iterator, view->live_steps);
    while (g_hash_table_iter_next(&iterator, &key, &value)) {
        LiveStep *step = value;
        if (live_step_at(step, snapshot) && g_str_equal(step->end_rule, "duration") && left > 0 && left <= zone) return key;
    }
    return ending;
}
/* The live step on air: the flow waits at it (R is pressable there). */
static const gchar *live_step_current(BroadcastView *view, const RadioSPlayoutSnapshot *snapshot)
{
    if (!view->live_steps || !live_step_waiting(snapshot)) return NULL;
    GHashTableIter iter; gpointer key, value; g_hash_table_iter_init(&iter, view->live_steps);
    while (g_hash_table_iter_next(&iter, &key, &value))
        if (live_step_at(value, snapshot)) return key;
    return NULL;
}
/* L lit while the event's live flow is active, R shown held while pressed;
 * the row's subtitle names the label that decides L now (bound strips). */
static void live_rows_sync(BroadcastView *view, gboolean force)
{
    static const gchar *const label_ids[] = {NULL, "dj", "talk", "music"};
    for (guint i = 0; view->live_blocks && i < view->live_blocks->len; ++i) {
        LiveBlock *live = g_ptr_array_index(view->live_blocks, i); gboolean active = FALSE, held = FALSE;
        RadioSLiveLabel label = radios_live_control_effective_label(view->ctx, live->block);
        if (force || !live->label_known || label != live->label) {
            live->label_known = TRUE; live->label = label;
            radios_programme_queue_set_live_label(view->queue_table, live->id,
                label == radios_live_control_label(live->block) ? NULL : label_ids[label]);
        }
        radios_live_control_state(view->ctx, live->block, &active, &held);
        if (!force && live->known && live->active == active && live->held == held) continue;
        live->known = TRUE; live->active = active; live->held = held;
        radios_programme_queue_set_live(view->queue_table, live->id, active, held);
    }
}
static gboolean event_run_active(BroadcastView *view, const gchar *event_id)
{ return view->event_run_origin && event_id && g_str_equal(view->event_run_origin, event_id); }
/* Every runner tick: S/G transition, end-zone blink, overlay state and live
 * availability of the queue rows. Each setter is called only on a change. */
static void programme_flow_feed(BroadcastView *view, const RadioSPlayoutSnapshot *snapshot)
{
    GtkWidget *table = view->queue_table; flow_index_ensure(view);
    gboolean force = view->feed_dirty; view->feed_dirty = FALSE;
    gboolean sounding = snapshot->state == RADIOS_PLAYOUT_PLAYING || snapshot->state == RADIOS_PLAYOUT_OVERLAP || snapshot->state == RADIOS_PLAYOUT_PAUSED;
    /* A-8/A-10: both rows carry the green G only while the overlap really runs. */
    const gchar *source = snapshot->state == RADIOS_PLAYOUT_OVERLAP && snapshot->entry_id[0] ? snapshot->entry_id : NULL;
    const gchar *target = source && snapshot->next_entry_id[0] ? snapshot->next_entry_id : NULL;
    if (force || g_strcmp0(source, view->transition_source) || g_strcmp0(target, view->transition_target)) {
        g_free(view->transition_source); g_free(view->transition_target);
        view->transition_source = g_strdup(source); view->transition_target = g_strdup(target);
        radios_programme_queue_set_transition(table, source, target);
    }
    /* A-9: only the active event blinks in its accepted transition region. */
    FlowLeaf *leaf = sounding && snapshot->entry_id[0] ? g_hash_table_lookup(view->flow_leaves, snapshot->entry_id) : NULL;
    const gchar *ending = leaf && leaf->last && snapshot->ending ? leaf->block->id : NULL;
    ending = live_step_ending(view, snapshot, ending);
    if (force || g_strcmp0(ending, view->ending_event)) {
        if (view->ending_event && g_strcmp0(ending, view->ending_event)) radios_programme_queue_set_ending(table, view->ending_event, FALSE);
        g_free(view->ending_event); view->ending_event = g_strdup(ending);
        if (ending) radios_programme_queue_set_ending(table, ending, TRUE);
    }
    /* A-14/A-16: the sounding deck's planned jingle/sweeper for its content row. */
    RadioSPlayoutDeckContent content = {0}; const gchar *overlay_leaf = NULL, *overlay_title = "", *overlay_id = "";
    RadioSPlayoutContentState overlay_state = RADIOS_PLAYOUT_CONTENT_NONE;
    if (sounding && snapshot->entry_id[0] && snapshot->deck >= 0 && snapshot->deck < 32 &&
        radios_playout_ui_deck_content(programme_context(view), (guint)snapshot->deck, &content) && !content.auxiliary &&
        content.overlay_state != RADIOS_PLAYOUT_CONTENT_NONE) {
        overlay_leaf = snapshot->entry_id; overlay_title = content.overlay_title; overlay_id = content.overlay_id; overlay_state = content.overlay_state;
    }
    if (force || g_strcmp0(overlay_leaf, view->overlay_leaf) || overlay_state != view->overlay_state ||
        g_strcmp0(overlay_id, view->overlay_id) ||
        g_strcmp0(overlay_title, view->overlay_title ? view->overlay_title : "")) {
        if (view->overlay_leaf && (g_strcmp0(overlay_leaf, view->overlay_leaf) || g_strcmp0(overlay_id, view->overlay_id)))
            radios_programme_queue_set_overlay_identity(table, view->overlay_leaf, view->overlay_id, NULL, RADIOS_PLAYOUT_CONTENT_NONE);
        g_free(view->overlay_leaf); g_free(view->overlay_title); g_free(view->overlay_id);
        view->overlay_leaf = g_strdup(overlay_leaf); view->overlay_title = g_strdup(overlay_title); view->overlay_state = overlay_state;
        view->overlay_id = g_strdup(overlay_id);
        if (overlay_leaf) radios_programme_queue_set_overlay_identity(table, overlay_leaf, overlay_id, overlay_title, overlay_state);
    }
    /* A-3 (PDF s.15): R is pressable on the event on air — the live step the
     * flow waits at, else the event of the sounding content — and passive on
     * every other row. */
    const gchar *step_id = live_step_current(view, snapshot);
    gboolean airing = snapshot->state == RADIOS_PLAYOUT_PLAYING || snapshot->state == RADIOS_PLAYOUT_OVERLAP;
    const gchar *talk = step_id ? step_id : leaf && airing ? leaf->block->id : NULL;
    if (force || g_strcmp0(talk, view->talk_event)) {
        g_free(view->talk_event); view->talk_event = g_strdup(talk); radios_programme_queue_set_talk_available(table, talk);
    }
    /* A-19 (PDF s.6): the outer order left a live step — its wait ended and
     * the flow goes on, also through a new plan's preparation when the step
     * closes a planning window — so the step is complete and its own schema
     * ends with it. A stopped flow does not complete the step. */
    gboolean going = snapshot->state != RADIOS_PLAYOUT_IDLE && snapshot->state != RADIOS_PLAYOUT_FINISHED && snapshot->state != RADIOS_PLAYOUT_ERROR;
    if (view->live_step_on_air && g_strcmp0(step_id, view->live_step_on_air) && going) {
        live_step_finished(view, view->live_step_on_air); radios_programme_queue_complete_live_step(table, view->live_step_on_air);
        if (!view->live_steps_done) view->live_steps_done = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_hash_table_add(view->live_steps_done, g_strdup(view->live_step_on_air));
    }
    /* A completed step's schema stays ended: a run of it that is still (or
     * again) sounding while the flow plays on is stopped once more. */
    if (view->live_steps_done && view->event_run_origin && g_hash_table_contains(view->live_steps_done, view->event_run_origin) &&
        g_get_monotonic_time() - view->live_stop_retry_us > 500 * G_TIME_SPAN_MILLISECOND) {
        view->live_stop_retry_us = g_get_monotonic_time(); live_event_run_stop(view, view->event_run_origin);
    }
    if (view->live_steps_done && (snapshot->state == RADIOS_PLAYOUT_IDLE || snapshot->state == RADIOS_PLAYOUT_FINISHED))
        g_hash_table_remove_all(view->live_steps_done);
    /* PDF s.8: the step the flow waits at reads as the event on air. */
    if (force || g_strcmp0(step_id, view->live_step_on_air)) {
        if (g_strcmp0(step_id, view->live_step_on_air)) { g_free(view->live_step_on_air); view->live_step_on_air = g_strdup(step_id); }
        radios_programme_queue_set_live_step(table, step_id);
    }
    live_rows_sync(view, force);
}
/* L and R of an event row (A-1/A-2/A-3). Switching L off and releasing R
 * never produce a transport command: the flow advances by its end rule. */
/* ---- Live event schema (A-2: "Müzik → evente atanmış otomasyon şeması";
 * "Konuşma → özel live event şeması"). live.automation_id names a schema of
 * the calendar (4c contract); it is prepared from now for the step's
 * duration (one hour when open) and runs as this event's independent Yarı
 * AutoDJ playback: the broadcast flow itself is never replaced. */
typedef struct { BroadcastView *view; gchar *event_id; JsonObject *playback; GCancellable *cancel; } LiveSchemaJob;
static void live_schema_job_free(gpointer value)
{ LiveSchemaJob *job = value; g_free(job->event_id); if (job->playback) json_object_unref(job->playback); g_free(job); }
static void live_schema_prepared(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *programme, const GError *error, gpointer value)
{
    (void)ctx; LiveSchemaJob *job = value; BroadcastView *view = job->view;
    if (view->disposed) return;
    if (view->live_schema_jobs && g_hash_table_lookup(view->live_schema_jobs, job->event_id) == job->cancel)
        g_hash_table_remove(view->live_schema_jobs, job->event_id);
    if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return;
    /* The step ended while its schema was being prepared: nothing starts. */
    if (view->live_steps_done && g_hash_table_contains(view->live_steps_done, job->event_id)) return;
    if (error || !programme) {
        gchar *message = g_strdup_printf("Eventin otomasyon şeması başlatılamadı: %s", error ? error->message : "şema hazırlanamadı");
        programme_notice(view, message); g_free(message); return;
    }
    gint64 visible = radios_json_int(job->playback, "player", 0); GError *failure = NULL;
    gboolean accepted = radios_event_playout_programme_for_event(view->ctx, owner, programme,
        visible > 0 && visible <= 32 ? (guint)visible - 1 : G_MAXUINT,
        radios_json_string(job->playback, "next_cluster_id", ""), job->event_id, &failure);
    programme_notice(view, accepted ? "Eventin otomasyon şeması kümesiz bir oynatıcıda Yarı AutoDJ ile başladı; yayın sırası korunuyor." :
        failure ? failure->message : "Eventin otomasyon şeması başlatılamadı.");
    g_clear_error(&failure);
}
/* An unfinished independent run of the event exists, whatever its state
 * (a preparing run is not on air yet but must not be started twice). */
static gboolean event_run_unfinished(BroadcastView *view, const gchar *event_id)
{
    if (event_run_active(view, event_id)) return TRUE;
    JsonNode *snapshot = radios_event_playout_display_snapshot(view->ctx); gboolean found = FALSE;
    JsonArray *runs = snapshot && JSON_NODE_HOLDS_OBJECT(snapshot) ? catalog_array(json_node_get_object(snapshot), "runs") : NULL;
    for (guint i = 0; runs && i < json_array_get_length(runs) && !found; ++i) {
        JsonObject *run = json_array_get_object_element(runs, i); JsonNode *completed = json_object_get_member(run, "completed");
        gint64 state = radios_json_int(run, "state", RADIOS_PLAYOUT_IDLE);
        found = !g_strcmp0(radios_json_string(run, "origin_block_id", ""), event_id) &&
            !(completed && JSON_NODE_HOLDS_VALUE(completed) && json_node_get_boolean(completed)) &&
            state != RADIOS_PLAYOUT_IDLE && state != RADIOS_PLAYOUT_FINISHED && state != RADIOS_PLAYOUT_ERROR;
    }
    g_clear_pointer(&snapshot, json_node_free); return found;
}
/* TRUE when the event carries a schema (its start is then handled here). */
static gboolean live_schema_start(BroadcastView *view, LiveBlock *live, const gchar *event_id)
{
    JsonObject *step = catalog_object(live->block, "live");
    const gchar *schema = radios_json_string(step, "automation_id", "");
    if (!*schema) return FALSE;
    if (!view->live_schema_jobs) view->live_schema_jobs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    if (event_run_unfinished(view, event_id) || g_hash_table_contains(view->live_schema_jobs, event_id)) {
        programme_notice(view, "Eventin otomasyon şeması zaten yayında ya da hazırlanıyor."); return TRUE;
    }
    LiveSchemaJob *job = g_new0(LiveSchemaJob, 1); job->view = view; job->event_id = g_strdup(event_id);
    job->playback = live->playback ? radios_programme_clone_object(live->playback) : NULL;
    GCancellable *cancel = radios_local_event_prepare_schema_async(view->ctx, view->page, schema,
        radios_json_int(step, "duration_ms", 0), live_schema_prepared, job, live_schema_job_free);
    if (!cancel) { programme_notice(view, "Eventin otomasyon şeması başlatılamadı."); return TRUE; }
    job->cancel = cancel; g_hash_table_replace(view->live_schema_jobs, g_strdup(event_id), cancel);
    programme_notice(view, "Eventin otomasyon şeması hazırlanıyor.");
    return TRUE;
}
static void live_schema_cancel(BroadcastView *view, const gchar *event_id)
{
    GCancellable *cancel = view->live_schema_jobs ? g_hash_table_lookup(view->live_schema_jobs, event_id) : NULL;
    if (!cancel) return;
    g_cancellable_cancel(cancel); g_hash_table_remove(view->live_schema_jobs, event_id);
}
/* Ends the event's own independent run (its live schema) when the operator
 * closes a manual live step; the flow itself is untouched by this stop. */
static void live_event_run_stop(BroadcastView *view, const gchar *event_id)
{
    JsonNode *snapshot = radios_event_playout_display_snapshot(view->ctx);
    JsonObject *data = snapshot && JSON_NODE_HOLDS_OBJECT(snapshot) ? json_node_get_object(snapshot) : NULL;
    /* Every unfinished run of the event (an earlier, completed one may be
     * listed first) is cancelled and its player stopped. */
    JsonArray *runs = catalog_array(data, "runs"); guint32 sources = 0;
    for (guint i = 0; runs && i < json_array_get_length(runs); ++i) {
        JsonObject *run = json_array_get_object_element(runs, i);
        JsonNode *completed = json_object_get_member(run, "completed"); gint64 source = radios_json_int(run, "source", -1);
        if (g_strcmp0(radios_json_string(run, "origin_block_id", ""), event_id) || source < 0 || source >= 32 ||
            (completed && JSON_NODE_HOLDS_VALUE(completed) && json_node_get_boolean(completed))) continue;
        sources |= 1u << source;
    }
    g_clear_pointer(&snapshot, json_node_free);
    for (guint source = 0; source < 32; ++source) {
        if (!(sources & (1u << source))) continue;
        radios_event_playout_cancel(view->ctx, source);
        RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = source}; GError *error = NULL;
        if (!radios_player_scope_execution_action(radios_player_scope_execution_get(view->ctx), &scope, RADIOS_PLAYOUT_STOP, &error))
            programme_notice(view, error ? error->message : "Eventin şeması durdurulamadı.");
        g_clear_error(&error);
    }
}
/* A live step is over (the outer order continued): its pending schema
 * preparation is dropped and its own schema run, if any, stops. */
static void live_step_finished(BroadcastView *view, const gchar *event_id)
{
    live_schema_cancel(view, event_id);
    if (event_run_active(view, event_id)) live_event_run_stop(view, event_id);
}
/* R on an event without a DJ/Konuşma binding: the studio microphone while
 * held (engine push-to-talk); release ends only this momentary talk. */
static void plain_talk(BroadcastView *view, const gchar *event_id, gboolean pressed)
{
    GError *error = NULL;
    if (pressed) {
        if (view->plain_talk) return;
        if (!radios_audio_ui_microphone_ptt(view->ctx, TRUE, &error)) {
            gchar *message = g_strdup_printf("Bas-konuş açılamadı: %s", error ? error->message : "mikrofon hazır değil");
            programme_notice(view, message); g_free(message); g_clear_error(&error); return;
        }
        view->plain_talk = g_strdup(event_id); radios_programme_queue_set_live(view->queue_table, event_id, FALSE, TRUE);
        return;
    }
    if (!view->plain_talk) return;
    if (!radios_audio_ui_microphone_ptt(view->ctx, FALSE, &error)) programme_notice(view, error ? error->message : "Bas-konuş kapatılamadı.");
    g_clear_error(&error);
    radios_programme_queue_set_live(view->queue_table, view->plain_talk, FALSE, FALSE); g_clear_pointer(&view->plain_talk, g_free);
}
static void programme_live_action(BroadcastView *view, const gchar *event_id, RadioSProgrammeRowAction action)
{
    flow_index_ensure(view); LiveBlock *live = live_block_find(view->live_blocks, event_id); GError *error = NULL;
    /* The bound strips' label decides (kılavuz s.4 adım 5, s.7). */
    RadioSLiveLabel label = live ? radios_live_control_effective_label(view->ctx, live->block) : RADIOS_LIVE_LABEL_NONE;
    if (action != RADIOS_PROGRAMME_ROW_LIVE) {
        gboolean pressed = action == RADIOS_PROGRAMME_ROW_TALK_PRESS;
        if (pressed && g_strcmp0(view->talk_event, event_id)) {
            programme_notice(view, "Bas-konuş yalnız yayındaki event için kullanılabilir."); return;
        }
        /* A DJ/Konuşma event talks on its own channels; any other event on air
         * on the studio microphone. Release ends whichever is held. */
        if (!pressed) plain_talk(view, event_id, FALSE);
        if (label == RADIOS_LIVE_LABEL_DJ || label == RADIOS_LIVE_LABEL_TALK) {
            if (!radios_live_control_talk(view->ctx, live->block, pressed, &error) && pressed) {
                gchar *message = g_strdup_printf("Bas-konuş açılamadı: %s", error ? error->message : "mikrofon hazır değil");
                programme_notice(view, message); g_free(message);
            }
            g_clear_error(&error); live_rows_sync(view, FALSE); return;
        }
        if (pressed) plain_talk(view, event_id, TRUE);
        return;
    }
    if (!live) { programme_notice(view, "Bu event canlı bağlantı taşımıyor."); return; }
    if (label == RADIOS_LIVE_LABEL_MUSIC) {
        /* L on a music event starts its assigned automation (live.automation_id,
         * the 4c schema contract) on an independent player via the existing
         * event playout; a second press never cuts the sounding track. */
        if (event_run_unfinished(view, event_id)) { programme_notice(view, "Eventin otomasyonu yayında; L kapanışı çalan parçayı kesmez."); return; }
        if (live_schema_start(view, live, event_id)) return;
        /* Without an assigned schema the event's own contents play. */
        JsonObject *selection = live->playback ? radios_programme_clone_object(live->playback) : json_object_new();
        if (!live->playback) { json_object_set_int_member(selection, "player", 0); json_object_set_string_member(selection, "next_cluster_id", ""); }
        if (!programme_event_start(view, event_id, selection, &error)) programme_notice(view, error ? error->message : "Event otomasyonu başlatılamadı.");
        json_object_unref(selection); g_clear_error(&error); return;
    }
    gboolean active = FALSE, held = FALSE; radios_live_control_state(view->ctx, live->block, &active, &held);
    if (!radios_live_control_toggle(view->ctx, view->page, live->block, !active, &error)) {
        gchar *message = g_strdup_printf(active ? "Canlı kanal kapatılamadı: %s" : "Mikrofon açılamadı: %s", error ? error->message : "ses motoru hazır değil");
        programme_notice(view, message); g_free(message);
    }
    /* A-19: a live step with the "manual" end rule is complete when its L is
     * switched off while the flow waits at that step (executor MANUAL_WAIT,
     * nothing of the flow is sounding): the outer order continues with the
     * next playlist exactly like the ▶| transport. R release never does this. */
    if (!error && active) {
        live_schema_cancel(view, event_id);
        RadioSPlayoutSnapshot snapshot = {0};
        LiveStep *step = view->live_steps ? g_hash_table_lookup(view->live_steps, event_id) : NULL;
        if (step && g_str_equal(step->end_rule, "manual") && view->transport[4] &&
            radios_playout_ui_get_snapshot(programme_context(view), &snapshot) && snapshot.state == RADIOS_PLAYOUT_MANUAL_WAIT &&
            !g_strcmp0(live_step_current(view, &snapshot), event_id)) {
            /* The manual live step is over: its own schema ends with it and the
             * outer order continues (C playlist). */
            if (event_run_active(view, event_id)) live_event_run_stop(view, event_id);
            programme_transport(GTK_BUTTON(view->transport[4]), view);
        }
    }
    /* "Konuşma: özel live event şeması" — the mic stays open even when the
     * schema cannot start; the failure is reported, not rolled back. */
    if (!error && !active) live_schema_start(view, live, event_id);
    g_clear_error(&error); live_rows_sync(view, FALSE);
}

static gboolean programme_playout_tick(gpointer data)
{
    BroadcastView *view = data; if (view->disposed) return G_SOURCE_REMOVE;
    programme_event_runs_tick(view);
    RadioSPlayoutSnapshot snapshot = {0}; if (!radios_playout_ui_get_snapshot(programme_context(view), &snapshot)) return G_SOURCE_CONTINUE;
    programme_runtime_hints(view);
    /* Each row keeps the player its content really sounds on. */
    if (snapshot.entry_id[0] && snapshot.deck >= 0 && snapshot.deck < 32)
        radios_programme_queue_set_player(view->queue_table, snapshot.entry_id, (guint)snapshot.deck + 1);
    if (snapshot.next_entry_id[0] && snapshot.next_deck >= 0 && snapshot.next_deck < 32)
        radios_programme_queue_set_player(view->queue_table, snapshot.next_entry_id, (guint)snapshot.next_deck + 1);
    gboolean sounding = snapshot.state == RADIOS_PLAYOUT_PLAYING || snapshot.state == RADIOS_PLAYOUT_OVERLAP ||
        snapshot.state == RADIOS_PLAYOUT_PAUSED || (snapshot.state == RADIOS_PLAYOUT_WAITING && snapshot.deck >= 0 &&
        snapshot.duration_ms > 0 && snapshot.position_ms < snapshot.duration_ms);
    radios_programme_queue_set_progress(view->queue_table, sounding && snapshot.entry_id[0] ? snapshot.entry_id : NULL,
        sounding && snapshot.next_entry_id[0] ? snapshot.next_entry_id : NULL,
        snapshot.position_ms - MIN(snapshot.position_ms, snapshot.cue_in_ms),
        snapshot.duration_ms - MIN(snapshot.duration_ms, snapshot.cue_in_ms), snapshot.state == RADIOS_PLAYOUT_PAUSED);
    JsonArray *completed = radios_playout_ui_take_completed(programme_context(view));
    for (guint i = 0; i < json_array_get_length(completed); ++i)
        radios_programme_queue_complete(view->queue_table, json_array_get_string_element(completed, i));
    json_array_unref(completed);
    programme_flow_feed(view, &snapshot);
    gchar *text = snapshot.error[0] ? g_strdup(snapshot.error) : g_strdup_printf("%s · %u/%u · %s · %02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT,
        radios_playout_ui_state_name(snapshot.state), snapshot.total ? snapshot.index + 1 : 0, snapshot.total, snapshot.title,
        snapshot.position_ms / 60000, (snapshot.position_ms / 1000) % 60);
    gtk_label_set_text(GTK_LABEL(view->playout_status), text); g_free(text);
    gboolean active = snapshot.state != RADIOS_PLAYOUT_IDLE && snapshot.state != RADIOS_PLAYOUT_FINISHED && snapshot.state != RADIOS_PLAYOUT_ERROR;
    if (!view->queue_slot && general_controls(view)) {
        general_status_update(view);
        for (guint i = 0; i < 5; ++i) gtk_widget_set_sensitive(view->transport[i], radios_broadcast_navigation_available(view->ctx, i));
        RadioSPlayerWeeklySnapshot schedule = {0}; radios_player_weekly_get_snapshot(view->general_automation, &schedule);
        if (schedule.armed && !schedule.current_event_name[0]) gtk_label_set_text(GTK_LABEL(view->playout_status), gtk_label_get_text(GTK_LABEL(view->general_status)));
        return G_SOURCE_CONTINUE;
    }
    gtk_widget_set_sensitive(view->transport[2], snapshot.state == RADIOS_PLAYOUT_PLAYING || snapshot.state == RADIOS_PLAYOUT_OVERLAP);
    gtk_widget_set_sensitive(view->transport[3], active); gtk_widget_set_sensitive(view->transport[0], active && snapshot.index > 0);
    gtk_widget_set_sensitive(view->transport[4], active && snapshot.index + 1 < snapshot.total);
    gtk_widget_set_sensitive(view->transport[1], radios_data_connected(view->ctx) && snapshot.state != RADIOS_PLAYOUT_PREPARING);
    return G_SOURCE_CONTINUE;
}
static void programme_destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; BroadcastView *view = data;
    g_object_set_data(G_OBJECT(view->page), "radios-general-programme-driver", NULL);
    if (view->general_automation) { radios_player_weekly_free(view->general_automation); view->general_automation = NULL; }
    policy_submit(view);
    if (view->policy_document) radios_data_document_preserve_pending(view->policy_document);
    if (view->live_schema_jobs) {
        GHashTableIter jobs; gpointer cancel; g_hash_table_iter_init(&jobs, view->live_schema_jobs);
        while (g_hash_table_iter_next(&jobs, NULL, &cancel)) g_cancellable_cancel(cancel);
    }
    /* A push-to-talk held while the page goes away must not stay open: the
     * queue's own release can no longer reach this disposed view. */
    for (guint i = 0; view->live_blocks && i < view->live_blocks->len; ++i) {
        LiveBlock *live = g_ptr_array_index(view->live_blocks, i); gboolean active = FALSE, held = FALSE;
        if (radios_live_control_state(view->ctx, live->block, &active, &held) && held)
            radios_live_control_talk(view->ctx, live->block, FALSE, NULL);
    }
    if (view->plain_talk) radios_audio_ui_microphone_ptt(view->ctx, FALSE, NULL);
    view->disposed = TRUE;
    if (view->local_event_cancel) g_cancellable_cancel(view->local_event_cancel);
    if (view->catalog_cancel) g_cancellable_cancel(view->catalog_cancel);
    if (view->playout_timer) { g_source_remove(view->playout_timer); view->playout_timer = 0; }
    if (view->view_refresh_timer) { g_source_remove(view->view_refresh_timer); view->view_refresh_timer = 0; }
    if (view->history_timer) { g_source_remove(view->history_timer); view->history_timer = 0; }
    if (view->history_dialog) gtk_widget_destroy(view->history_dialog);
    if (view->queue_timer) { g_source_remove(view->queue_timer); view->queue_timer = 0; }
    if (view->topology_timer) { g_source_remove(view->topology_timer); view->topology_timer = 0; }
    for (guint index = 1; index < G_N_ELEMENTS(view->queues); ++index) {
        radios_playout_ui_close(view->queues[index].context.playout);
        radios_playout_ui_unref(view->queues[index].context.playout); view->queues[index].context.playout = NULL;
    }
    g_object_set_data(G_OBJECT(view->ctx->parent), "radios-broadcast-context", NULL);
}

static const gchar *const programme_fields[] = {
    "Program adı", "Program tarihi", "Program notu", NULL
};
static const gchar *const event_fields[] = {
    "İçerik başlığı", "Sanatçı", "Albüm", "Öğe türü", "Başlangıç saati",
    "Süre (mm:ss)", "İçerik sürümü", "Hazırlık durumu", "Ana sistem dosyası",
    "Yedek sistem dosyası", "Öğe notu", NULL
};
static const gchar *const timing_fields[] = {
    "Hedef olay", "Program başlangıcı (UTC)", "Hedef saat (UTC)",
    "Hedef saatte davranış", "En fazla bekleme (saniye)", NULL
};

typedef struct {
    RadioSViewContext *ctx;
    gchar *title;
    const gchar *const *fields;
} BroadcastForm;

static void broadcast_form_open(GtkButton *button, gpointer data);

static void
add_class(GtkWidget *widget, const gchar *name)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}
static void
remove_class(GtkWidget *widget, const gchar *name)
{
    gtk_style_context_remove_class(gtk_widget_get_style_context(widget), name);
}

static void
pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{
    gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0);
}

static GtkWidget *
button(GtkWidget *box, const gchar *text, const gchar *style,
       RadioSViewContext *ctx, const gchar *notice)
{
    GtkWidget *widget = radios_ui_button(text);
    if (style != NULL)
        add_class(widget, style);
    if (notice != NULL)
        radios_ui_bind_notice(widget, ctx, notice);
    pack(box, widget, FALSE);
    return widget;
}

static GtkWidget *
named_box(GtkOrientation orientation, gint spacing, const gchar *style)
{
    return radios_ui_box(orientation, spacing, style);
}

static GtkWidget *
form_entry(const gchar *placeholder, gint maximum, const gchar *pattern)
{
    GtkWidget *entry = radios_ui_entry(placeholder);
    gtk_entry_set_max_length(GTK_ENTRY(entry), maximum);
    g_object_set_data(G_OBJECT(entry), "broadcast-pattern", (gpointer)pattern);
    return entry;
}

static GtkWidget *
form_notes(gint maximum)
{
    GtkWidget *text = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_size_request(text, -1, 76);
    g_object_set_data(G_OBJECT(text), "broadcast-max-length", GINT_TO_POINTER(maximum));
    return text;
}

static void
editor_field(GtkWidget *dialog, GtkWidget *grid, guint index,
             const gchar *title, const gchar *id, GtkWidget *widget,
             gboolean required)
{
    GtkWidget *group = named_box(GTK_ORIENTATION_VERTICAL, 6, NULL);
    GtkWidget *label = radios_ui_label(title, "form-label");
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), widget);
    pack(group, label, FALSE);
    pack(group, widget, FALSE);
    gtk_widget_set_name(widget, id);
    gtk_widget_set_hexpand(group, TRUE);
    gtk_grid_attach(GTK_GRID(grid), group, (gint)(index % 2), (gint)(index / 2), 1, 1);
    g_object_set_data(G_OBJECT(widget), "broadcast-required", GINT_TO_POINTER(required));
    g_object_set_data(G_OBJECT(widget), "broadcast-field-title", (gpointer)title);
    g_object_set_data(G_OBJECT(dialog), id, widget);
    GPtrArray *fields = g_object_get_data(G_OBJECT(dialog), "broadcast-fields");
    g_ptr_array_add(fields, widget);
}

static gboolean
date_is_valid(const gchar *value)
{
    gint y, m, d, consumed = 0;
    return sscanf(value, "%4d-%2d-%2d%n", &y, &m, &d, &consumed) == 3 &&
           consumed == 10 && value[consumed] == '\0' && y >= 1 && y <= 9999 &&
           m >= 1 && m <= 12 && d >= 1 && d <= 31 &&
           g_date_valid_dmy((GDateDay)d, (GDateMonth)m, (GDateYear)y);
}

static void
broadcast_form_response(GtkDialog *dialog, gint response, gpointer data)
{
    RadioSViewContext *ctx = data;
    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }
    GtkLabel *status = g_object_get_data(G_OBJECT(dialog), "broadcast-status");
    GPtrArray *fields = g_object_get_data(G_OBJECT(dialog), "broadcast-fields");
    for (guint i = 0; i < fields->len; ++i) {
        GtkWidget *widget = g_ptr_array_index(fields, i);
        if (GTK_IS_TEXT_VIEW(widget)) {
            GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
            gint maximum = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "broadcast-max-length"));
            if (gtk_text_buffer_get_char_count(buffer) > maximum) {
                gtk_label_set_text(status, "Not alanı izin verilen uzunluğu aşıyor.");
                gtk_widget_grab_focus(widget);
                return;
            }
            continue;
        }
        if (!GTK_IS_ENTRY(widget))
            continue;
        const gchar *value = gtk_entry_get_text(GTK_ENTRY(widget));
        const gchar *pattern = g_object_get_data(G_OBJECT(widget), "broadcast-pattern");
        gboolean required = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "broadcast-required"));
        gchar *trimmed = g_strdup(value);
        g_strstrip(trimmed);
        gboolean valid = (!required || *trimmed != '\0') &&
                         (*trimmed == '\0' || pattern == NULL ||
                          (g_str_equal(pattern, "date") ? date_is_valid(trimmed) :
                           g_regex_match_simple(pattern, trimmed, 0, 0)));
        g_free(trimmed);
        if (!valid) {
            const gchar *title = g_object_get_data(G_OBJECT(widget), "broadcast-field-title");
            gchar *message = g_strdup_printf("%s alanını geçerli biçimde doldurun.", title);
            gtk_label_set_text(status, message);
            gtk_widget_grab_focus(widget);
            g_free(message);
            return;
        }
    }
    BroadcastView *view = broadcast_context(ctx);
    if (view && view->programme_document && radios_data_connected(ctx)) {
        if (!view->queue_slot && general_mode(view) && g_object_get_data(G_OBJECT(dialog), "programme-block-title")) {
            const gchar *id = g_object_get_data(G_OBJECT(dialog), "programme-block-id"); GError *error = NULL;
            JsonObject *programme = programme_event_configuration(view, id, &error);
            if (!programme) { gtk_label_set_text(status, error ? error->message : "Event bulunamadı."); g_clear_error(&error); return; }
            JsonObject *old = g_hash_table_lookup(view->event_overrides, id);
            JsonObject *override = old ? radios_programme_clone_object(old) : json_object_new();
            json_object_set_object_member(override, "programme", programme);
            json_object_set_object_member(override, "playback", programme_event_playback_value(GTK_WIDGET(dialog)));
            g_hash_table_replace(view->event_overrides, g_strdup(id), override);
            gtk_label_set_text(status, "Bu program için eventin oynatıcısı ve devam kümesi güncellendi; takvim korundu."); return;
        }
        if (!programme_can_change(view)) { gtk_label_set_text(status, "Program kaydı yüklenmeden kaydedilemez; alanlarınız korunuyor."); return; }
        JsonObject *config = json_node_get_object(view->programme_config);
        if (g_object_get_data(G_OBJECT(dialog), "programme-name")) {
            object_field(config, "title", GTK_WIDGET(dialog), "programme-name");
            object_field(config, "date", GTK_WIDGET(dialog), "programme-date");
            if (!*radios_json_string(config, "date", "")) {
                GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d");
                json_object_set_string_member(config, "date", date); g_free(date); g_date_time_unref(now);
            }
            object_field(config, "note", GTK_WIDGET(dialog), "programme-notes");
        } else if (g_object_get_data(G_OBJECT(dialog), "programme-item-title")) {
            JsonArray *events = json_object_get_array_member(config, "events");
            const gchar *id = g_object_get_data(G_OBJECT(dialog), "programme-event-id");
            if (!id) { g_object_set_data_full(G_OBJECT(dialog), "programme-event-id", g_uuid_string_random(), g_free);
                id = g_object_get_data(G_OBJECT(dialog), "programme-event-id"); }
            JsonObject *event = NULL;
            for (guint i = 0; i < json_array_get_length(events); ++i) {
                JsonObject *candidate = json_array_get_object_element(events, i);
                if (g_str_equal(radios_json_string(candidate, "id", ""), id)) { event = candidate; break; }
            }
            if (!event && g_object_get_data(G_OBJECT(dialog), "programme-existing-event")) {
                gtk_label_set_text(status, "Düzenlenen event güncel programda bulunamadı; girdileriniz korunuyor."); return;
            }
            gboolean fresh = !event;
            if (fresh) event = json_object_new();
            json_object_set_string_member(event, "id", id);
            object_field(event, "title", GTK_WIDGET(dialog), "programme-item-title");
            object_field(event, "artist", GTK_WIDGET(dialog), "programme-item-artist");
            object_field(event, "album", GTK_WIDGET(dialog), "programme-item-album");
            object_field(event, "start_time", GTK_WIDGET(dialog), "programme-item-time");
            object_field(event, "version_label", GTK_WIDGET(dialog), "programme-item-version");
            object_field(event, "note", GTK_WIDGET(dialog), "programme-item-notes");
            const gchar *const kinds[] = {"music", "announcement", "advertisement", "jingle", "program", "sweeper", "news", "effect", "voice_track", "other"};
            GtkWidget *type = g_object_get_data(G_OBJECT(dialog), "programme-item-type");
            gint kind = gtk_combo_box_get_active(GTK_COMBO_BOX(type));
            json_object_set_string_member(event, "kind", kinds[CLAMP(kind, 0, 9)]);
            gchar *duration = field_value(GTK_WIDGET(dialog), "programme-item-duration"); guint minutes = 0, seconds = 0;
            sscanf(duration, "%u:%u", &minutes, &seconds);
            const gchar *original = g_object_get_data(G_OBJECT(dialog), "programme-original-duration");
            gint64 *original_ms = g_object_get_data(G_OBJECT(dialog), "programme-original-duration-ms");
            json_object_set_int_member(event, "duration_ms", original_ms && g_strcmp0(duration, original) == 0 ?
                *original_ms : ((gint64)minutes * 60 + seconds) * 1000); g_free(duration);
            if (!json_object_has_member(event, "primary_file")) json_object_set_string_member(event, "primary_file", "");
            if (!json_object_has_member(event, "backup_file")) json_object_set_string_member(event, "backup_file", "");
            JsonObject *binding = g_object_get_data(G_OBJECT(dialog), "programme-media-binding");
            const gchar *binding_keys[] = {"media_id", "version_id", "media_file_id", "media_sha256"};
            if (binding) for (guint i = 0; i < G_N_ELEMENTS(binding_keys); ++i)
                json_object_set_string_member(event, binding_keys[i], radios_json_string(binding, binding_keys[i], ""));
            if (fresh) {
                RadioSProgrammeTree *tree = programme_tree(view);
                GPtrArray *leaves = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref); g_ptr_array_add(leaves, event);
                programme_place(tree, programme_target(view, tree), leaves, NULL, NULL); g_ptr_array_unref(leaves);
                if (programme_commit(view, tree, id)) gtk_label_set_text(status, "Program taslağı kaydediliyor; ses/yayın komutu gönderilmedi.");
                return;
            }
        } else if (g_object_get_data(G_OBJECT(dialog), "programme-block-title")) {
            const gchar *id = g_object_get_data(G_OBJECT(dialog), "programme-block-id");
            RadioSProgrammeTree *tree = programme_tree(view); guint block = 0; gint leaf = -1;
            if (!id || !radios_programme_tree_find(tree, id, &block, &leaf) || leaf >= 0) {
                radios_programme_tree_free(tree); gtk_label_set_text(status, "Düzenlenen event güncel programda bulunamadı; girdileriniz korunuyor."); return;
            }
            RadioSProgrammeBlock *owner = g_ptr_array_index(tree->blocks, block);
            JsonObject *edited = radios_programme_clone_object(owner->block);
            object_field(edited, "title", GTK_WIDGET(dialog), "programme-block-title");
            object_field(edited, "start_time", GTK_WIDGET(dialog), "programme-block-time");
            object_field(edited, "note", GTK_WIDGET(dialog), "programme-block-notes");
            const gchar *const kinds[] = {"music", "announcement", "advertisement", "jingle", "program", "sweeper", "news", "effect", "voice_track", "other"};
            gint kind = gtk_combo_box_get_active(GTK_COMBO_BOX(g_object_get_data(G_OBJECT(dialog), "programme-block-kind")));
            json_object_set_string_member(edited, "kind", kinds[CLAMP(kind, 0, 9)]);
            json_object_set_object_member(edited, "playback", programme_event_playback_value(GTK_WIDGET(dialog)));
            json_object_unref(owner->block); owner->block = edited;
            gtk_label_set_text(status, programme_commit(view, tree, id) ? "Kaydedildi." : "Program kaydı gönderilemedi; girdileriniz korunuyor.");
            return;
        } else if (g_object_get_data(G_OBJECT(dialog), "programme-timed-title")) {
            const gchar *target_id = g_object_get_data(G_OBJECT(dialog), "programme-timed-event-id");
            gboolean found = FALSE; JsonArray *events = json_object_get_array_member(config, "events");
            for (guint i = 0; target_id && i < json_array_get_length(events); ++i)
                found |= g_str_equal(target_id, radios_json_string(json_array_get_object_element(events, i), "id", ""));
            if (!found) { gtk_label_set_text(status, "Kesin saat için yayın sırasından mevcut bir öğe seçin."); return; }
            JsonObject *timing = json_object_new(); json_object_set_string_member(timing, "event_id", target_id);
            object_field(timing, "start_time", GTK_WIDGET(dialog), "programme-start-time");
            object_field(timing, "target_time", GTK_WIDGET(dialog), "programme-target-time");
            json_object_set_string_member(timing, "policy", gtk_combo_box_get_active(GTK_COMBO_BOX(g_object_get_data(G_OBJECT(dialog), "programme-timed-policy"))) == 0 ? "cut" : "finish");
            json_object_set_int_member(timing, "max_wait_ms", 1000 * gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_object_get_data(G_OBJECT(dialog), "programme-timed-wait"))));
            json_object_set_object_member(config, "timing", timing);
        } else { gtk_label_set_text(status, "Bu işlem için bir kayıt seçin; medya ve yayın iş akışları sonraki aşamalarda bağlanacak."); return; }
        radios_data_document_dirty(view->programme_document, TRUE);
        if (radios_data_document_save(view->programme_document, view->programme_config)) {
            gtk_label_set_text(status, "Program taslağı kaydediliyor; ses/yayın komutu gönderilmedi."); programme_render(view);
        }
        return;
    }
    gtk_label_set_text(status, "Veri servisi bağlı değil; değişiklik kaydedilmedi. Girdiğiniz alanlar korunuyor.");
    if (ctx->notify != NULL)
        ctx->notify(ctx->notify_data, "Veri servisi bağlı değil; değişiklik kaydedilmedi.");
}

static GtkWidget *
broadcast_dialog(RadioSViewContext *ctx, const gchar *title, const gchar *id,
                 const gchar *style, GtkWidget **grid)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(dialog, id);
    add_class(dialog, "radios");
    add_class(dialog, style);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 640, 580);
    GtkWidget *body = named_box(GTK_ORIENTATION_VERTICAL, 14, NULL);
    gtk_container_set_border_width(GTK_CONTAINER(body), 20);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), body);
    pack(gtk_dialog_get_content_area(GTK_DIALOG(dialog)), scroll, TRUE);
    *grid = gtk_grid_new();
    add_class(*grid, "programme-dialog-fields");
    gtk_grid_set_column_homogeneous(GTK_GRID(*grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(*grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(*grid), 12);
    pack(body, *grid, FALSE);
    GtkWidget *status = radios_ui_label("", "ld-status");
    pack(body, status, FALSE);
    g_object_set_data(G_OBJECT(dialog), "broadcast-status", status);
    g_object_set_data_full(G_OBJECT(dialog), "broadcast-fields", g_ptr_array_new(),
                           (GDestroyNotify)g_ptr_array_unref);
    g_signal_connect(dialog, "response", G_CALLBACK(broadcast_form_response), ctx);
    return dialog;
}

static void
broadcast_form_open(GtkButton *button_widget, gpointer data)
{
    BroadcastForm *form = data;
    GtkWidget *grid;
    GtkWidget *dialog;
    const gchar *time_pattern = "^(?:[01][0-9]|2[0-3]):[0-5][0-9]$";
    const gchar *duration_pattern = "^[0-9]{1,4}:[0-5][0-9]$";
    if (form->fields == programme_fields) {
        dialog = broadcast_dialog(form->ctx, form->title, "programme-editor", "programme-dialog", &grid);
        editor_field(dialog, grid, 0, "Program adı", "programme-name", form_entry(NULL, 80, NULL), TRUE);
        editor_field(dialog, grid, 1, "Program tarihi", "programme-date", form_entry("YYYY-AA-GG", 10, "date"), FALSE);
        editor_field(dialog, grid, 2, "Program notu", "programme-notes", form_notes(400), FALSE);
        BroadcastView *view = broadcast_context(form->ctx);
        if (view && view->programme_config) {
            JsonObject *config = json_node_get_object(view->programme_config);
            gtk_entry_set_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "programme-name")), radios_json_string(config, "title", ""));
            gtk_entry_set_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "programme-date")), radios_json_string(config, "date", ""));
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_object_get_data(G_OBJECT(dialog), "programme-notes"))), radios_json_string(config, "note", ""), -1);
        }
    } else if (form->fields == event_fields) {
        dialog = broadcast_dialog(form->ctx, form->title, "programme-item-editor", "programme-dialog", &grid);
        const gchar *const kinds[] = {"Müzik", "Anons", "Reklam", "Jingle", "Program", "Sweeper", "Haber", "Efekt", "Ses kaydı", "Diğer", NULL};
        const gchar *const states[] = {"Kontrol bekliyor", "Hazır · yerel beyan", "Dosya eksik",
                                      "Onay bekliyor", "Geçersiz reklam", "Kilitli", NULL};
        const gchar *const files[] = {"Bilinmiyor", "Mevcut · yerel beyan", "Eksik", NULL};
        const gchar *label = gtk_button_get_label(button_widget);
        gint kind = label != NULL && strstr(label, "Anons") != NULL ? 1 :
                    label != NULL && strstr(label, "Reklam") != NULL ? 2 : 0;
        editor_field(dialog, grid, 0, "İçerik başlığı", "programme-item-title", form_entry(NULL, 160, NULL), TRUE);
        editor_field(dialog, grid, 1, "Sanatçı", "programme-item-artist", form_entry(NULL, 120, NULL), FALSE);
        editor_field(dialog, grid, 2, "Albüm", "programme-item-album", form_entry(NULL, 120, NULL), FALSE);
        editor_field(dialog, grid, 3, "Öğe türü", "programme-item-type", radios_ui_combo(kinds, kind), TRUE);
        editor_field(dialog, grid, 4, "Başlangıç saati (UTC)", "programme-item-time", form_entry("SS:DD", 5, time_pattern), FALSE);
        editor_field(dialog, grid, 5, "Süre (mm:ss)", "programme-item-duration", form_entry("03:30", 7, duration_pattern), FALSE);
        editor_field(dialog, grid, 6, "İçerik sürümü", "programme-item-version", form_entry("v1", 32, NULL), TRUE);
        editor_field(dialog, grid, 7, "Hazırlık durumu", "programme-item-readiness", radios_ui_combo(states, 0), FALSE);
        editor_field(dialog, grid, 8, "Ana sistem dosyası", "programme-item-primary", radios_ui_combo(files, 0), FALSE);
        editor_field(dialog, grid, 9, "Yedek sistem dosyası", "programme-item-backup", radios_ui_combo(files, 0), FALSE);
        editor_field(dialog, grid, 10, "Öğe notu", "programme-item-notes", form_notes(300), FALSE);
        BroadcastView *view = broadcast_context(form->ctx); g_object_set_data(G_OBJECT(dialog), "programme-view", view);
        g_object_set_data_full(G_OBJECT(dialog), "programme-insert-before", g_strdup(g_object_get_data(G_OBJECT(button_widget), "programme-insert-before")), g_free);
        if (form->ctx->data) {
            GtkWidget *pick = radios_ui_button("Kütüphaneden seç"); gtk_widget_set_name(pick, "programme-item-media");
            gtk_grid_attach(GTK_GRID(grid), pick, 0, 6, 2, 1); g_signal_connect_object(pick, "clicked", G_CALLBACK(programme_media_open), dialog, 0);
        }
        const gchar *edit_id = g_object_get_data(G_OBJECT(button_widget), "programme-edit-event-id");
        if (edit_id && view && view->programme_config) {
            JsonArray *events = json_object_get_array_member(json_node_get_object(view->programme_config), "events");
            for (guint i = 0; events && i < json_array_get_length(events); ++i) {
                JsonObject *event = json_array_get_object_element(events, i);
                if (!g_str_equal(edit_id, radios_json_string(event, "id", ""))) continue;
                g_object_set_data_full(G_OBJECT(dialog), "programme-event-id", g_strdup(edit_id), g_free);
                g_object_set_data(G_OBJECT(dialog), "programme-existing-event", GINT_TO_POINTER(1));
                const gchar *keys[] = {"title", "artist", "album", "start_time", "version_label"};
                const gchar *field_ids[] = {"programme-item-title", "programme-item-artist", "programme-item-album", "programme-item-time", "programme-item-version"};
                for (guint j = 0; j < G_N_ELEMENTS(keys); ++j)
                    gtk_entry_set_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), field_ids[j])), radios_json_string(event, keys[j], ""));
                const gchar *kind_keys[] = {"music", "announcement", "advertisement", "jingle", "program", "sweeper", "news", "effect", "voice_track", "other"};
                for (guint j = 0; j < G_N_ELEMENTS(kind_keys); ++j)
                    if (g_str_equal(kind_keys[j], radios_json_string(event, "kind", "")))
                        gtk_combo_box_set_active(GTK_COMBO_BOX(g_object_get_data(G_OBJECT(dialog), "programme-item-type")), (gint)j);
                gint64 seconds = radios_json_int(event, "duration_ms", 0) / 1000;
                gchar *duration = g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, seconds / 60, seconds % 60);
                g_object_set_data_full(G_OBJECT(dialog), "programme-original-duration", g_strdup(duration), g_free);
                gint64 *original_ms = g_new(gint64, 1); *original_ms = radios_json_int(event, "duration_ms", 0);
                g_object_set_data_full(G_OBJECT(dialog), "programme-original-duration-ms", original_ms, g_free);
                gtk_entry_set_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "programme-item-duration")), duration); g_free(duration);
                if (json_object_has_member(event, "media_id")) gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(dialog), "programme-item-duration"), FALSE);
                gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_object_get_data(G_OBJECT(dialog), "programme-item-notes"))), radios_json_string(event, "note", ""), -1);
                gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(dialog), "broadcast-status")), "Seçili metadata event düzenleniyor; UUID korunur, ses dosyası doğrulanmaz.");
                break;
            }
        }
    } else if (form->fields == timing_fields) {
        dialog = broadcast_dialog(form->ctx, form->title, "programme-timing-editor", "programme-dialog", &grid);
        const gchar *const policies[] = {"Parçayı kes · hedef saatte gir", "Bitmesini bekle · sınırlı bekleme", NULL};
        editor_field(dialog, grid, 0, "Hedef olay", "programme-timed-title", form_entry(NULL, 80, NULL), TRUE);
        editor_field(dialog, grid, 1, "Program başlangıcı (UTC)", "programme-start-time", form_entry("SS:DD", 5, time_pattern), TRUE);
        editor_field(dialog, grid, 2, "Hedef saat (UTC)", "programme-target-time", form_entry("SS:DD", 5, time_pattern), TRUE);
        editor_field(dialog, grid, 3, "Hedef saatte davranış", "programme-timed-policy", radios_ui_combo(policies, 0), FALSE);
        GtkWidget *wait = gtk_spin_button_new_with_range(0, 600, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(wait), 30);
        editor_field(dialog, grid, 4, "En fazla bekleme (saniye)", "programme-timed-wait", wait, FALSE);
        BroadcastView *view = broadcast_context(form->ctx);
        if (view && view->programme_config) {
            JsonObject *config = json_node_get_object(view->programme_config);
            JsonObject *timing = json_object_has_member(config, "timing") ? json_object_get_object_member(config, "timing") : NULL;
            gint selected = programme_selected(view);
            if (selected >= 0) {
                JsonObject *event = json_array_get_object_element(json_object_get_array_member(config, "events"), (guint)selected);
                g_object_set_data_full(G_OBJECT(dialog), "programme-timed-event-id", g_strdup(radios_json_string(event, "id", "")), g_free);
                gtk_entry_set_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "programme-timed-title")), radios_json_string(event, "title", ""));
                gtk_editable_set_editable(GTK_EDITABLE(g_object_get_data(G_OBJECT(dialog), "programme-timed-title")), FALSE);
            }
            gtk_entry_set_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "programme-start-time")), radios_json_string(timing, "start_time", ""));
            gtk_entry_set_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "programme-target-time")), radios_json_string(timing, "target_time", ""));
            gtk_combo_box_set_active(GTK_COMBO_BOX(g_object_get_data(G_OBJECT(dialog), "programme-timed-policy")), g_str_equal(radios_json_string(timing, "policy", "cut"), "finish") ? 1 : 0);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(wait), radios_json_int(timing, "max_wait_ms", 30000) / 1000.0);
        }
    } else {
        gboolean playlist = strstr(form->title, "Playlist") != NULL;
        dialog = broadcast_dialog(form->ctx, form->title,
                                  playlist ? "playlist-track-editor" : "broadcast-catalog-editor",
                                  playlist ? "playlist-dialog" : "broadcast-catalog-dialog", &grid);
        if (playlist) {
            editor_field(dialog, grid, 0, "Parça adı", "playlist-track-title", form_entry(NULL, 160, NULL), TRUE);
            editor_field(dialog, grid, 1, "Sanatçı", "playlist-track-artist", form_entry(NULL, 120, NULL), FALSE);
            editor_field(dialog, grid, 2, "Albüm", "playlist-track-album", form_entry(NULL, 120, NULL), FALSE);
            editor_field(dialog, grid, 3, "Süre (dk:sn)", "playlist-track-duration", form_entry("03:30", 6, duration_pattern), FALSE);
        } else {
            const gchar *const kinds[] = {"Parça", "Jingle", "Anons", "Reklam", "Program", NULL};
            GtkWidget *kind = radios_ui_combo(kinds, 0);
            gtk_widget_set_sensitive(kind, FALSE);
            editor_field(dialog, grid, 0, "Tür", "broadcast-catalog-kind", kind, FALSE);
            editor_field(dialog, grid, 1, "Sürüm", "broadcast-catalog-version", form_entry("v1", 40, NULL), FALSE);
            editor_field(dialog, grid, 2, "Başlık", "broadcast-catalog-title", form_entry(NULL, 160, NULL), TRUE);
            editor_field(dialog, grid, 3, "Sanatçı / yayıncı", "broadcast-catalog-artist", form_entry(NULL, 120, NULL), FALSE);
            editor_field(dialog, grid, 4, "Albüm / içerik grubu", "broadcast-catalog-album", form_entry(NULL, 120, NULL), FALSE);
            editor_field(dialog, grid, 5, "Süre · dk:ss", "broadcast-catalog-duration", form_entry(NULL, 6, duration_pattern), FALSE);
        }
    }
    gtk_widget_show_all(dialog);
}

/* Ö of a content row: the content editor keeps its id, event and media. */
static void programme_item_editor(BroadcastView *view, const gchar *edit_id)
{
    GtkWidget *proxy = g_object_ref_sink(gtk_button_new_with_label(""));
    g_object_set_data_full(G_OBJECT(proxy), "programme-edit-event-id", g_strdup(edit_id), g_free);
    BroadcastForm form = {view->ctx, "İçerik özellikleri", event_fields};
    broadcast_form_open(GTK_BUTTON(proxy), &form);
    gtk_widget_destroy(proxy); g_object_unref(proxy);
}

/* Source assignments and playback are separate from the immutable on-air
 * programme. The existing event UUID anchors its independent runtime badge. */
static JsonObject *programme_event_playback_value(GtkWidget *dialog)
{
    GtkComboBox *player = g_object_get_data(G_OBJECT(dialog), "event-playback-player");
    GtkComboBox *after = g_object_get_data(G_OBJECT(dialog), "event-playback-after");
    const gchar *selected = gtk_combo_box_get_active_id(player), *next = gtk_combo_box_get_active_id(after);
    JsonObject *value = json_object_new();
    json_object_set_int_member(value, "player", selected && g_strcmp0(selected, "auto") ? (gint64)g_ascii_strtoull(selected, NULL, 10) : 0);
    json_object_set_string_member(value, "next_cluster_id", next && g_strcmp0(next, "stop") ? next : ""); return value;
}
static JsonObject *programme_event_configuration(BroadcastView *view, const gchar *id, GError **error)
{
    RadioSProgrammeTree *tree = programme_tree(view); guint index = 0; gint leaf = -1;
    if (!radios_programme_tree_find(tree, id, &index, &leaf) || leaf >= 0) {
        radios_programme_tree_free(tree); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Event artık bu sırada bulunmuyor."); return NULL;
    }
    RadioSProgrammeBlock *block = g_ptr_array_index(tree->blocks, index);
    JsonObject *override = !view->queue_slot ? g_hash_table_lookup(view->event_overrides, id) : NULL;
    JsonObject *assigned = catalog_object(override, "programme");
    if (assigned) { radios_programme_tree_free(tree); return radios_programme_clone_object(assigned); }
    if (!block->leaves->len) { radios_programme_tree_free(tree); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Evente önce playlist veya şema atayın."); return NULL; }
    JsonObject *programme = json_object_new();
    json_object_set_string_member(programme, "title", radios_json_string(block->block, "title", "Event"));
    json_object_set_string_member(programme, "date", radios_json_string(json_node_get_object(view->programme_config), "date", ""));
    json_object_set_string_member(programme, "note", "");
    JsonArray *blocks = json_array_new(), *events = json_array_new();
    json_array_add_object_element(blocks, radios_programme_clone_object(block->block));
    for (guint i = 0; i < block->leaves->len; ++i) {
        JsonObject *entry = radios_programme_clone_object(g_ptr_array_index(block->leaves, i));
        json_object_set_string_member(entry, "block_id", radios_json_string(block->block, "id", ""));
        json_object_remove_member(entry, "planned_jingles"); json_object_remove_member(entry, "planned_jingle");
        json_array_add_object_element(events, entry);
    }
    json_object_set_array_member(programme, "blocks", blocks); json_object_set_array_member(programme, "events", events);
    radios_programme_tree_free(tree); return programme;
}
static gboolean programme_event_start(BroadcastView *view, const gchar *id, JsonObject *playback, GError **error)
{
    JsonObject *programme = programme_event_configuration(view, id, error); if (!programme) return FALSE;
    gint64 visible = radios_json_int(playback, "player", 0);
    /* 4c schema contract: live.automation_id is carried for the event run;
     * event_playout records it and keeps the formal programme unchanged. */
    const gchar *automation = radios_json_string(playback, "automation_id", "");
    if (*automation) json_object_set_string_member(programme, "automation_id", automation);
    gboolean accepted = radios_event_playout_programme_for_event(view->ctx, view->page, programme,
        visible > 0 && visible <= 32 ? (guint)visible - 1 : G_MAXUINT,
        radios_json_string(playback, "next_cluster_id", ""), id, error);
    json_object_unref(programme); return accepted;
}
static void programme_event_play(GtkButton *button_widget, gpointer value)
{
    (void)button_widget; GtkWidget *dialog = value;
    RadioSViewContext *ctx = g_object_get_data(G_OBJECT(dialog), "event-playback-context");
    BroadcastView *view = ctx ? broadcast_context(ctx) : NULL;
    GtkLabel *status = g_object_get_data(G_OBJECT(dialog), "broadcast-status");
    if (!view || view->disposed) return;
    if (view->programme_document != g_object_get_data(G_OBJECT(dialog), "event-playback-document") ||
        view->queue_slot + 1 != GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(dialog), "event-playback-slot"))) {
        gtk_label_set_text(status, "Yayın sırası değişti; eventi yeniden seçin."); return;
    }
    JsonObject *playback = programme_event_playback_value(dialog); GError *error = NULL;
    gboolean accepted = programme_event_start(view, g_object_get_data(G_OBJECT(dialog), "programme-block-id"), playback, &error);
    gtk_label_set_text(status, accepted ? "Event kümesiz oynatıcıya Yarı AutoDJ ile gönderildi." : error ? error->message : "Event başlatılamadı.");
    json_object_unref(playback); g_clear_error(&error);
}
static void programme_event_playback_fields(BroadcastView *view, GtkWidget *dialog, GtkWidget *grid, JsonObject *block)
{
    GtkWidget *player = gtk_combo_box_text_new(), *after = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(player), "auto", "Otomatik · boş kümesiz oynatıcı");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(after), "stop", "Event bitince dur");
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(view->ctx->players);
    if (topology && radios_player_workspace_ready(view->ctx->players)) {
        for (guint i = 0; i < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++i) {
            const RadioSPlayerDefinition *definition = radios_player_topology_player_const(topology, i);
            if (!definition || *definition->cluster_id) continue;
            gchar *id = g_strdup_printf("%u", i + 1);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(player), id, definition->name); g_free(id);
        }
        for (guint i = 0; i < topology->cluster_count; ++i)
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(after), topology->clusters[i].id, topology->clusters[i].name);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(player), 0); gtk_combo_box_set_active(GTK_COMBO_BOX(after), 0);
    JsonObject *override = !view->queue_slot ? g_hash_table_lookup(view->event_overrides, radios_json_string(block, "id", "")) : NULL;
    JsonObject *saved = catalog_object(override ? override : block, "playback");
    gint64 visible = radios_json_int(saved, "player", 0);
    if (visible > 0 && visible <= 32) {
        gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, visible);
        if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(player), id)) {
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(player), id, "Kayıtlı oynatıcı kullanılamıyor");
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(player), id);
        }
        g_free(id);
    }
    const gchar *cluster = radios_json_string(saved, "next_cluster_id", "");
    if (*cluster && !gtk_combo_box_set_active_id(GTK_COMBO_BOX(after), cluster)) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(after), cluster, "Kayıtlı küme kullanılamıyor");
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(after), cluster);
    }
    editor_field(dialog, grid, 4, "Kümesiz oynatıcı", "event-playback-player", player, FALSE);
    editor_field(dialog, grid, 5, "Event sonrası", "event-playback-after", after, FALSE);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *play = gtk_button_new_with_label("Kümesizde çal"), *assign = gtk_button_new_with_label("Playlist / şema ata…");
    gtk_widget_set_name(play, "programme-block-play"); gtk_widget_set_name(assign, "programme-block-assign");
    gtk_box_pack_start(GTK_BOX(actions), play, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(actions), assign, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), actions, 0, 6, 2, 1);
    g_object_set_data(G_OBJECT(dialog), "event-playback-context", view->ctx);
    g_object_set_data(G_OBJECT(dialog), "event-playback-document", view->programme_document);
    g_object_set_data(G_OBJECT(dialog), "event-playback-slot", GUINT_TO_POINTER(view->queue_slot + 1));
    g_signal_connect(play, "clicked", G_CALLBACK(programme_event_play), dialog);
    g_signal_connect(assign, "clicked", G_CALLBACK(local_event_open_for_block), dialog);
}

/* Ö of an event row: name, type, planned start and note of the event. */
static void programme_block_editor(BroadcastView *view, const gchar *block_id)
{
    RadioSProgrammeTree *tree = programme_tree(view); guint block = 0; gint leaf = -1;
    if (!radios_programme_tree_find(tree, block_id, &block, &leaf) || leaf >= 0) { radios_programme_tree_free(tree); return; }
    JsonObject *object = ((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, block))->block;
    const gchar *const kinds[] = {"Müzik", "Anons", "Reklam", "Jingle", "Program", "Sweeper", "Haber", "Efekt", "Ses kaydı", "Diğer", NULL};
    const gchar *const kind_keys[] = {"music", "announcement", "advertisement", "jingle", "program", "sweeper", "news", "effect", "voice_track", "other"};
    gint kind = 0;
    for (guint i = 0; i < G_N_ELEMENTS(kind_keys); ++i) if (g_str_equal(kind_keys[i], radios_json_string(object, "kind", ""))) kind = (gint)i;
    GtkWidget *grid, *dialog = broadcast_dialog(view->ctx, "Event özellikleri", "programme-block-editor", "programme-dialog", &grid);
    editor_field(dialog, grid, 0, "Event adı", "programme-block-title", form_entry(NULL, 160, NULL), TRUE);
    editor_field(dialog, grid, 1, "Event türü", "programme-block-kind", radios_ui_combo(kinds, kind), TRUE);
    editor_field(dialog, grid, 2, "Başlangıç saati (UTC)", "programme-block-time",
        form_entry("SS:DD", 5, "^(?:[01][0-9]|2[0-3]):[0-5][0-9]$"), FALSE);
    editor_field(dialog, grid, 3, "Event notu", "programme-block-notes", form_notes(2000), FALSE);
    gtk_entry_set_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "programme-block-title")), radios_json_string(object, "title", ""));
    gtk_entry_set_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "programme-block-time")), radios_json_string(object, "start_time", ""));
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(g_object_get_data(G_OBJECT(dialog), "programme-block-notes"))),
        radios_json_string(object, "note", ""), -1);
    g_object_set_data_full(G_OBJECT(dialog), "programme-block-id", g_strdup(block_id), g_free);
    programme_event_playback_fields(view, dialog, grid, object);
    if (!view->queue_slot && general_mode(view)) {
        const gchar *fields[] = {"programme-block-title", "programme-block-kind", "programme-block-time", "programme-block-notes"};
        for (guint i = 0; i < G_N_ELEMENTS(fields); ++i)
            gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(dialog), fields[i]), FALSE);
    }
    radios_programme_tree_free(tree);
    gtk_widget_show_all(dialog);
}

static GtkWidget *
stack_tabs(GtkWidget *container, GtkWidget **stack, const gchar *style)
{
    GtkWidget *tabs = gtk_stack_switcher_new();
    *stack = gtk_stack_new();
    gtk_stack_set_homogeneous(GTK_STACK(*stack), FALSE);
    gtk_stack_set_transition_type(GTK_STACK(*stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(tabs), GTK_STACK(*stack));
    add_class(tabs, style);
    gtk_widget_set_halign(tabs, GTK_ALIGN_FILL);
    pack(container, tabs, FALSE);
    pack(container, *stack, TRUE);
    return tabs;
}

static GtkWidget *
programme_toolbar(BroadcastView *view)
{
    GtkWidget *programme = named_box(GTK_ORIENTATION_HORIZONTAL, 4, "broadcast-programme-toolbar");
    gtk_widget_set_name(programme, "programme-toolbar");
    GtkWidget *description = named_box(GTK_ORIENTATION_VERTICAL, 1, NULL);
    JsonObject *config = json_node_get_object(view->programme_config);
    view->programme_title = radios_ui_label(radios_json_string(config, "title", ""), "ld-prog-name");
    view->programme_date = radios_ui_label(radios_json_string(config, "date", ""), "ld-prog-meta");
    gtk_widget_set_tooltip_text(view->programme_title, radios_json_string(config, "title", ""));
    gtk_widget_set_name(view->programme_title, "programme-title");
    gtk_widget_set_name(view->programme_date, "programme-date");
    gtk_label_set_line_wrap(GTK_LABEL(view->programme_title), FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(view->programme_title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(view->programme_title), 16);
    pack(description, view->programme_title, FALSE); pack(description, view->programme_date, FALSE);
    pack(programme, description, FALSE);
    return programme;
}

/* Design side buttons: catalogue insert/replace/append modes and clearing the
 * selected event's contents; the catalogue opens beside the flow. */
static void
queue_side_action(GtkButton *control, gpointer data)
{
    BroadcastView *view = data;
    guint action = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(control), "broadcast-side-action"));
    if (action == 2) {
        if (!programme_can_change(view)) return;
        RadioSProgrammeTree *tree = programme_tree(view); guint block = 0; gint leaf = -1;
        if (!programme_item(view, tree, &block, &leaf)) { radios_programme_tree_free(tree); programme_notice(view, "Temizlenecek eventi seçin."); return; }
        RadioSProgrammeBlock *owner = g_ptr_array_index(tree->blocks, block);
        gchar *id = g_strdup(radios_json_string(owner->block, "id", ""));
        if (!owner->leaves->len) { radios_programme_tree_free(tree); g_free(id); return; }
        radios_programme_tree_clear(tree, block);
        if (programme_commit(view, tree, id)) programme_notice(view, "Event içeriği temizlendi.");
        g_free(id); return;
    }
    view->queue_mode = action == 1 ? QUEUE_REPLACE : action == 3 ? QUEUE_APPEND : QUEUE_INSERT;
    placement_update(view); catalog_actions_update(view);
    gtk_stack_set_visible_child_name(GTK_STACK(view->right_stack), "library");
    gtk_widget_grab_focus(view->query);
}

/* ⇈ ↑ ↓ ⇊ move the selected event among events, or a content inside its event. */
static void programme_order(GtkButton *control, gpointer data)
{
    BroadcastView *view = data;
    if (!programme_can_change(view)) return;
    RadioSProgrammeTree *tree = programme_tree(view); guint block = 0; gint leaf = -1;
    if (!programme_item(view, tree, &block, &leaf)) { radios_programme_tree_free(tree); programme_notice(view, "Taşımak için yayın sırasından bir event veya içerik seçin."); return; }
    guint length = leaf < 0 ? tree->blocks->len : ((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, block))->leaves->len;
    guint index = leaf < 0 ? block : (guint)leaf;
    guint action = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(control), "programme-order-action"));
    guint position = action == 0 ? 0 : action == 1 ? (index ? index - 1 : 0) : action == 2 ? index + 2 : length;
    gchar *id = g_strdup(view->selected_event);
    if (!radios_programme_tree_move(tree, id, block, MIN(position, length))) {
        radios_programme_tree_free(tree); g_free(id); programme_notice(view, "Seçili öğe zaten bu yöndeki sınırda."); return;
    }
    programme_commit(view, tree, id); g_free(id);
}

/* + Müzik / Anons / Reklam eventi: an empty event at the end of the flow. */
static void programme_add_event(GtkButton *control, gpointer data)
{
    BroadcastView *view = data;
    if (!programme_can_change(view)) return;
    RadioSProgrammeTree *tree = programme_tree(view);
    RadioSProgrammeBlock *created = radios_programme_tree_add_block(tree, tree->blocks->len,
        g_object_get_data(G_OBJECT(control), "programme-event-kind"), NULL);
    gchar *id = g_strdup(radios_json_string(created->block, "id", ""));
    radios_programme_queue_set_expanded(view->queue_table, id, TRUE);
    view->queue_mode = QUEUE_APPEND;
    programme_commit(view, tree, id); g_free(id);
}

static void programme_start(BroadcastView *view, guint start);
static void programme_item_editor(BroadcastView *view, const gchar *edit_id);
static void programme_block_editor(BroadcastView *view, const gchar *block_id);
/* Entries of the running plan that a row stands for: the content itself, or
 * every content of an event row. -1 when the row is not part of the run, 0
 * when the executor refused (the reason is shown), 1 when they left it. */
static gint programme_run_remove(BroadcastView *view, const gchar *id)
{
    RadioSViewContext *context = programme_context(view);
    JsonNode *order = radios_playout_ui_plan_snapshot(context); if (!order) return -1;
    JsonArray *entries = catalog_array(json_node_get_object(order), "entries");
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; entries && i < json_array_get_length(entries); ++i) {
        JsonObject *row = json_array_get_object_element(entries, i);
        if (!g_strcmp0(radios_json_string(row, "entry_id", ""), id) || !g_strcmp0(radios_json_string(row, "block_id", ""), id))
            g_ptr_array_add(ids, g_strdup(radios_json_string(row, "entry_id", "")));
    }
    json_node_free(order);
    RadioSAudioRunSnapshot run = {0};
    if (!ids->len || !radios_playout_ui_navigation_snapshot(context, &run) || run.state == RADIOS_AUDIO_RUN_IDLE ||
        run.state == RADIOS_AUDIO_RUN_FINISHED || run.state == RADIOS_AUDIO_RUN_ERROR) { g_ptr_array_unref(ids); return -1; }
    GError *error = NULL;
    gboolean removed = radios_playout_ui_remove_future_entries(context, (const gchar *const *)ids->pdata, ids->len, &error);
    if (!removed) programme_notice(view, error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_BUSY) ?
        "Geçiş sürüyor; bitince yeniden kaldırın." : error ? error->message : "İçerik yayındaki sıradan kaldırılamadı.");
    g_clear_error(&error); g_ptr_array_unref(ids);
    return removed ? 1 : 0;
}
/* The running general flow lost a row: its projection drops it at once (the
 * numbers, the next row and the players follow) and later projections of
 * this run keep it out. The calendar itself is unchanged. */
static void programme_run_removed(BroadcastView *view, const gchar *id)
{
    if (!view->run_removed) view->run_removed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_add(view->run_removed, g_strdup(id));
    /* An event left without contents in this run leaves the list with them. */
    RadioSProgrammeTree *shown = programme_tree(view); guint block = 0; gint leaf = -1;
    if (radios_programme_tree_find(shown, id, &block, &leaf) && leaf >= 0 &&
        ((RadioSProgrammeBlock *)g_ptr_array_index(shown->blocks, block))->leaves->len == 1)
        g_hash_table_add(view->run_removed, g_strdup(radios_json_string(((RadioSProgrammeBlock *)g_ptr_array_index(shown->blocks, block))->block, "id", "")));
    radios_programme_tree_free(shown);
    JsonNode **slots[] = {&view->programme_config, &view->queues[0].config, &view->queues[0].run_config};
    for (guint i = 0; i < G_N_ELEMENTS(slots); ++i) {
        if (!*slots[i] || !JSON_NODE_HOLDS_OBJECT(*slots[i])) continue;
        RadioSProgrammeTree *tree = radios_programme_tree_new(json_node_get_object(*slots[i]));
        gboolean changed = FALSE; GHashTableIter removed; gpointer key; g_hash_table_iter_init(&removed, view->run_removed);
        while (g_hash_table_iter_next(&removed, &key, NULL)) changed |= radios_programme_tree_remove(tree, key);
        if (changed) {
            JsonNode *next = radios_programme_tree_config(tree, json_node_get_object(*slots[i])); json_node_free(*slots[i]); *slots[i] = next;
        }
        radios_programme_tree_free(tree);
    }
    if (!g_strcmp0(view->selected_event, id)) g_clear_pointer(&view->selected_event, g_free);
    programme_render(view);
    radios_programme_queue_set_run_config(view->queue_table, view->queues[0].run_config ? json_node_get_object(view->queues[0].run_config) : NULL);
}
static void queue_action(gpointer data, const gchar *id, RadioSProgrammeRowAction action)
{
    BroadcastView *view = data;
    if (view->disposed) return;
    RadioSProgrammeTree *tree = programme_tree(view); guint block = 0; gint leaf = -1;
    if (!radios_programme_tree_find(tree, id, &block, &leaf)) { radios_programme_tree_free(tree); return; }
    RadioSProgrammeBlock *owner = g_ptr_array_index(tree->blocks, block);
    JsonObject *content = leaf >= 0 ? g_ptr_array_index(owner->leaves, (guint)leaf) : NULL;
    if (action == RADIOS_PROGRAMME_ROW_PLAY) {
        JsonObject *override = !view->queue_slot && leaf < 0 ? g_hash_table_lookup(view->event_overrides, id) : NULL;
        JsonObject *playback = leaf < 0 ? catalog_object(override ? override : owner->block, "playback") : NULL;
        if (playback) {
            JsonObject *selection = radios_programme_clone_object(playback); gchar *event_id = g_strdup(id);
            radios_programme_tree_free(tree); GError *error = NULL;
            if (!programme_event_start(view, event_id, selection, &error)) programme_notice(view, error ? error->message : "Event başlatılamadı.");
            g_clear_error(&error); json_object_unref(selection); g_free(event_id); return;
        }
        gint start = radios_programme_tree_leaf_index(tree, id);
        radios_programme_tree_free(tree);
        if (start < 0) { programme_notice(view, "Event boş."); return; }
        programme_start(view, (guint)start); return;
    }
    if (action == RADIOS_PROGRAMME_ROW_WAVEFORM) {
        gchar *media = content ? g_strdup(radios_json_string(content, "media_id", "")) : NULL;
        radios_programme_tree_free(tree);
        if (media && *media) radios_media_library_open_selected(view->ctx, media);
        g_free(media); return;
    }
    if (action == RADIOS_PROGRAMME_ROW_LIVE || action == RADIOS_PROGRAMME_ROW_TALK_PRESS || action == RADIOS_PROGRAMME_ROW_TALK_RELEASE) {
        gchar *event_id = g_strdup(radios_json_string(owner->block, "id", ""));
        radios_programme_tree_free(tree); programme_live_action(view, event_id, action); g_free(event_id); return;
    }
    radios_programme_tree_free(tree);
    if (action == RADIOS_PROGRAMME_ROW_PROPERTIES) {
        if (leaf >= 0) programme_item_editor(view, id); else programme_block_editor(view, id);
        return;
    }
    /* U7: × on rows of the running flow first takes their unheard contents
     * out of the executor's queue (the sounding content keeps playing). The
     * general queue is the calendar's projection: only the run changes. */
    gint run = action == RADIOS_PROGRAMME_ROW_REMOVE ? programme_run_remove(view, id) : -1;
    if (run == 0) return;
    if (run == 1 && !view->queue_slot && general_mode(view)) { programme_run_removed(view, id); return; }
    if (!programme_can_change(view)) return;
    tree = programme_tree(view);
    /* The neighbour in the same event, else the event, stays selected. */
    gchar *next = NULL;
    owner = g_ptr_array_index(tree->blocks, block);
    if (leaf >= 0 && owner->leaves->len > 1) next = g_strdup(radios_json_string(g_ptr_array_index(owner->leaves,
        (guint)leaf + 1 < owner->leaves->len ? (guint)leaf + 1 : (guint)leaf - 1), "id", ""));
    else if (leaf >= 0) next = g_strdup(radios_json_string(owner->block, "id", ""));
    else if (tree->blocks->len > 1) next = g_strdup(radios_json_string(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks,
        block + 1 < tree->blocks->len ? block + 1 : block - 1))->block, "id", ""));
    radios_programme_tree_remove(tree, id);
    g_clear_pointer(&view->selected_event, g_free);
    programme_commit(view, tree, next); g_free(next);
}

static JsonNode *json_parse(const gchar *text)
{
    JsonParser *parser = json_parser_new();
    JsonNode *root = text && json_parser_load_from_data(parser, text, -1, NULL) ? json_node_copy(json_parser_get_root(parser)) : NULL;
    g_object_unref(parser); return root;
}

/* Saves leaves (adopted) at placement and selects the first one. */
static gboolean programme_insert_leaves(BroadcastView *view, GPtrArray *leaves, RadioSProgrammePlacement placement,
    const gchar *kind, const gchar *title)
{
    if (!leaves->len) { g_ptr_array_unref(leaves); programme_notice(view, "Aktarılacak geçerli içerik bulunamadı."); return FALSE; }
    RadioSProgrammeTree *tree = programme_tree(view);
    if (radios_programme_tree_leaf_count(tree) + leaves->len > 5000) {
        radios_programme_tree_free(tree); g_ptr_array_unref(leaves); programme_notice(view, "Program en fazla 5000 öğe içerebilir."); return FALSE;
    }
    guint block = programme_place(tree, placement, leaves, kind, title);
    gchar *first = g_strdup(radios_json_string(g_ptr_array_index(leaves, 0), "id", ""));
    radios_programme_queue_set_expanded(view->queue_table, radios_json_string(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, block))->block, "id", ""), TRUE);
    g_ptr_array_unref(leaves);
    gboolean saved = programme_commit(view, tree, first); g_free(first);
    return saved;
}

/* A saved programme (Programlar) becomes one "program" event with copies of
 * its contents, like the design's "Program bloğu". */
static gboolean programme_insert_saved(BroadcastView *view, JsonObject *record, RadioSProgrammePlacement placement)
{
    JsonNode *config = record ? json_object_get_member(record, "config") : NULL;
    if (!config || !JSON_NODE_HOLDS_OBJECT(config)) return FALSE;
    RadioSProgrammeTree *source = radios_programme_tree_new(json_node_get_object(config));
    GPtrArray *leaves = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
    for (guint i = 0; i < radios_programme_tree_leaf_count(source); ++i) {
        JsonObject *leaf = radios_programme_clone_object(radios_programme_tree_leaf_at(source, i, NULL));
        gchar *id = g_uuid_string_random(); json_object_set_string_member(leaf, "id", id); g_free(id);
        json_object_remove_member(leaf, "block_id");
        g_ptr_array_add(leaves, leaf);
    }
    radios_programme_tree_free(source);
    const gchar *name = radios_json_string(record, "name", "");
    gchar *title = g_utf8_substring(name, 0, MIN(g_utf8_strlen(name, -1), 160));
    gboolean saved = programme_insert_leaves(view, leaves, placement, "program", *title ? title : NULL);
    g_free(title); return saved;
}

static gboolean catalog_media_drop(BroadcastView *view, const gchar *payload, RadioSProgrammePlacement placement);
/* Drops onto the flow: moved rows, catalogue media, saved programmes. */
static gboolean queue_drop(gpointer data, RadioSDragInfo info, const gchar *payload, RadioSProgrammePlacement placement)
{
    BroadcastView *view = data;
    if (view->disposed || !programme_can_change(view)) return FALSE;
    if (info == RADIOS_DRAG_INFO_PLAYLIST) return playlist_drop(view, payload, placement);
    if (info == RADIOS_DRAG_INFO_MEDIA) return catalog_media_drop(view, payload, placement);
    if (info == RADIOS_DRAG_INFO_ROW) {
        RadioSProgrammeTree *tree = programme_tree(view); guint block = 0; gint leaf = -1; gboolean moved;
        if (!radios_programme_tree_find(tree, payload, &block, &leaf)) { radios_programme_tree_free(tree); return FALSE; }
        if (leaf < 0) moved = radios_programme_tree_move(tree, payload, 0, placement.block < 0 ? placement.position : (guint)placement.block);
        else if (placement.block >= 0) moved = radios_programme_tree_move(tree, payload, (guint)placement.block, placement.position);
        else {
            /* A content dropped between events opens its own event there. */
            const gchar *kind = radios_json_string(g_ptr_array_index(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, block))->leaves, (guint)leaf), "kind", "music");
            guint at = MIN(placement.position, tree->blocks->len);
            radios_programme_tree_add_block(tree, at, kind, NULL);
            moved = radios_programme_tree_move(tree, payload, at, 0);
        }
        if (!moved) { radios_programme_tree_free(tree); return FALSE; }
        return programme_commit(view, tree, payload);
    }
    JsonNode *node = json_parse(payload); gboolean accepted = FALSE;
    if (info == RADIOS_DRAG_INFO_PROGRAMME && node && JSON_NODE_HOLDS_OBJECT(node))
        accepted = programme_insert_saved(view, json_node_get_object(node), placement);
    if (node) json_node_free(node);
    return accepted;
}

gboolean radios_broadcast_transition_policy(RadioSViewContext *ctx, guint *mode,
    gdouble *overlap_seconds, gboolean *beat_sync)
{
    BroadcastView *view = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context") : NULL;
    if (!view || view->disposed || !view->policy_active || !view->policy) return FALSE;
    RadioSDataClient *current = radios_data_client_acquire(ctx);
    gboolean identity_ok = !view->policy_client || radios_data_client_same_identity(current, view->policy_client);
    if (current) radios_data_client_unref(current);
    if (!identity_ok) return FALSE;
    JsonObject *policy = json_node_get_object(view->policy);
    if (mode) *mode = (guint)radios_json_int(policy, "mode", 1);
    if (overlap_seconds) *overlap_seconds = radios_json_int(policy, "overlap_ms", 3000) / 1000.0;
    if (beat_sync) *beat_sync = json_object_get_boolean_member(policy, "beat_sync");
    return TRUE;
}
static void policy_status_changed(RadioSDocument *document, const gchar *message, gpointer data)
{
    (void)document; BroadcastView *view = data;
    if (!view->disposed && view->policy_status) gtk_label_set_text(GTK_LABEL(view->policy_status), message);
}
static void policy_submit(BroadcastView *view)
{
    if (!view->policy_dirty || !view->policy_document || !radios_data_document_ready(view->policy_document)) return;
    RadioSDataClient *current = radios_data_client_acquire(view->ctx);
    if (current && view->policy_client && !radios_data_client_same_identity(current, view->policy_client)) {
        if (current) radios_data_client_unref(current);
        gtk_label_set_text(GTK_LABEL(view->policy_status), "Bağlantı veya istasyon değişti; ayarlar kaydedilmedi."); return;
    }
    if (!view->policy_client) view->policy_client = current;
    else if (current) radios_data_client_unref(current);
    if (radios_data_document_autosave(view->policy_document, view->policy)) view->policy_dirty = FALSE;
}
void radios_broadcast_prepare_close(RadioSViewContext *ctx)
{
    BroadcastView *view = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context") : NULL;
    if (!view || view->disposed) return;
    /* The window closes the data service before destroying child pages. Flush
     * the newest successor while its authenticated recovery client is alive. */
    policy_submit(view);
    if (view->policy_document) radios_data_document_preserve_pending(view->policy_document);
}
static void policy_store(BroadcastView *view, guint selected, gint64 overlap)
{
    guint mode = selected == 4 ? RADIOS_AUDIO_RUN_CROSSFADE : selected;
    gboolean beat = selected == 4;
    JsonObject *policy = json_object_new(); json_object_set_int_member(policy, "schema", 1);
    json_object_set_int_member(policy, "mode", mode); json_object_set_int_member(policy, "overlap_ms", overlap);
    json_object_set_boolean_member(policy, "beat_sync", beat);
    g_clear_pointer(&view->policy, json_node_free); view->policy = json_node_new(JSON_NODE_OBJECT); json_node_take_object(view->policy, policy);
    view->policy_active = view->policy_dirty = TRUE;
    RadioSDataClient *current = radios_data_client_acquire(view->ctx);
    if (current) {
        g_clear_pointer(&view->policy_client, radios_data_client_unref); view->policy_client = current;
    }
    view->queues[0].transition = selected; view->queues[0].overlap = overlap / 1000.0;
    if (view->policy_document) radios_data_document_dirty(view->policy_document, TRUE);
    gtk_label_set_text(GTK_LABEL(view->policy_status), view->policy_document ? "Kaydediliyor…" : "Bu oturumda geçerli");
    policy_submit(view);
}
static void policy_changed(BroadcastView *view)
{
    if (view->disposed || view->queue_switching || view->queue_slot) return;
    gint selected = gtk_combo_box_get_active(GTK_COMBO_BOX(view->transition_mode));
    guint mode = selected == 4 ? RADIOS_AUDIO_RUN_CROSSFADE : (guint)MAX(0, selected);
    gboolean beat = selected == 4;
    gint64 overlap = mode == RADIOS_AUDIO_RUN_CROSSFADE || mode == RADIOS_AUDIO_RUN_CUE ?
        (gint64)llround(gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->transition_overlap)) * 1000) : 0;
    if (beat && !overlap) {
        view->queue_switching = TRUE;
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->transition_overlap), 3); view->queue_switching = FALSE;
        overlap = 3000;
    }
    policy_store(view, (guint)MAX(0, selected), overlap);
}
gboolean radios_broadcast_transition_policy_ready(RadioSViewContext *ctx)
{
    BroadcastView *view = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context") : NULL;
    if (!view || view->disposed || !view->policy_document || !radios_data_document_ready(view->policy_document)) return FALSE;
    RadioSDataClient *current = radios_data_client_acquire(ctx);
    gboolean ready = current && (!view->policy_client || radios_data_client_same_identity(current, view->policy_client));
    if (current) radios_data_client_unref(current);
    return ready;
}
const gchar *radios_broadcast_transition_policy_status(RadioSViewContext *ctx)
{
    BroadcastView *view = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context") : NULL;
    return view && !view->disposed && view->policy_status ? gtk_label_get_text(GTK_LABEL(view->policy_status)) : "Yayın ayarları hazır değil.";
}
gboolean radios_broadcast_transition_policy_set(RadioSViewContext *ctx, guint mode,
    gdouble overlap_seconds, gboolean beat_sync, GError **error)
{
    if (!radios_broadcast_transition_policy_ready(ctx)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Auto DJ ayarları veritabanından yükleniyor; bağlantıyı kontrol edin."); return FALSE;
    }
    if (mode > RADIOS_AUDIO_RUN_MANUAL || !isfinite(overlap_seconds) || overlap_seconds < 0 || overlap_seconds > 30 ||
        (beat_sync && mode != RADIOS_AUDIO_RUN_CROSSFADE)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Geçiş türü ve 0–30 saniye arasında bir süre gerekli."); return FALSE;
    }
    BroadcastView *view = g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context");
    guint selected = beat_sync ? 4 : mode;
    gint64 overlap = mode == RADIOS_AUDIO_RUN_CROSSFADE || mode == RADIOS_AUDIO_RUN_CUE ? (gint64)llround(overlap_seconds * 1000) : 0;
    if (beat_sync && !overlap) overlap = 3000;
    if (!view->queue_slot) {
        view->queue_switching = TRUE;
        gtk_combo_box_set_active(GTK_COMBO_BOX(view->transition_mode), (gint)selected);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->transition_overlap), overlap / 1000.0);
        view->queue_switching = FALSE;
    }
    policy_store(view, selected, overlap); return TRUE;
}
static void policy_overlap_changed(GtkSpinButton *spin, gpointer data)
{ (void)spin; policy_changed(data); }
static void policy_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *policy, gpointer data)
{
    (void)owner; BroadcastView *view = data; if (view->disposed || view->policy_dirty) return;
    g_clear_pointer(&view->policy_client, radios_data_client_unref); view->policy_client = radios_data_client_acquire(ctx);
    g_clear_pointer(&view->policy, json_node_free); view->policy = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(view->policy, radios_programme_clone_object(policy)); view->policy_active = TRUE;
    view->queues[0].transition = json_object_get_boolean_member(policy, "beat_sync") ? 4 : (guint)radios_json_int(policy, "mode", 1);
    view->queues[0].overlap = radios_json_int(policy, "overlap_ms", 3000) / 1000.0;
    if (!view->queue_slot) {
        view->queue_switching = TRUE;
        gtk_combo_box_set_active(GTK_COMBO_BOX(view->transition_mode), (gint)view->queues[0].transition);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->transition_overlap), view->queues[0].overlap);
        view->queue_switching = FALSE;
    }
    gtk_label_set_text(GTK_LABEL(view->policy_status), "Kaydedildi");
}
static void policy_dialog_response(GtkDialog *dialog, gint response, gpointer data)
{
    GtkWidget *owner = data; BroadcastView *view = g_object_get_data(G_OBJECT(owner), "radios-broadcast-view");
    if (!view) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    if (response == GTK_RESPONSE_ACCEPT && !view->disposed && !view->queue_slot) {
        RadioSDataClient *captured = g_object_get_data(G_OBJECT(dialog), "policy-client");
        RadioSDataClient *current = radios_data_client_acquire(view->ctx);
        /* Offline changes retain their original document identity and are
         * staged by the autosave recovery path. A connected new identity may
         * never consume a dialog opened for another session. */
        gboolean identity_ok = !current || radios_data_client_same_identity(current, captured);
        if (current) radios_data_client_unref(current);
        if (!identity_ok) {
            gtk_label_set_text(GTK_LABEL(view->policy_status), "Bağlantı veya istasyon değişti; pencereyi yeniden açın.");
            gtk_widget_destroy(GTK_WIDGET(dialog)); return;
        }
        GtkWidget *mode = g_object_get_data(G_OBJECT(dialog), "policy-mode"), *overlap = g_object_get_data(G_OBJECT(dialog), "policy-overlap");
        view->queue_switching = TRUE;
        gtk_combo_box_set_active(GTK_COMBO_BOX(view->transition_mode), gtk_combo_box_get_active(GTK_COMBO_BOX(mode)));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->transition_overlap), gtk_spin_button_get_value(GTK_SPIN_BUTTON(overlap)));
        view->queue_switching = FALSE; policy_changed(view);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void policy_settings_clicked(GtkButton *button, gpointer data)
{
    (void)button; BroadcastView *view = data; if (view->disposed || view->queue_slot) return;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Yayın sırası otomasyonu", view->ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL, "Uygula", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(dialog, "programme-automation-dialog"); gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 240);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_box_set_spacing(GTK_BOX(box), 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    const gchar *const modes[] = {"Parça bitince", "Crossfade", "AutoDJ OUT / Cue", "Elle geç", "SYNC beatmatch · 4 vuruş", NULL};
    GtkWidget *mode = radios_ui_combo(modes, (gint)view->queues[0].transition);
    gtk_widget_set_name(mode, "programme-automation-dialog-mode"); pack(box, mode, FALSE);
    GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL); pack(row, radios_ui_label("Geçiş süresi (sn)", NULL), FALSE);
    GtkWidget *overlap = gtk_spin_button_new_with_range(0, 30, .1); gtk_spin_button_set_digits(GTK_SPIN_BUTTON(overlap), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(overlap), view->queues[0].overlap); pack(row, overlap, TRUE); pack(box, row, FALSE);
    g_object_set_data(G_OBJECT(dialog), "policy-mode", mode); g_object_set_data(G_OBJECT(dialog), "policy-overlap", overlap);
    g_object_set_data_full(G_OBJECT(dialog), "policy-client", radios_data_client_acquire(view->ctx),
        (GDestroyNotify)radios_data_client_unref);
    g_signal_connect_object(dialog, "response", G_CALLBACK(policy_dialog_response), view->page, 0);
    g_signal_connect_object(view->page, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    gtk_widget_show_all(dialog);
}
static void
transition_mode_changed(GtkComboBox *combo, gpointer data)
{
    BroadcastView *view = data;
    gint selected = gtk_combo_box_get_active(combo);
    if (view->queue_slot && (selected == 1 || selected == 4)) {
        gtk_combo_box_set_active(combo, 0);
        programme_notice(view, "Tek oynatıcıda iki parça bindirilemez; Crossfade için Genel yayın sırasını kullanın."); return;
    }
    gtk_widget_set_sensitive(view->transition_overlap, !view->queue_slot && (selected == 1 || selected == 2 || selected == 4));
    if (!view->queue_slot) policy_changed(view);
    if (view->queue_slot && !view->queue_switching && view->programme_config) programme_save(NULL, view);
}

static void queue_stash(BroadcastView *view)
{
    PlayerQueue *queue = &view->queues[view->queue_slot];
    g_clear_pointer(&queue->config, json_node_free); queue->config = programme_copy(view->programme_config);
    g_free(queue->selected); queue->selected = g_strdup(view->selected_event);
    queue->transition = (guint)MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(view->transition_mode)));
    queue->overlap = gtk_spin_button_get_value(GTK_SPIN_BUTTON(view->transition_overlap));
}

static JsonNode *queue_empty(guint slot)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *config = json_object_new(); json_node_take_object(node, config);
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d");
    gchar *title = slot ? g_strdup_printf("Oynatıcı %u yayın sırası", slot) : g_strdup("Genel yayın otomasyonu");
    json_object_set_string_member(config, "title", title); json_object_set_string_member(config, "date", date);
    json_object_set_string_member(config, "note", ""); json_object_set_array_member(config, "events", json_array_new());
    json_object_set_int_member(config, "queue_player", slot); json_object_set_int_member(config, "queue_transition", 0);
    json_object_set_double_member(config, "queue_overlap", 0);
    g_free(title); g_free(date); g_date_time_unref(now); return node;
}

/* Flow rows name their player like its card does. A player's own queue
 * defaults to that player; the general queue lets automation choose. */
static void queue_players_context(BroadcastView *view)
{
    if (!view->queue_table) return;
    const RadioSPlayerTopology *topology = view->ctx->players ? radios_player_workspace_topology(view->ctx->players) : NULL;
    const gchar *names[32] = {NULL};
    for (guint deck = 0; topology && deck < 32; ++deck) {
        const RadioSPlayerDefinition *definition = radios_player_topology_player_const(topology, deck);
        if (definition && *definition->name) names[deck] = definition->name;
    }
    radios_programme_queue_set_player_context(view->queue_table, view->queue_slot, names);
}

static void queue_player_changed(GtkComboBox *combo, gpointer data)
{
    BroadcastView *view = data; const gchar *id = gtk_combo_box_get_active_id(combo);
    if (view->disposed || view->queue_switching || !id) return;
    guint slot = (guint)g_ascii_strtoull(id, NULL, 10);
    if (slot > 32 || slot == view->queue_slot) return;
    if (view->editor_caption) {
        const RadioSPlayerTopology *topology = radios_player_workspace_topology(view->ctx->players);
        const RadioSPlayerDefinition *definition = slot ? radios_player_topology_player_const(topology, slot - 1) : NULL;
        gchar *caption = slot ? g_strdup_printf("%s · yayın sırasını düzenle", definition && *definition->name ? definition->name : "Seçili oynatıcı") : g_strdup("Genel yayın sırasını düzenle");
        gtk_label_set_text(GTK_LABEL(view->editor_caption), caption); g_free(caption);
    }
    queue_stash(view); ++view->catalogue_intent; view->catalogue_busy = FALSE; view->queue_switching = TRUE; view->queue_slot = slot;
    radios_programme_queue_reset_progress(view->queue_table);
    PlayerQueue *queue = &view->queues[slot];
    if (!queue->config) queue->config = queue_empty(slot);
    if (slot && !queue->document) {
        gchar *name = g_strdup_printf("Oynatıcı %u yayın sırası", slot);
        queue->document = radios_data_document_new(view->ctx, view->page, "programme", name, programme_apply, queue); g_free(name);
        if (view->ctx->audio) queue->context.playout = radios_playout_ui_new_player(&queue->context, slot - 1, view->page);
    }
    view->programme_document = queue->document;
    g_clear_pointer(&view->programme_config, json_node_free); view->programme_config = programme_copy(queue->config);
    g_free(view->selected_event); view->selected_event = g_strdup(queue->selected);
    gtk_combo_box_set_active(GTK_COMBO_BOX(view->transition_mode), (gint)queue->transition);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->transition_overlap), queue->overlap);
    gtk_widget_set_sensitive(view->transition_overlap, !slot && (queue->transition == 1 || queue->transition == 2 || queue->transition == 4));
    gtk_widget_set_visible(view->policy_status, !slot);
    gtk_widget_set_visible(view->policy_gear, !slot);
    gtk_widget_set_visible(view->general_status, !slot && view->general_enabled);
    gtk_widget_set_visible(view->general_enable, !slot);
    view->lifecycle_context = *programme_context(view); view->queue_switching = FALSE;
    view->history_count = G_MAXUINT; g_clear_pointer(&view->history_latest, g_free);
    if (view->history_table) gtk_list_store_clear(g_object_get_data(G_OBJECT(view->history_table), "list-store"));
    programme_render(view); catalog_actions_update(view); queue_players_context(view);
    radios_programme_queue_set_run_config(view->queue_table, queue->run_config ? json_node_get_object(queue->run_config) : NULL);
    if (view->playout_status) programme_playout_tick(view);
}

static gboolean queue_documents_tick(gpointer data)
{
    BroadcastView *view = data; if (view->disposed) return G_SOURCE_REMOVE;
    policy_submit(view); general_status_update(view);
    for (guint slot = 1; slot < G_N_ELEMENTS(view->queues); ++slot) {
        PlayerQueue *queue = &view->queues[slot];
        if (!queue->document || queue->initialized || !radios_data_document_ready(queue->document)) continue;
        if (radios_data_document_revision(queue->document) > 0) queue->initialized = TRUE;
        else queue->initialized = TRUE; /* Selection reads; the first edit creates the draft. */
    }
    return G_SOURCE_CONTINUE;
}

static void queue_selector_refresh(BroadcastView *view)
{
    if (!view->queue_player) return;
    view->queue_switching = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(view->queue_player));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->queue_player), "0", "Otomasyon · Genel yayın sırası");
    for (guint slot = 1; slot < G_N_ELEMENTS(view->queues); ++slot) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", slot - 1);
        gboolean present = g_object_get_data(G_OBJECT(view->ctx->parent), key) != NULL; g_free(key);
        if (view->ctx->players && !radios_player_workspace_contains(view->ctx->players, slot - 1)) continue;
        if (!present && !view->queues[slot].document) continue;
        const RadioSPlayerDefinition *definition = view->ctx->players ?
            radios_player_topology_player_const(radios_player_workspace_topology(view->ctx->players), slot - 1) : NULL;
        gchar *name = definition && *definition->name ? g_strdup(definition->name) : g_strdup_printf("Oynatıcı %u", slot);
        gchar *id = g_strdup_printf("%u", slot), *title = g_strdup_printf("%s%s", name, present ? "" : " (kart kaldırıldı)"); g_free(name);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->queue_player), id, title); g_free(id); g_free(title);
    }
    gchar *selected = g_strdup_printf("%u", view->queue_slot);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->queue_player), selected); g_free(selected);
    view->queue_switching = FALSE;
    queue_players_context(view);
}

static GtkWidget *
flow_queue(BroadcastView *view)
{
    RadioSViewContext *ctx = view->ctx;
    GtkWidget *body;
    GtkWidget *panel = radios_ui_panel("Otomasyon · Yayın sırası", &body);
    add_class(panel, "ld-flow");
    add_class(panel, "is-selected");
    gtk_widget_set_name(panel, "flow-workspace");
    add_class(body, "ld-flow-body");
    gtk_box_set_spacing(GTK_BOX(body), 7);
    GList *children = gtk_container_get_children(GTK_CONTAINER(panel));
    if (children != NULL && GTK_IS_BOX(children->data)) {
        GtkWidget *head = children->data;
        view->queue_count = radios_ui_label("— event · — içerik", "ld-head-meta"); pack(head, view->queue_count, FALSE);
        view->queue_duration = radios_ui_label("—", "ld-head-meta"); pack(head, view->queue_duration, FALSE);
        gtk_widget_set_name(view->queue_count, "programme-queue-count"); gtk_widget_set_name(view->queue_duration, "programme-queue-duration");
        gtk_widget_set_tooltip_text(view->queue_duration, "Toplam süre");
    }
    g_list_free(children);
    GtkWidget *player_row = named_box(GTK_ORIENTATION_HORIZONTAL, 6, "programme-player-bar");
    view->editor_caption = radios_ui_label("Genel yayın sırasını düzenle", "heading");
    if (view->ctx->players) pack(player_row, view->editor_caption, TRUE);
    GtkWidget *player_label = radios_ui_label("Oynatıcı", NULL); pack(player_row, player_label, FALSE);
    view->queue_player = gtk_combo_box_text_new(); gtk_widget_set_name(view->queue_player, "programme-player-selector");
    /* With a player workspace the caption names the queue and the scope tree picks it; the selector stays for programmatic switches only. */
    if (view->ctx->players) { gtk_widget_set_no_show_all(player_label, TRUE); gtk_widget_set_no_show_all(view->queue_player, TRUE); }
    gtk_widget_set_tooltip_text(view->queue_player, "Her oynatıcının event sırası ve taşıma kontrolleri bağımsızdır.");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->queue_player), "0", "Otomasyon · Genel yayın sırası");
    gtk_combo_box_set_active(GTK_COMBO_BOX(view->queue_player), 0);
    g_signal_connect(view->queue_player, "changed", G_CALLBACK(queue_player_changed), view);
    pack(player_row, view->queue_player, TRUE); pack(body, player_row, FALSE);
    view->general_enable = gtk_check_button_new_with_label("Takvime göre sürekli yayın");
    gtk_widget_set_name(view->general_enable, "programme-general-continuous");
    view->general_enabled = TRUE; gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->general_enable), TRUE);
    gtk_widget_set_no_show_all(view->general_enable, TRUE); gtk_widget_show(view->general_enable);
    g_signal_connect(view->general_enable, "toggled", G_CALLBACK(general_enabled_changed), view);
    pack(body, view->general_enable, FALSE);
    view->general_status = radios_ui_label("Takvimdeki en yakın program aranıyor", "task-help");
    gtk_widget_set_name(view->general_status, "programme-general-schedule");
    gtk_label_set_line_wrap(GTK_LABEL(view->general_status), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(view->general_status), 42);
    gtk_widget_set_no_show_all(view->general_status, TRUE); gtk_widget_show(view->general_status); pack(body, view->general_status, FALSE);

    GtkWidget *queue = gtk_grid_new();
    add_class(queue, "programme-queue-layout");
    gtk_grid_set_column_spacing(GTK_GRID(queue), 5);
    gtk_grid_set_row_spacing(GTK_GRID(queue), 7);
    gtk_widget_set_hexpand(queue, TRUE);
    gtk_widget_set_vexpand(queue, TRUE);
    pack(body, queue, TRUE);

    GtkWidget *toolbar_frame = named_box(GTK_ORIENTATION_VERTICAL, 5, "programme-queue-toolbar");
    add_class(toolbar_frame, "programme-playback-toolbar");
    /* Settings and transport still share these controls internally. Keep their
     * owner lifetime while removing the legacy panel from the visible queue. */
    gtk_widget_set_no_show_all(toolbar_frame, TRUE);
    gtk_widget_hide(toolbar_frame);
    GtkWidget *toolbar = named_box(GTK_ORIENTATION_VERTICAL, 6, NULL);
    pack(toolbar_frame, toolbar, FALSE);
    view->placement = radios_ui_label("Sona yeni event olarak ekle", "programme-placement");
    gtk_widget_set_name(view->placement, "programme-placement"); pack(toolbar_frame, view->placement, FALSE);
    GtkWidget *transport = named_box(GTK_ORIENTATION_HORIZONTAL, 4, "programme-square-controls");
    const gchar *symbols[] = {"|◀", "▶", "Ⅱ", "■", "▶|"};
    const gchar *tips[] = {"Önceki içeriğe geç", "Seçili event veya içeriği oynat",
                          "Duraklat", "Durdur", "Sonraki içeriğe geç"};
    const gchar *transport_ids[] = {"programme-previous", "programme-play", "programme-pause", "programme-stop", "programme-next"};
    for (guint i = 0; i < G_N_ELEMENTS(symbols); ++i) {
        GtkWidget *control = button(transport, symbols[i], "programme-square", ctx,
                                   ctx->playout ? NULL : "Yayın kontrolü için gerçek ses motoru bağlantısı gerekiyor.");
        gtk_widget_set_name(control, transport_ids[i]); view->transport[i] = control;
        if (ctx->playout) { g_object_set_data(G_OBJECT(control), "programme-transport-action", GUINT_TO_POINTER(i)); g_signal_connect(control, "clicked", G_CALLBACK(programme_transport), view); }
        gtk_widget_set_tooltip_text(control, tips[i]);
    }
    pack(toolbar, transport, FALSE);
    GtkWidget *transition = named_box(GTK_ORIENTATION_VERTICAL, 5, "programme-transition-controls");
    GtkWidget *mode_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, NULL);
    GtkWidget *overlap_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, NULL);
    pack(transition, mode_row, FALSE); pack(transition, overlap_row, FALSE);
    const gchar *const modes[] = {"Parça bitince", "Crossfade", "AutoDJ OUT / Cue", "Elle geç", "SYNC beatmatch · 4 vuruş", NULL};
    pack(mode_row, radios_ui_label("Otomasyon", NULL), FALSE);
    GtkWidget *mode = radios_ui_combo(modes, 1);
    view->transition_mode = mode;
    gtk_widget_set_name(mode, "programme-transition-mode");
    GList *mode_cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(mode));
    for (GList *it = mode_cells; it; it = it->next) if (GTK_IS_CELL_RENDERER_TEXT(it->data))
        g_object_set(it->data, "ellipsize", PANGO_ELLIPSIZE_END, "max-width-chars", 18, NULL);
    g_list_free(mode_cells);
    pack(mode_row, mode, TRUE);
    pack(overlap_row, radios_ui_label("Bindirme", NULL), FALSE);
    GtkWidget *overlap = gtk_spin_button_new_with_range(0, 30, 0.1);
    view->transition_overlap = overlap;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(overlap), 3);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(overlap), 1);
    gtk_entry_set_width_chars(GTK_ENTRY(overlap), 3);
    gtk_widget_set_name(overlap, "programme-transition-overlap");
    g_signal_connect(mode, "changed", G_CALLBACK(transition_mode_changed), view);
    pack(overlap_row, overlap, FALSE);
    pack(overlap_row, radios_ui_label("sn", NULL), FALSE);
    GtkWidget *gear = button(mode_row, "⚙", "ld-btn", ctx, NULL);
    view->policy_gear = gear;
    gtk_widget_set_name(gear, "programme-automation-settings");
    gtk_widget_set_no_show_all(gear, TRUE); gtk_widget_show(gear);
    g_signal_connect(gear, "clicked", G_CALLBACK(policy_settings_clicked), view);
    add_class(gear, "programme-transition-settings");
    gtk_widget_set_tooltip_text(gear, "Geçiş otomasyonu ayarlarını aç");
    pack(toolbar, transition, FALSE);
    view->policy_status = radios_ui_label("Genel geçiş ayarı", "small"); gtk_widget_set_name(view->policy_status, "programme-automation-status");
    gtk_widget_set_no_show_all(view->policy_status, TRUE); gtk_widget_show(view->policy_status);
    pack(toolbar_frame, view->policy_status, FALSE);
    g_signal_connect(overlap, "value-changed", G_CALLBACK(policy_overlap_changed), view);
    gtk_grid_attach(GTK_GRID(queue), toolbar_frame, 0, 0, 2, 1);

    RadioSProgrammeQueueHooks hooks = {view, queue_selected, queue_action, queue_drop};
    GtkWidget *table = radios_programme_queue_new(&hooks);
    view->queue_table = table;
    gtk_widget_set_hexpand(table, TRUE); gtk_widget_set_vexpand(table, TRUE);
    gtk_grid_attach(GTK_GRID(queue), table, 0, 1, 1, 1);
    GtkWidget *side = named_box(GTK_ORIENTATION_VERTICAL, 5, "programme-queue-side");
    const gchar *side_symbols[] = {"＋", "⇄", "×", "▤"};
    const gchar *side_text[] = {"Araya\nekle", "Değiştir", "Temizle", "Girdi"};
    const gchar *side_ids[] = {"programme-insert", "programme-replace", "programme-remove", "programme-input"};
    for (guint i = 0; i < G_N_ELEMENTS(side_text); ++i) {
        GtkWidget *control = gtk_button_new();
        gtk_widget_set_name(control, side_ids[i]);
        GtkWidget *content = named_box(GTK_ORIENTATION_VERTICAL, 3, NULL);
        GtkWidget *symbol = radios_ui_label(side_symbols[i], "queue-side-symbol");
        GtkWidget *caption = radios_ui_label(side_text[i], "queue-side-caption small");
        gtk_label_set_xalign(GTK_LABEL(symbol), 0.5f);
        gtk_label_set_xalign(GTK_LABEL(caption), 0.5f);
        gtk_label_set_max_width_chars(GTK_LABEL(caption), 6);
        gtk_widget_set_size_request(control, 58, 59);
        pack(content, symbol, FALSE);
        pack(content, caption, FALSE);
        gtk_container_add(GTK_CONTAINER(control), content);
        g_object_set_data(G_OBJECT(control), "broadcast-side-action", GUINT_TO_POINTER(i));
        g_signal_connect(control, "clicked", G_CALLBACK(queue_side_action), view);
        pack(side, control, FALSE);
    }
    gtk_grid_attach(GTK_GRID(queue), side, 1, 1, 1, 1);

    GtkWidget *footer = named_box(GTK_ORIENTATION_VERTICAL, 6, "programme-queue-toolbar");
    add_class(footer, "programme-queue-footer");
    GtkWidget *order = named_box(GTK_ORIENTATION_HORIZONTAL, 4, "programme-square-controls");
    const gchar *order_text[] = {"⇈", "↑", "↓", "⇊"};
    const gchar *order_tips[] = {"En üste taşı", "Yukarı taşı", "Aşağı taşı", "En alta taşı"};
    const gchar *order_ids[] = {"programme-first", "programme-up", "programme-down", "programme-last"};
    for (guint i = 0; i < G_N_ELEMENTS(order_text); ++i) {
        GtkWidget *control = button(order, order_text[i], "programme-square", ctx, NULL);
        gtk_widget_set_name(control, order_ids[i]);
        g_object_set_data(G_OBJECT(control), "programme-order-action", GUINT_TO_POINTER(i));
        g_signal_connect(control, "clicked", G_CALLBACK(programme_order), view);
        gtk_widget_set_tooltip_text(control, order_tips[i]);
    }
    /* Kılavuz s.10/s.16: the view settings at the far right of this row. */
    GtkWidget *settings = radios_ui_button("⚙ Ayarlar");
    add_class(settings, "ld-btn"); add_class(settings, "programme-view-settings");
    gtk_widget_set_name(settings, "programme-view-settings");
    gtk_widget_set_tooltip_text(settings, "Event ve içerik çekme/gösterme adetleri, yenilenme aralığı");
    g_signal_connect(settings, "clicked", G_CALLBACK(programme_view_settings_clicked), view);
    gtk_box_pack_end(GTK_BOX(order), settings, FALSE, FALSE, 0);
    pack(footer, order, FALSE);
    GtkWidget *events = named_box(GTK_ORIENTATION_HORIZONTAL, 5, "ld-add-buttons");
    gtk_widget_set_no_show_all(events, TRUE);
    gtk_widget_hide(events);
    const gchar *event_text[] = {"+ Müzik eventi", "+ Anons eventi", "+ Reklam eventi"};
    const gchar *event_style[] = {"ld-type--music", "ld-type--announcement", "ld-type--advert"};
    const gchar *event_ids[] = {"programme-add-music", "programme-add-announcement", "programme-add-advert"};
    const gchar *event_kinds[] = {"music", "announcement", "advertisement"};
    for (guint i = 0; i < G_N_ELEMENTS(event_text); ++i) {
        GtkWidget *control = button(events, event_text[i], "ld-btn", ctx, NULL);
        g_object_set_data(G_OBJECT(control), "programme-event-kind", (gpointer)event_kinds[i]);
        g_signal_connect(control, "clicked", G_CALLBACK(programme_add_event), view);
        gtk_widget_set_name(control, event_ids[i]);
        add_class(control, "ld-add-btn");
        add_class(control, event_style[i]);
    }
    gtk_widget_set_halign(events, GTK_ALIGN_END);
    pack(footer, events, TRUE);
    gtk_grid_attach(GTK_GRID(queue), footer, 0, 2, 2, 1);
    if (ctx->playout) {
        view->playout_status = radios_ui_label("Yerel yürütücü beklemede", "ld-head-meta"); gtk_widget_set_name(view->playout_status, "programme-playout-status");
        gtk_label_set_ellipsize(GTK_LABEL(view->playout_status), PANGO_ELLIPSIZE_END); pack(body, view->playout_status, FALSE);
    }
    return panel;
}

static gboolean history_tick(gpointer data)
{
    BroadcastView *view = data; if (view->disposed || !view->history_table) return G_SOURCE_REMOVE;
    JsonNode *history = radios_playout_ui_history(programme_context(view)); if (!history) return G_SOURCE_CONTINUE;
    JsonArray *entries = json_object_get_array_member(json_node_get_object(history), "entries"); guint count = entries ? json_array_get_length(entries) : 0;
    JsonObject *last = count ? json_array_get_object_element(entries, count - 1) : NULL;
    gchar *latest = g_strdup_printf("%s|%s|%s|%" G_GINT64_FORMAT, radios_json_string(last, "media_id", ""), radios_json_string(last, "version_id", ""),
        radios_json_string(last, "started_utc", ""), radios_json_int(last, "position_frames", 0));
    if (count == view->history_count && g_strcmp0(latest, view->history_latest) == 0) { g_free(latest); json_node_free(history); return G_SOURCE_CONTINUE; }
    view->history_count = count; g_free(view->history_latest); view->history_latest = latest;
    GtkListStore *store = g_object_get_data(G_OBJECT(view->history_table), "list-store"); gtk_list_store_clear(store);
    for (guint i = count; i > 0 && count - i < 100; --i) {
        JsonObject *entry = json_array_get_object_element(entries, i - 1); GtkTreeIter iter;
        const gchar *stamp = radios_json_string(entry, "started_utc", ""); GDateTime *utc = g_date_time_new_from_iso8601(stamp, NULL);
        gchar *time = utc ? g_date_time_format(utc, "%H:%M:%S UTC") : g_strdup(stamp); if (utc) g_date_time_unref(utc);
        gint64 milliseconds = radios_json_int(entry, "position_frames", 0) / 48;
        gchar *position = g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, milliseconds / 60000, (milliseconds / 1000) % 60);
        gtk_list_store_append(store, &iter); gtk_list_store_set(store, &iter, 0, time, 1, radios_json_string(entry, "artist", ""),
            2, radios_json_string(entry, "title", radios_json_string(entry, "media_id", "")), 3, position, -1); g_free(time); g_free(position);
    }
    json_node_free(history); return G_SOURCE_CONTINUE;
}
static void history_export_response(GtkDialog *dialog, gint response, gpointer data)
{
    BroadcastView *view = g_object_get_data(G_OBJECT(data), "radios-broadcast-view");
    if (response == GTK_RESPONSE_ACCEPT && view && !view->disposed) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)); GError *error = NULL;
        if (filename && g_file_set_contents(filename, g_object_get_data(G_OBJECT(dialog), "history-export"), -1, &error)) programme_notice(view, "Yerel oynatma geçmişi kaydedildi.");
        else programme_notice(view, error ? error->message : "Dosya adı seçilmedi.");
        g_clear_error(&error); g_free(filename);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void history_export(GtkButton *button, gpointer data)
{
    (void)button; BroadcastView *view = data; if (view->disposed) return;
    JsonNode *history = radios_playout_ui_history(programme_context(view)); if (!history) return;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Yerel oynatma geçmişini kaydet", view->ctx->parent, GTK_FILE_CHOOSER_ACTION_SAVE,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL); gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE); gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "RadioS-oynatma-gecmisi.json");
    g_object_set_data_full(G_OBJECT(dialog), "history-export", json_to_string(history, TRUE), g_free); json_node_free(history);
    g_signal_connect_object(dialog, "response", G_CALLBACK(history_export_response), view->page, 0); gtk_widget_show(dialog);
}
static GtkWidget *
history_view(BroadcastView *view)
{
    RadioSViewContext *ctx = view->ctx;
    GtkWidget *history = named_box(GTK_ORIENTATION_VERTICAL, 5, "broadcast-history");
    add_class(history, "ops-card");
    add_class(history, "ops-history");
    GtkWidget *heading = named_box(GTK_ORIENTATION_HORIZONTAL, 8, "ops-card-heading");
    pack(heading, radios_ui_label("Yerel oynatma geçmişi", "heading"), TRUE);
    GtkWidget *download = button(heading, "Kaydı indir", NULL, ctx, ctx->playout ? NULL : "Yayın geçmişini indirmek için veri servisi bağlantısı gerekiyor.");
    if (ctx->playout) { gtk_widget_set_name(download, "programme-history-export"); g_signal_connect(download, "clicked", G_CALLBACK(history_export), view); }
    GtkWidget *stack = gtk_stack_new();
    GtkWidget *tabs = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(tabs), GTK_STACK(stack));
    add_class(tabs, "ops-history-tabs");
    pack(heading, tabs, FALSE);
    pack(history, heading, FALSE);
    pack(history, stack, TRUE);
    const gchar *headers[] = {"Saat", "Sanatçı", "Parça", "Konum"};
    GtkWidget *actual = radios_ui_table(headers, G_N_ELEMENTS(headers));
    view->history_table = actual; gtk_widget_set_name(actual, "programme-history-table");
    gtk_widget_set_tooltip_text(actual, "Yerel ses çıkışında başlayan öğeler ve gözlenen oynatma konumu.");
    add_class(actual, "ops-table-scroll");
    GtkWidget *tree = g_object_get_data(G_OBJECT(actual), "tree-view");
    add_class(tree, "ops-table");
    for (guint i = 0; i < G_N_ELEMENTS(headers); ++i)
        gtk_tree_view_column_set_expand(gtk_tree_view_get_column(GTK_TREE_VIEW(tree),
                                                                (gint)i), TRUE);
    gtk_widget_set_size_request(actual, -1, 62);
    gtk_stack_add_titled(GTK_STACK(stack), actual, "actual", "Yerel ses");
    GtkWidget *draft = radios_ui_label("—", "ops-audit-list");
    gtk_widget_set_valign(draft, GTK_ALIGN_START);
    gtk_stack_add_titled(GTK_STACK(stack), draft, "draft", "Düzenleme kaydı");
    return history;
}

static void history_dialog_destroyed(GtkWidget *dialog, gpointer data)
{
    (void)dialog; BroadcastView *view = data;
    if (view->history_timer) { g_source_remove(view->history_timer); view->history_timer = 0; }
    view->history_dialog = NULL; view->history_table = NULL;
    view->history_count = G_MAXUINT; g_clear_pointer(&view->history_latest, g_free);
}

static void history_open(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; RadioSViewContext *ctx = data;
    BroadcastView *view = g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context");
    if (!view || view->disposed) return;
    if (view->history_dialog) { gtk_window_present(GTK_WINDOW(view->history_dialog)); return; }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Yerel oynatma geçmişi", ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Kapat", GTK_RESPONSE_CLOSE, NULL);
    view->history_dialog = dialog;
    gtk_widget_set_name(dialog, "programme-history-dialog"); add_class(dialog, "radios");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 860, 460);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    pack(content, history_view(view), TRUE);
    g_signal_connect(dialog, "destroy", G_CALLBACK(history_dialog_destroyed), view);
    g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);
    view->history_count = G_MAXUINT; g_clear_pointer(&view->history_latest, g_free);
    if (ctx->playout) {
        history_tick(view);
        view->history_timer = g_timeout_add(500, history_tick, view);
    }
    gtk_widget_show_all(dialog); gtk_window_present(GTK_WINDOW(dialog));
}

GtkWidget *radios_broadcast_history_button_new(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx && ctx->parent, NULL);
    GtkWidget *control = gtk_button_new();
    gtk_widget_set_name(control, "header-playback-history");
    gtk_widget_set_tooltip_text(control, "Yerel oynatma geçmişi");
    atk_object_set_name(gtk_widget_get_accessible(control), "Yerel oynatma geçmişi");
    gtk_button_set_image(GTK_BUTTON(control), gtk_image_new_from_icon_name("document-open-recent-symbolic", GTK_ICON_SIZE_BUTTON));
    add_class(control, "header-settings-button");
    g_signal_connect(control, "clicked", G_CALLBACK(history_open), ctx);
    return control;
}

typedef struct { BroadcastView *view; guint generation; JsonNode *programmes; } CatalogRead;
static void catalog_read_free(gpointer value)
{ CatalogRead *read = value; if (read->programmes) json_node_free(read->programmes); g_free(read); }
static gchar *catalog_duration(gint64 milliseconds)
{ gint64 seconds = (milliseconds + 500) / 1000; return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, seconds / 60, seconds % 60); }
static JsonObject *catalog_object(JsonObject *object, const gchar *key)
{ JsonNode *node = object ? json_object_get_member(object, key) : NULL; return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL; }
static void catalog_message(BroadcastView *view, const gchar *message)
{ gtk_label_set_text(GTK_LABEL(view->catalog_status), message); programme_notice(view, message); }
static gboolean catalog_matches(const gchar *value, const gchar *query)
{
    if (!query || !*query) return TRUE;
    gchar *haystack = g_utf8_casefold(value, -1), *needle = g_utf8_casefold(query, -1);
    gboolean found = strstr(haystack, needle) != NULL; g_free(haystack); g_free(needle); return found;
}

/* Rows of the shared catalogue table: saved programmes (Programlar) first, then media. */
static void catalog_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; (void)owner; CatalogRead *read = data; BroadcastView *view = read->view;
    if (read->generation != view->catalog_generation) return;
    if (error) { gtk_label_set_text(GTK_LABEL(view->catalog_status), error->message); return; }
    if (!result || !JSON_NODE_HOLDS_OBJECT(result)) return;
    JsonObject *object = json_node_get_object(result);
    JsonNode *media_node = json_object_get_member(object, "records");
    if (!media_node || !JSON_NODE_HOLDS_ARRAY(media_node)) return;
    JsonArray *records = json_array_new(), *media = json_node_get_array(media_node);
    const gchar *query = gtk_entry_get_text(GTK_ENTRY(view->query)); guint saved = 0;
    JsonObject *programmes = read->programmes && JSON_NODE_HOLDS_OBJECT(read->programmes) ? json_node_get_object(read->programmes) : NULL;
    JsonNode *documents = programmes ? json_object_get_member(programmes, "records") : NULL;
    for (guint i = 0; documents && JSON_NODE_HOLDS_ARRAY(documents) && i < json_array_get_length(json_node_get_array(documents)); ++i) {
        JsonObject *document = json_array_get_object_element(json_node_get_array(documents), i);
        const gchar *name = radios_json_string(document, "name", "");
        /* The working draft is the flow itself, not a catalogue entry. */
        JsonObject *config = catalog_object(document, "config");
        if (g_str_equal(name, "Mevcut program") || (config && json_object_has_member(config, "queue_player")) || !catalog_matches(name, query)) continue;
        JsonObject *wrapper = json_object_new(); json_object_set_object_member(wrapper, "programme", json_object_ref(document));
        json_array_add_object_element(records, wrapper); ++saved;
    }
    for (guint i = 0; i < json_array_get_length(media); ++i) json_array_add_element(records, json_node_copy(json_array_get_element(media, i)));
    g_clear_pointer(&view->catalog_records, json_node_free);
    view->catalog_records = json_node_new(JSON_NODE_OBJECT); json_node_take_object(view->catalog_records, json_object_new());
    json_object_set_array_member(json_node_get_object(view->catalog_records), "records", records);
    GtkListStore *store = g_object_get_data(G_OBJECT(view->catalog_table), "list-store"); gtk_list_store_clear(store);
    for (guint i = 0; i < json_array_get_length(records); ++i) {
        JsonObject *row = json_array_get_object_element(records, i), *programme = catalog_object(row, "programme"); GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        if (programme) {
            JsonObject *config = catalog_object(programme, "config");
            RadioSProgrammeTree *tree = radios_programme_tree_new(config); gint64 total = 0;
            for (guint j = 0; j < tree->blocks->len; ++j) total += radios_programme_block_duration(g_ptr_array_index(tree->blocks, j));
            gchar *duration = catalog_duration(total), *source = g_strdup_printf("Program · %u içerik", radios_programme_tree_leaf_count(tree));
            gtk_list_store_set(store, &iter, 0, "", 1, radios_json_string(programme, "name", ""), 2, radios_json_string(config, "date", ""),
                3, "Program", 4, duration, 5, source, -1);
            g_free(duration); g_free(source); radios_programme_tree_free(tree);
            continue;
        }
        JsonObject *item = catalog_object(row, "media"), *version = catalog_object(row, "version");
        gchar *duration = catalog_duration(radios_json_int(version, "duration_ms", 0));
        gtk_list_store_set(store, &iter, 0, radios_json_string(item, "artist", ""), 1, radios_json_string(item, "title", ""),
            2, radios_json_string(item, "album", ""), 3, radios_programme_kind_label(radios_json_string(item, "kind", "")),
            4, duration, 5, "Medya arşivi", -1);
        g_free(duration);
    }
    gchar *count = g_strdup_printf("%u / %" G_GINT64_FORMAT " kayıt", json_array_get_length(records), radios_json_int(object, "total", 0) + saved);
    gtk_label_set_text(GTK_LABEL(view->catalog_count), count); g_free(count);
    gtk_label_set_text(GTK_LABEL(view->catalog_status), "");
    catalog_actions_update(view);
}

/* Owned, ordered snapshots: GTK keeps selected paths in model order. */
static JsonArray *catalog_selected_records(BroadcastView *view)
{
    JsonArray *selected = json_array_new();
    if (!view->catalog_records) return selected;
    GtkTreeView *tree = g_object_get_data(G_OBJECT(view->catalog_table), "tree-view");
    GList *paths = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(tree), NULL);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(view->catalog_records), "records");
    for (GList *entry = paths; entry; entry = entry->next) {
        gint *indices = gtk_tree_path_get_indices(entry->data);
        if (indices && indices[0] >= 0 && (guint)indices[0] < json_array_get_length(rows))
            json_array_add_element(selected, radios_programme_clone(json_array_get_element(rows, (guint)indices[0])));
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free); return selected;
}
static guint catalog_selected_count(BroadcastView *view)
{
    GtkTreeView *tree = g_object_get_data(G_OBJECT(view->catalog_table), "tree-view");
    return (guint)gtk_tree_selection_count_selected_rows(gtk_tree_view_get_selection(tree));
}
/* Singular actions require precisely one record; never silently pick a batch's first row. */
static JsonObject *catalog_selected_record(BroadcastView *view)
{
    if (catalog_selected_count(view) != 1 || !view->catalog_records) return NULL;
    GtkTreeView *tree = g_object_get_data(G_OBJECT(view->catalog_table), "tree-view");
    GList *paths = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(tree), NULL);
    gint *indices = paths ? gtk_tree_path_get_indices(paths->data) : NULL;
    JsonArray *rows = json_object_get_array_member(json_node_get_object(view->catalog_records), "records");
    JsonObject *record = indices && indices[0] >= 0 && (guint)indices[0] < json_array_get_length(rows) ?
        json_array_get_object_element(rows, (guint)indices[0]) : NULL;
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free); return record;
}
typedef enum { CATALOG_NONE, CATALOG_MEDIA, CATALOG_PROGRAMME, CATALOG_PLAYLIST, CATALOG_MIXED } CatalogSelection;
static CatalogSelection catalog_selection_kind(BroadcastView *view)
{
    if (!catalog_selected_count(view)) return CATALOG_NONE;
    JsonArray *selected = catalog_selected_records(view); CatalogSelection kind = CATALOG_NONE;
    for (guint i = 0; i < json_array_get_length(selected); ++i) {
        JsonObject *record = json_array_get_object_element(selected, i);
        CatalogSelection next = catalog_object(record, "media") ? CATALOG_MEDIA : catalog_object(record, "programme") ? CATALOG_PROGRAMME :
            catalog_object(record, "playlist") ? CATALOG_PLAYLIST : CATALOG_NONE;
        if (!i) kind = next;
        else if (kind != next) { kind = CATALOG_MIXED; break; }
    }
    json_array_unref(selected); return kind;
}
/* Media id of the selected archive row; saved programmes have none. */
static const gchar *catalog_selected(BroadcastView *view)
{ JsonObject *media = catalog_object(catalog_selected_record(view), "media"); return media ? radios_json_string(media, "_id", "") : NULL; }
static JsonObject *catalog_selected_programme(BroadcastView *view)
{ return catalog_object(catalog_selected_record(view), "programme"); }
static void catalog_selection(GtkTreeSelection *selection, gpointer data)
{
    (void)selection; BroadcastView *view = data; ++view->catalog_selection_generation; catalog_actions_update(view);
    if (catalog_selection_kind(view) == CATALOG_MIXED)
        catalog_message(view, "Birlikte eklemek için aynı türden içerikler seçin.");
}

/* Buttons and the selected-content details follow the catalogue mode. */
static void catalog_actions_update(BroadcastView *view)
{
    if (!view->catalog_insert) return;
    gboolean clean = !view->disposed && !(view->queue_slot == 0 && general_mode(view)) && radios_data_connected(view->ctx) && radios_data_document_revision(view->programme_document) > 0;
    guint selected_count = catalog_selected_count(view); CatalogSelection kind = catalog_selection_kind(view);
    JsonObject *record = catalog_selected_record(view), *programme = catalog_object(record, "programme"), *media = catalog_object(record, "media");
    JsonObject *playlist = catalog_object(record, "playlist");
    GtkWidget *tree = g_object_get_data(G_OBJECT(view->catalog_table), "tree-view");
    const GtkTargetEntry *target = &radios_drag_targets(NULL)[(kind == CATALOG_PROGRAMME && selected_count == 1 ? RADIOS_DRAG_INFO_PROGRAMME : RADIOS_DRAG_INFO_MEDIA) - 1];
    gboolean draggable = kind == CATALOG_MEDIA || kind == CATALOG_PROGRAMME;
    GtkTargetList *targets = gtk_target_list_new(draggable ? target : NULL, draggable ? 1 : 0);
    gtk_drag_source_set_target_list(tree, targets); gtk_target_list_unref(targets);
    gboolean selected = kind == CATALOG_MEDIA || kind == CATALOG_PROGRAMME, idle = !view->catalogue_busy;
    gtk_button_set_label(GTK_BUTTON(view->catalog_insert), kind == CATALOG_PROGRAMME ? selected_count > 1 ? "Programları ekle" : "Programı ekle" : selected_count > 1 ? "Seçili parçaları ekle" : "Bloğa ekle");
    gtk_button_set_label(GTK_BUTTON(view->catalog_replace), programme ? "Mevcut taslağı değiştir" : "Seçiliyle değiştir");
    gtk_button_set_label(GTK_BUTTON(view->catalog_edit), playlist ? "Playlisti aç" : "Düzenle");
    gtk_widget_set_visible(view->catalog_edit, !programme);
    gtk_widget_set_sensitive(view->catalog_edit, selected_count == 1 && (media || playlist) && idle);
    gtk_widget_set_sensitive(view->catalog_insert, selected && clean && idle);
    gtk_widget_set_sensitive(view->catalog_replace, selected_count == 1 && selected && clean && idle && (programme || programme_leaf_selected(view)));
    if (view->catalog_create_playlist) radios_catalogue_playlist_button_refresh(view->catalog_create_playlist);
    JsonObject *version = catalog_object(record, "version"), *config = catalog_object(programme, "config");
    gchar *duration = programme || media ? NULL : g_strdup("—");
    if (!duration) {
        gint64 milliseconds = radios_json_int(version, "duration_ms", 0);
        if (programme) {
            RadioSProgrammeTree *tree = radios_programme_tree_new(config);
            for (guint j = 0; j < tree->blocks->len; ++j) milliseconds += radios_programme_block_duration(g_ptr_array_index(tree->blocks, j));
            radios_programme_tree_free(tree);
        }
        duration = milliseconds > 0 ? catalog_duration(milliseconds) : g_strdup("—");
    }
    const gchar *title = programme ? radios_json_string(programme, "name", "") : media ? radios_json_string(media, "title", "") :
        playlist ? radios_json_string(playlist, "name", "") : "İçerik seçilmedi";
    const gchar *values[] = {programme ? "—" : media ? radios_json_string(media, "artist", "") : "—",
        programme ? radios_json_string(config, "date", "") : media ? radios_json_string(media, "album", "") : "—", duration,
        programme ? "Program taslağı" : media ? "Medya arşivi" : "—"};
    gtk_label_set_text(GTK_LABEL(view->catalog_title), *title ? title : "—");
    for (guint i = 0; i < G_N_ELEMENTS(values); ++i) gtk_label_set_text(GTK_LABEL(view->catalog_details[i]), values[i] && *values[i] ? values[i] : "—");
    if (selected_count > 1) {
        gchar *summary = g_strdup_printf("%u içerik seçildi", selected_count);
        gtk_label_set_text(GTK_LABEL(view->catalog_title), summary); g_free(summary);
        for (guint i = 0; i < G_N_ELEMENTS(view->catalog_details); ++i) gtk_label_set_text(GTK_LABEL(view->catalog_details[i]), "—");
    }
    g_free(duration);
}

typedef struct {
    gchar *document_id, *event_id, *store_id, *station_id, *user_id, *media_id, *version_id, *media_file_id, *sha256;
    gint64 revision, media_revision, version_revision;
    guint intent, generation, selection_generation, batch_index;
    gint refs;
    gboolean replace, placed;
    RadioSProgrammePlacement placement;
    JsonNode *config;
    JsonArray *records;
    GPtrArray *leaves;
} CatalogueIntent;
static void catalogue_intent_free(gpointer value)
{
    CatalogueIntent *intent = value;
    if (--intent->refs) return;
    g_free(intent->document_id); g_free(intent->event_id); g_free(intent->store_id); g_free(intent->station_id); g_free(intent->user_id);
    g_free(intent->media_id); g_free(intent->version_id); g_free(intent->media_file_id); g_free(intent->sha256);
    json_node_free(intent->config);
    if (intent->records) json_array_unref(intent->records);
    if (intent->leaves) g_ptr_array_unref(intent->leaves);
    g_free(intent);
}
static CatalogueIntent *catalogue_capture(BroadcastView *view)
{
    if (view->disposed || view->catalogue_busy || radios_data_document_revision(view->programme_document) <= 0) return NULL;
    RadioSDataClient *client = radios_data_client_acquire(view->ctx); if (!client) return NULL;
    CatalogueIntent *intent = g_new0(CatalogueIntent, 1); intent->refs = 1;
    intent->document_id = g_strdup(radios_data_document_id(view->programme_document));
    intent->revision = radios_data_document_revision(view->programme_document); intent->event_id = g_strdup(view->selected_event);
    intent->config = programme_copy(view->programme_config); intent->intent = ++view->catalogue_intent;
    intent->store_id = g_strdup(radios_data_client_store_id(client)); intent->station_id = g_strdup(radios_data_client_station_id(client));
    intent->user_id = g_strdup(radios_data_client_user_id(client)); radios_data_client_unref(client); return intent;
}
static gboolean catalogue_identity(BroadcastView *view, CatalogueIntent *intent)
{
    RadioSDataClient *client = radios_data_client_acquire(view->ctx);
    gboolean same = !view->disposed && client &&
        !g_strcmp0(intent->store_id, radios_data_client_store_id(client)) && !g_strcmp0(intent->user_id, radios_data_client_user_id(client)) &&
        !g_strcmp0(intent->station_id, radios_data_station_id(view->ctx));
    if (client) radios_data_client_unref(client);
    return same;
}
static gboolean catalogue_current(BroadcastView *view, CatalogueIntent *intent)
{
    return catalogue_identity(view, intent) && intent->intent == view->catalogue_intent &&
        !g_strcmp0(intent->document_id, radios_data_document_id(view->programme_document)) &&
        intent->revision == radios_data_document_revision(view->programme_document) &&
        !g_strcmp0(intent->event_id, view->selected_event) && json_node_equal(intent->config, view->programme_config);
}
static gboolean catalogue_save(BroadcastView *view, JsonNode *next)
{
    if (!view->queue_slot && general_mode(view)) { json_node_free(next); return FALSE; }
    queue_binding(view, json_node_get_object(next));
    radios_data_document_dirty(view->programme_document, TRUE);
    if (!radios_data_document_save(view->programme_document, next)) { json_node_free(next); return FALSE; }
    g_clear_pointer(&view->programme_config, json_node_free); view->programme_config = next; programme_render(view); return TRUE;
}
/* The selected content keeps its id, event, start time and note; its source changes. */
static gboolean programme_replace_leaf(BroadcastView *view, JsonObject *source)
{
    RadioSProgrammeTree *tree = programme_tree(view); guint block = 0; gint leaf = -1;
    if (!programme_item(view, tree, &block, &leaf) || leaf < 0) { radios_programme_tree_free(tree); json_object_unref(source); return FALSE; }
    GPtrArray *leaves = ((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, block))->leaves;
    JsonObject *replacement = radios_programme_clone_object(g_ptr_array_index(leaves, (guint)leaf));
    const gchar *sources[] = {"media_id", "version_id", "media_file_id", "media_sha256"};
    for (guint i = 0; i < G_N_ELEMENTS(sources); ++i) json_object_remove_member(replacement, sources[i]);
    JsonObjectIter iter; const gchar *name; JsonNode *member;
    json_object_iter_init(&iter, source);
    while (json_object_iter_next(&iter, &name, &member))
        if (!g_str_equal(name, "id") && !g_str_equal(name, "start_time") && !g_str_equal(name, "note"))
            json_object_set_member(replacement, name, radios_programme_clone(member));
    json_object_unref(source);
    g_ptr_array_remove_index(leaves, (guint)leaf); g_ptr_array_insert(leaves, leaf, replacement);
    gchar *id = g_strdup(radios_json_string(replacement, "id", ""));
    gboolean saved = programme_commit(view, tree, id); g_free(id); return saved;
}
static gboolean catalog_records_equal(JsonArray *left, JsonArray *right)
{
    if (json_array_get_length(left) != json_array_get_length(right)) return FALSE;
    for (guint i = 0; i < json_array_get_length(left); ++i)
        if (!json_node_equal(json_array_get_element(left, i), json_array_get_element(right, i))) return FALSE;
    return TRUE;
}
static void catalog_intent_media(CatalogueIntent *intent, JsonObject *record)
{
    JsonObject *media = catalog_object(record, "media"), *version = catalog_object(record, "version");
    g_free(intent->media_id); g_free(intent->version_id); g_free(intent->media_file_id); g_free(intent->sha256);
    intent->media_id = g_strdup(radios_json_string(media, "_id", ""));
    intent->media_revision = radios_json_int(media, "_rev", 0); intent->version_revision = radios_json_int(version, "_rev", 0);
    intent->version_id = g_strdup(radios_json_string(version, "_id", "")); intent->media_file_id = g_strdup(radios_json_string(version, "media_file_id", ""));
    intent->sha256 = g_strdup(radios_json_string(version, "sha256", ""));
}
static void catalog_media_ready(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value);
/* Each request owns a reference, including rejected requests. A batch commits only at its last verified row. */
static gboolean catalog_media_request_next(BroadcastView *view, CatalogueIntent *intent)
{
    ++intent->refs;
    gboolean started = radios_media_library_get(view->ctx, view->page, intent->media_id, catalog_media_ready, intent, catalogue_intent_free);
    if (!started) {
        view->catalogue_busy = FALSE; catalog_actions_update(view);
        catalog_message(view, "Medya doğrulaması başlatılamadı; program korunuyor.");
    }
    return started;
}
static void catalog_media_ready(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    CatalogueIntent *intent = value; BroadcastView *view = g_object_get_data(G_OBJECT(owner), "radios-broadcast-view");
    if (!view || view->disposed || intent->intent != view->catalogue_intent) return;
    if (error) { view->catalogue_busy = FALSE; catalog_actions_update(view); catalog_message(view, error->message); return; }
    JsonArray *selected = intent->records ? catalog_selected_records(view) : NULL;
    gboolean same_selection = intent->selection_generation == view->catalog_selection_generation &&
        (intent->records ? catalog_records_equal(intent->records, selected) : !g_strcmp0(intent->media_id, catalog_selected(view)));
    if (selected) json_array_unref(selected);
    if (!catalogue_current(view, intent) || intent->generation != view->catalog_generation || !same_selection) {
        view->catalogue_busy = FALSE; catalog_actions_update(view);
        catalog_message(view, "Hedef program, seçili öğe, katalog veya veri kimliği değişti; hiçbir öğe eklenmedi/değiştirilmedi."); return;
    }
    JsonObject *detail = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    JsonObject *media = catalog_object(detail, "media"), *version = catalog_object(detail, "version"), *analysis = catalog_object(detail, "analysis");
    if (!media || !version || !analysis || g_strcmp0(radios_json_string(media, "_id", ""), intent->media_id) ||
        g_strcmp0(radios_json_string(media, "station_id", ""), radios_data_station_id(ctx)) ||
        g_strcmp0(radios_json_string(media, "status", ""), "active") || radios_json_int(media, "_rev", 0) != intent->media_revision ||
        radios_json_int(version, "_rev", 0) != intent->version_revision || g_strcmp0(radios_json_string(version, "_id", ""), intent->version_id) ||
        g_strcmp0(radios_json_string(version, "media_file_id", ""), intent->media_file_id) || g_strcmp0(radios_json_string(version, "sha256", ""), intent->sha256) ||
        g_strcmp0(radios_json_string(analysis, "sha256", ""), intent->sha256) || !json_object_get_boolean_member(version, "verified") ||
        !json_object_get_boolean_member(version, "object_available") || radios_json_int(version, "duration_ms", 0) <= 0) {
        view->catalogue_busy = FALSE; catalog_actions_update(view);
        catalog_message(view, "Seçili medya değişti veya gerçek SHA doğrulaması kullanılamıyor; katalog aramasını yenileyin. Program korunuyor."); return;
    }
    JsonObject *event = json_object_new(); gchar *id = g_uuid_string_random(); json_object_set_string_member(event, "id", id); g_free(id);
    const gchar *blank[] = {"start_time", "primary_file", "backup_file", "note"};
    for (guint i = 0; i < G_N_ELEMENTS(blank); ++i) json_object_set_string_member(event, blank[i], "");
    const gchar *keys[] = {"title", "artist", "album", "kind"}; const glong limits[] = {160,120,120,40};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) { const gchar *text = radios_json_string(media, keys[i], "");
        gchar *bounded = g_utf8_substring(text, 0, MIN(g_utf8_strlen(text, -1), limits[i])); json_object_set_string_member(event, keys[i], bounded); g_free(bounded); }
    if (!radios_programme_kind_valid(radios_json_string(event, "kind", ""))) json_object_set_string_member(event, "kind", "music");
    json_object_set_int_member(event, "duration_ms", radios_json_int(version, "duration_ms", 0));
    json_object_set_string_member(event, "version_label", "Taslak");
    json_object_set_string_member(event, "media_id", intent->media_id); json_object_set_string_member(event, "version_id", intent->version_id);
    json_object_set_string_member(event, "media_file_id", intent->media_file_id); json_object_set_string_member(event, "media_sha256", intent->sha256);
    if (intent->records) {
        g_ptr_array_add(intent->leaves, event);
        if (++intent->batch_index < json_array_get_length(intent->records)) {
            catalog_intent_media(intent, json_array_get_object_element(intent->records, intent->batch_index));
            catalog_media_request_next(view, intent); return;
        }
        GPtrArray *leaves = intent->leaves; intent->leaves = NULL;
        view->catalogue_busy = FALSE;
        gboolean saved = programme_insert_leaves(view, leaves, intent->placement, NULL, NULL);
        catalog_actions_update(view);
        catalog_message(view, saved ? "Seçili medyalar yayın sırasına eklendi." : "Program kaydı gönderilemedi; mevcut sıra korunuyor."); return;
    }
    view->catalogue_busy = FALSE; catalog_actions_update(view);
    gboolean saved;
    if (intent->replace) saved = programme_replace_leaf(view, event);
    else {
        RadioSProgrammeTree *tree = programme_tree(view);
        RadioSProgrammePlacement placement = intent->placed ? intent->placement : programme_target(view, tree);
        radios_programme_tree_free(tree);
        GPtrArray *leaves = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref); g_ptr_array_add(leaves, event);
        saved = programme_insert_leaves(view, leaves, placement, NULL, NULL);
    }
    catalog_message(view, saved ? "Medya yayın sırasına eklendi." : "Program kaydı gönderilemedi; mevcut sıra korunuyor.");
}
/* Verifies the real file and SHA of a media record before it enters the flow. */
static gboolean catalog_media_verify(BroadcastView *view, JsonObject *record, gboolean replace, const RadioSProgrammePlacement *placement)
{
    JsonObject *media = catalog_object(record, "media"), *version = catalog_object(record, "version");
    CatalogueIntent *intent = media && version ? catalogue_capture(view) : NULL;
    if (!intent) { catalog_message(view, "Önce programı kaydedin ve katalogdan medya seçin."); return FALSE; }
    intent->replace = replace;
    if (replace && !programme_leaf_selected(view)) { catalogue_intent_free(intent); catalog_message(view, "Değiştirilecek içeriği seçin."); return FALSE; }
    if (placement) { intent->placed = TRUE; intent->placement = *placement; }
    intent->generation = view->catalog_generation; intent->selection_generation = view->catalog_selection_generation;
    catalog_intent_media(intent, record); view->catalogue_busy = TRUE; catalog_actions_update(view);
    catalog_message(view, "Seçili medyanın gerçek dosyası ve SHA özeti doğrulanıyor…");
    gboolean started = catalog_media_request_next(view, intent); catalogue_intent_free(intent); return started;
}
static gboolean catalog_media_batch(BroadcastView *view, JsonArray *records, const RadioSProgrammePlacement *placement)
{
    CatalogueIntent *intent = json_array_get_length(records) ? catalogue_capture(view) : NULL;
    if (!intent) { catalog_message(view, "Önce programı kaydedin ve katalogdan medya seçin."); return FALSE; }
    for (guint i = 0; i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i);
        if (!catalog_object(record, "media") || !catalog_object(record, "version")) {
            catalogue_intent_free(intent); catalog_message(view, "Birlikte eklemek için medya arşivinden parçalar seçin."); return FALSE;
        }
    }
    intent->records = json_array_ref(records);
    intent->leaves = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
    intent->generation = view->catalog_generation; intent->selection_generation = view->catalog_selection_generation;
    RadioSProgrammeTree *tree = programme_tree(view);
    intent->placement = placement ? *placement : programme_target(view, tree); radios_programme_tree_free(tree);
    catalog_intent_media(intent, json_array_get_object_element(records, 0));
    view->catalogue_busy = TRUE; catalog_actions_update(view);
    catalog_message(view, "Seçili medyaların gerçek dosyaları ve SHA özetleri doğrulanıyor…");
    gboolean started = catalog_media_request_next(view, intent); catalogue_intent_free(intent); return started;
}
/* A content after the selected event, or as a new last event. */
static RadioSProgrammePlacement programme_after_event(BroadcastView *view)
{
    RadioSProgrammeTree *tree = programme_tree(view); guint block = 0; gint leaf = -1;
    RadioSProgrammePlacement placement = {-1, programme_item(view, tree, &block, &leaf) ? block + 1 : tree->blocks->len};
    radios_programme_tree_free(tree); return placement;
}

/* A local event is resolved independently, then appended in one draft edit.
 * Re-selecting/editing a queue while it is being prepared cancels that insert. */
typedef struct {
    BroadcastView *view;
    RadioSDocument *document;
    JsonNode *baseline;
    gchar *document_id;
    gint64 revision;
    guint slot;
    RadioSProgrammePlacement placement;
    gchar *target_block;
    JsonObject *source_config;
    gboolean detached;
    GWeakRef dialog;
} LocalEventApply;
static void local_event_apply_free(gpointer value)
{
    LocalEventApply *job = value;
    json_node_free(job->baseline); g_weak_ref_clear(&job->dialog); g_free(job->document_id);
    g_free(job->target_block); if (job->source_config) json_object_unref(job->source_config); g_free(job);
}
static void local_event_message(LocalEventApply *job, const gchar *message)
{
    programme_notice(job->view, message);
    GtkWidget *dialog = g_weak_ref_get(&job->dialog);
    if (dialog) { radios_local_event_ui_insert_result(dialog, message); g_object_unref(dialog); }
}
static void local_event_prepared(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *programme, const GError *error, gpointer value)
{
    (void)ctx; (void)owner;
    LocalEventApply *job = value; BroadcastView *view = job->view;
    if (view->disposed) return;
    g_clear_object(&view->local_event_cancel);
    if (error) { local_event_message(job, error->message); return; }
    if (view->queue_slot != job->slot || view->programme_document != job->document ||
        g_strcmp0(radios_data_document_id(view->programme_document), job->document_id) ||
        radios_data_document_revision(view->programme_document) != job->revision ||
        !view->programme_config || !json_node_equal(job->baseline, view->programme_config)) {
        local_event_message(job, "Seçili yayın sırası değişti; yerel eventi yeniden sıraya ekleyin."); return;
    }
    if (!programme) { local_event_message(job, "Event kaynakları hazırlanamadı."); return; }
    if (job->detached) {
        JsonObject *playback = catalog_object(job->source_config, "playback");
        gint64 visible = radios_json_int(playback, "player", 0); GError *failure = NULL;
        gboolean accepted = radios_event_playout_programme_for_event(view->ctx, view->page, programme,
            visible > 0 && visible <= 32 ? (guint)visible - 1 : G_MAXUINT,
            radios_json_string(playback, "next_cluster_id", ""), job->target_block, &failure);
        if (accepted) {
            JsonObject *override = json_object_new();
            json_object_set_object_member(override, "programme", radios_programme_clone_object(programme));
            json_object_set_object_member(override, "local_event", radios_programme_clone_object(job->source_config));
            if (playback) json_object_set_object_member(override, "playback", radios_programme_clone_object(playback));
            g_hash_table_replace(view->event_overrides, g_strdup(job->target_block), override);
        }
        local_event_message(job, accepted ? "Event kaynağı kümesiz Yarı AutoDJ'ye atandı; takvim ve mevcut yayın sırası korundu." :
            failure ? failure->message : "Event başlatılamadı."); g_clear_error(&failure); return;
    }
    if (!programme_can_change(view)) return;
    RadioSProgrammeTree *incoming = radios_programme_tree_new(programme), *tree = programme_tree(view);
    guint position = job->placement.position, target = 0; gint leaf = -1;
    JsonObject *original = NULL;
    if (job->target_block) {
        if (!radios_programme_tree_find(tree, job->target_block, &target, &leaf) || leaf >= 0) {
            radios_programme_tree_free(incoming); radios_programme_tree_free(tree);
            local_event_message(job, "Atanacak event artık bu sırada bulunmuyor."); return;
        }
        original = radios_programme_clone_object(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, target))->block);
        radios_programme_tree_remove(tree, job->target_block); position = target;
    }
    if (radios_programme_tree_leaf_count(tree) + radios_programme_tree_leaf_count(incoming) > 5000) {
        if (original) json_object_unref(original);
        radios_programme_tree_free(incoming); radios_programme_tree_free(tree);
        local_event_message(job, "Program en fazla 5000 öğe içerebilir."); return;
    }
    gchar *selected = NULL;
    for (guint i = 0; i < incoming->blocks->len; ++i) {
        RadioSProgrammeBlock *source = g_ptr_array_index(incoming->blocks, i);
        RadioSProgrammeBlock *created = radios_programme_tree_add_block(tree, position + i,
            radios_json_string(source->block, "kind", "music"), radios_json_string(source->block, "title", "Yerel Event"));
        if (i == 0 && job->target_block) json_object_set_string_member(created->block, "id", job->target_block);
        if (i == 0) {
            selected = g_strdup(radios_json_string(created->block, "id", ""));
            if (original) {
                const gchar *fields[] = {"start_time", "note"};
                for (guint j = 0; j < G_N_ELEMENTS(fields); ++j)
                    json_object_set_string_member(created->block, fields[j], radios_json_string(original, fields[j], ""));
            }
            json_object_set_object_member(created->block, "local_event", radios_programme_clone_object(job->source_config));
            JsonObject *playback = catalog_object(job->source_config, "playback");
            if (playback) json_object_set_object_member(created->block, "playback", radios_programme_clone_object(playback));
        }
        for (guint j = 0; j < source->leaves->len; ++j) {
            JsonObject *entry = radios_programme_clone_object(g_ptr_array_index(source->leaves, j));
            gchar *id = g_uuid_string_random(); json_object_set_string_member(entry, "id", id); g_free(id);
            json_object_remove_member(entry, "block_id");
            radios_programme_tree_insert(tree, position + i, j, entry);
        }
    }
    radios_programme_tree_free(incoming); if (original) json_object_unref(original);
    gboolean saved = programme_commit(view, tree, selected); g_free(selected);
    if (saved && job->target_block) {
        g_clear_pointer(&view->local_event_target_baseline, json_node_free);
        view->local_event_target_baseline = programme_copy(view->programme_config);
    }
    local_event_message(job, saved ? job->target_block ? "Kaynaklar seçili evente atandı; oynatıcı ve küme seçimi kaydediliyor." :
        "Yerel Event seçili yayın sırasına eklendi." : "Yerel Event kaydı gönderilemedi; mevcut kayıt korunuyor.");
}
static gboolean local_event_insert(RadioSViewContext *ctx, JsonObject *configuration, gpointer value, GError **error)
{
    BroadcastView *view = value;
    if (view->disposed || view->local_event_cancel) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Yerel Event hazırlığı sürüyor."); return FALSE;
    }
    gboolean assigning = view->local_event_target != NULL;
    gboolean detached = assigning && !view->queue_slot && general_mode(view);
    if (assigning && (view->queue_slot != view->local_event_target_slot ||
        view->programme_document != view->local_event_target_document ||
        g_strcmp0(radios_data_document_id(view->programme_document), view->local_event_target_document_id) ||
        !view->local_event_target_baseline || !json_node_equal(view->local_event_target_baseline, view->programme_config))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Atanacak event veya yayın sırası değişti; eventi yeniden seçin."); return FALSE;
    }
    if (!detached && !view->queue_slot && general_mode(view)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
            "Event kaydedilebilir veya Kümesizde çal ile çalıştırılabilir. Takvim sırası değiştirilemez."); return FALSE;
    }
    if (!detached && (!programme_can_change(view) || !radios_data_document_revision(view->programme_document))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Seçili yayın sırasının kaydı hazır olmalı."); return FALSE;
    }
    LocalEventApply *job = g_new0(LocalEventApply, 1);
    GtkWidget *dialog = g_weak_ref_get(&view->local_event_dialog);
    g_weak_ref_init(&job->dialog, dialog); g_clear_object(&dialog);
    job->view = view; job->slot = view->queue_slot; job->document = view->programme_document;
    job->document_id = g_strdup(radios_data_document_id(job->document));
    job->revision = radios_data_document_revision(job->document);
    job->baseline = programme_copy(view->programme_config);
    job->target_block = g_strdup(view->local_event_target); job->detached = detached;
    job->source_config = radios_programme_clone_object(configuration);
    job->placement = programme_after_event(view);
    view->local_event_cancel = radios_local_event_prepare_async(ctx, view->page, configuration,
        local_event_prepared, job, local_event_apply_free);
    if (!view->local_event_cancel) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Yerel Event kaynakları hazırlanmaya başlanamadı."); return FALSE;
    }
    programme_notice(view, "Yerel Event kaynakları hazırlanıyor…"); return TRUE;
}
static gboolean local_event_play(RadioSViewContext *ctx, JsonObject *configuration, gpointer value, GError **error)
{
    BroadcastView *view = value;
    if (view->disposed) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Yayın ekranı kapandı."); return FALSE; }
    GtkWidget *dialog = g_weak_ref_get(&view->local_event_dialog); guint deck = G_MAXUINT; gchar cluster_id[37] = "";
    gboolean selected = dialog && radios_local_event_ui_playback(dialog, &deck, cluster_id);
    g_clear_object(&dialog);
    if (!selected) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Event oynatıcısını ve devam hedefini seçin."); return FALSE; }
    return radios_event_playout_local_for_event(ctx, view->page, configuration, deck, cluster_id, view->local_event_target, error);
}
static void local_event_target_clear(GtkWidget *dialog, gpointer value)
{
    (void)dialog; BroadcastView *view = value;
    g_clear_pointer(&view->local_event_target, g_free); g_clear_pointer(&view->local_event_target_document_id, g_free);
    g_clear_pointer(&view->local_event_target_baseline, json_node_free); view->local_event_target_document = NULL;
}
static void local_event_open(GtkButton *control, gpointer value)
{
    (void)control; BroadcastView *view = value;
    if (view->disposed) return;
    GtkWidget *existing = g_weak_ref_get(&view->local_event_dialog);
    if (existing) { gtk_window_present(GTK_WINDOW(existing)); g_object_unref(existing); return; }
    local_event_target_clear(NULL, view);
    GtkWidget *dialog = radios_local_event_ui_open(view->ctx, view->ctx->parent, local_event_insert, view);
    if (dialog) {
        g_weak_ref_set(&view->local_event_dialog, dialog);
        radios_local_event_ui_set_play_handler(dialog, local_event_play, view);
        g_signal_connect(dialog, "destroy", G_CALLBACK(local_event_target_clear), view);
        g_signal_connect_object(view->page, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    }
}

/* The event's own source assignment uses the same independent builder. */
static void local_event_open_for_block(GtkButton *control, gpointer value)
{
    (void)control; GtkWidget *properties = value;
    RadioSViewContext *ctx = g_object_get_data(G_OBJECT(properties), "event-playback-context");
    BroadcastView *view = ctx ? broadcast_context(ctx) : NULL;
    if (!view || view->disposed) return;
    const gchar *id = g_object_get_data(G_OBJECT(properties), "programme-block-id");
    RadioSProgrammeTree *tree = programme_tree(view); guint index = 0; gint leaf = -1;
    if (view->programme_document != g_object_get_data(G_OBJECT(properties), "event-playback-document") ||
        view->queue_slot + 1 != GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(properties), "event-playback-slot")) ||
        !radios_programme_tree_find(tree, id, &index, &leaf) || leaf >= 0) {
        radios_programme_tree_free(tree); programme_notice(view, "Yayın sırası değişti; eventi yeniden seçin."); return;
    }
    RadioSProgrammeBlock *block = g_ptr_array_index(tree->blocks, index);
    JsonObject *override = !view->queue_slot ? g_hash_table_lookup(view->event_overrides, id) : NULL;
    JsonObject *source = catalog_object(override ? override : block->block, "local_event");
    JsonObject *recipe = source ? radios_programme_clone_object(source) : NULL;
    radios_programme_tree_free(tree);
    local_event_open(NULL, view);
    GtkWidget *dialog = g_weak_ref_get(&view->local_event_dialog);
    if (dialog) {
        const gchar *title = gtk_entry_get_text(GTK_ENTRY(g_object_get_data(G_OBJECT(properties), "programme-block-title")));
        const gchar *const kinds[] = {"music", "announcement", "advertisement", "jingle", "program", "sweeper", "news", "effect", "voice_track", "other"};
        gint kind = gtk_combo_box_get_active(GTK_COMBO_BOX(g_object_get_data(G_OBJECT(properties), "programme-block-kind")));
        gboolean seeded = recipe ? radios_local_event_ui_set_configuration(dialog, recipe) :
            radios_local_event_ui_set_event(dialog, title, kinds[CLAMP(kind, 0, 9)]);
        if (seeded) {
            JsonObject *playback = programme_event_playback_value(properties);
            gint64 visible = radios_json_int(playback, "player", 0);
            radios_local_event_ui_set_playback(dialog, visible ? (guint)visible - 1 : G_MAXUINT,
                radios_json_string(playback, "next_cluster_id", "")); json_object_unref(playback);
            local_event_target_clear(NULL, view);
            view->local_event_target = g_strdup(id); view->local_event_target_document = view->programme_document;
            view->local_event_target_document_id = g_strdup(radios_data_document_id(view->programme_document));
            view->local_event_target_slot = view->queue_slot;
            view->local_event_target_baseline = programme_copy(view->programme_config);
            radios_local_event_ui_set_insert_label(dialog, !view->queue_slot && general_mode(view) ?
                "Evente ata ve kümesizde çal" : "Seçili evente ata");
            gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(properties));
        }
        g_object_unref(dialog);
    }
    if (recipe) json_object_unref(recipe);
}

static void programme_library_load(BroadcastView *view, JsonObject *document);
/* Saved drafts retain their catalogue order and are written as one programme revision. */
static gboolean catalog_programme_batch(BroadcastView *view, JsonArray *records, RadioSProgrammePlacement placement)
{
    if (!programme_can_change(view)) return FALSE;
    RadioSProgrammeTree *tree = programme_tree(view); gchar *first = NULL;
    for (guint i = 0; i < json_array_get_length(records); ++i) {
        JsonObject *record = catalog_object(json_array_get_object_element(records, i), "programme"), *config = catalog_object(record, "config");
        if (!config) { radios_programme_tree_free(tree); g_free(first); return FALSE; }
        RadioSProgrammeTree *source = radios_programme_tree_new(config);
        guint count = radios_programme_tree_leaf_count(source);
        if (!count || radios_programme_tree_leaf_count(tree) + count > 5000) {
            radios_programme_tree_free(source); radios_programme_tree_free(tree); g_free(first);
            catalog_message(view, count ? "Program en fazla 5000 öğe içerebilir." : "Seçili programlardan biri boş; hiçbir program eklenmedi."); return FALSE;
        }
        GPtrArray *leaves = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
        for (guint j = 0; j < count; ++j) {
            JsonObject *leaf = radios_programme_clone_object(radios_programme_tree_leaf_at(source, j, NULL));
            gchar *id = g_uuid_string_random(); json_object_set_string_member(leaf, "id", id); g_free(id);
            json_object_remove_member(leaf, "block_id"); g_ptr_array_add(leaves, leaf);
        }
        radios_programme_tree_free(source);
        const gchar *name = radios_json_string(record, "name", "");
        gchar *title = g_utf8_substring(name, 0, MIN(g_utf8_strlen(name, -1), 160));
        guint block = programme_place(tree, placement, leaves, "program", *title ? title : NULL); g_free(title);
        if (!first) first = g_strdup(radios_json_string(g_ptr_array_index(leaves, 0), "id", ""));
        if (placement.block >= 0) placement.position += count;
        else placement.position = block + 1;
        g_ptr_array_unref(leaves);
    }
    if (!first) { radios_programme_tree_free(tree); return FALSE; }
    gboolean saved = programme_commit(view, tree, first); g_free(first);
    catalog_message(view, saved ? "Seçili programlar yayın sırasına eklendi." : "Program kaydı gönderilemedi; mevcut sıra korunuyor."); return saved;
}
static void catalog_to_programme(GtkButton *button_widget, gpointer data)
{
    BroadcastView *view = data; gboolean replace = GTK_WIDGET(button_widget) == view->catalog_replace;
    guint count = catalog_selected_count(view); CatalogSelection kind = catalog_selection_kind(view);
    if (replace && count != 1) { catalog_message(view, "Değiştirmek için tek bir katalog öğesi seçin."); return; }
    if (count > 1) {
        JsonArray *records = catalog_selected_records(view);
        if (kind == CATALOG_MEDIA) catalog_media_batch(view, records, NULL);
        else if (kind == CATALOG_PROGRAMME) catalog_programme_batch(view, records, programme_after_event(view));
        else catalog_message(view, "Birlikte eklemek için aynı türden içerikler seçin.");
        json_array_unref(records); return;
    }
    JsonObject *programme = catalog_selected_programme(view);
    if (programme) {
        if (replace) { programme_library_load(view, programme); return; }
        if (programme_can_change(view) && programme_insert_saved(view, programme, programme_after_event(view)))
            catalog_message(view, "Program yayın sırasına event olarak eklendi.");
        return;
    }
    catalog_media_verify(view, catalog_selected_record(view), replace, NULL);
}

/* A catalogue drag carries the selected row: a media record or {"programme": …}. */
static gboolean catalog_media_drop(BroadcastView *view, const gchar *payload, RadioSProgrammePlacement placement)
{
    JsonNode *node = json_parse(payload); gboolean accepted = FALSE;
    if (node && JSON_NODE_HOLDS_OBJECT(node)) {
        JsonObject *object = json_node_get_object(node), *programme = catalog_object(object, "programme");
        JsonNode *batch = json_object_get_member(object, "catalogue_records");
        if (batch && JSON_NODE_HOLDS_ARRAY(batch)) {
            JsonArray *records = json_node_get_array(batch), *selected = catalog_selected_records(view);
            if (catalog_records_equal(records, selected)) {
                CatalogSelection kind = catalog_selection_kind(view);
                if (kind == CATALOG_MEDIA) accepted = catalog_media_batch(view, records, &placement);
                else if (kind == CATALOG_PROGRAMME) accepted = catalog_programme_batch(view, records, placement);
            } else catalog_message(view, "Katalog seçimi değişti; sürüklenen içerikler eklenmedi.");
            json_array_unref(selected);
        } else if (programme) {
            accepted = programme_insert_saved(view, programme, placement);
        } else {
            JsonObject *media = catalog_object(object, "media");
            /* Verification re-reads the dragged (still selected) row. */
            accepted = media && !g_strcmp0(radios_json_string(media, "_id", ""), catalog_selected(view)) &&
                catalog_media_verify(view, catalog_selected_record(view), FALSE, &placement);
        }
    }
    if (node) json_node_free(node);
    return accepted;
}
static gchar *json_text(JsonNode *node)
{
    JsonGenerator *generator = json_generator_new(); json_generator_set_root(generator, node);
    gchar *text = json_generator_to_data(generator, NULL); g_object_unref(generator); return text;
}
static void catalog_drag_get(GtkWidget *tree, GdkDragContext *context, GtkSelectionData *selection, guint info, guint time, gpointer data)
{
    (void)tree; (void)context; (void)time; BroadcastView *view = data; JsonNode *node = NULL;
    CatalogSelection kind = catalog_selection_kind(view);
    if (catalog_selected_count(view) > 1 && (kind == CATALOG_MEDIA || kind == CATALOG_PROGRAMME)) {
        node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
        json_object_set_array_member(json_node_get_object(node), "catalogue_records", catalog_selected_records(view));
    } else {
        JsonObject *record = info == RADIOS_DRAG_INFO_PROGRAMME ? catalog_selected_programme(view) : catalog_selected_record(view);
        if (record) { node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, record); }
    }
    if (!node) return;
    gchar *payload = json_text(node); radios_drag_set_payload(selection, payload); g_free(payload); json_node_free(node);
}
static void catalog_open(GtkWidget *widget, gpointer data)
{
    (void)widget; BroadcastView *view = data; JsonObject *record = catalog_selected_record(view);
    JsonObject *playlist = catalog_object(record,"playlist");
    if (playlist) { playlist_open(view,radios_json_string(playlist,"_id","")); return; }
    const gchar *id = catalog_selected(view); if (id) radios_media_library_open_selected(view->ctx, id);
}
static void catalog_activated(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column, gpointer data)
{ (void)tree; (void)path; (void)column; catalog_open(NULL, data); }
static void catalog_media_request(BroadcastView *view, CatalogRead *read)
{
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(body, object);
    json_object_set_string_member(object, "station_id", radios_data_station_id(view->ctx));
    json_object_set_string_member(object, "query", gtk_entry_get_text(GTK_ENTRY(view->query)));
    json_object_set_string_member(object, "kind", view->catalog_kind ? view->catalog_kind : "music");
    json_object_set_string_member(object, "field", "");
    json_object_set_int_member(object, "limit", 200);
    radios_data_request(view->ctx, view->page, "POST", "/v1/workflows/desktop-media/list", body, catalog_loaded, read, catalog_read_free);
    json_node_free(body);
}
static void catalog_programmes_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; (void)owner; CatalogRead *read = data; BroadcastView *view = read->view;
    if (read->generation != view->catalog_generation) return;
    if (error || !result) { gtk_label_set_text(GTK_LABEL(view->catalog_status), error ? error->message : ""); return; }
    CatalogRead *next = g_new0(CatalogRead, 1); next->view = view; next->generation = read->generation; next->programmes = json_node_copy(result);
    catalog_media_request(view, next);
}
typedef struct {
    BroadcastView *view;
    guint generation;
    GWeakRef owner;
    RadioSDataClient *client;
    JsonNode *payload;
    GCancellable *cancel;
    gboolean playlist;
} CatalogQuery;
static void catalog_query_free(gpointer value)
{
    CatalogQuery *query = value; g_weak_ref_clear(&query->owner);
    if (query->client) radios_data_client_unref(query->client);
    g_clear_pointer(&query->payload, json_node_free); g_clear_object(&query->cancel); g_free(query);
}
static CatalogQuery *catalog_query_new(BroadcastView *view)
{
    CatalogQuery *query = g_new0(CatalogQuery, 1); query->view = view; query->generation = view->catalog_generation;
    g_weak_ref_init(&query->owner, view->page); query->client = radios_data_client_acquire(view->ctx);
    query->cancel = g_object_ref(view->catalog_cancel); query->payload = radios_data_body(view->ctx);
    query->playlist = gtk_combo_box_get_active(GTK_COMBO_BOX(view->catalog_field)) == 1;
    json_object_set_string_member(json_node_get_object(query->payload), "query", gtk_entry_get_text(GTK_ENTRY(view->query)));
    json_object_set_string_member(json_node_get_object(query->payload), "kind", query->playlist ? "playlist" : "music");
    return query;
}
static void catalog_search_prompt(BroadcastView *view)
{
    if (!view->query) return;
    gboolean playlist = gtk_combo_box_get_active(GTK_COMBO_BOX(view->catalog_field)) == 1;
    gtk_entry_set_placeholder_text(GTK_ENTRY(view->query), playlist ? "Medya arşivi · Playlist ara…" : "Medya arşivi · Parça, sanatçı veya albüm ara…");
}

static void catalog_unified_render(BroadcastView *view)
{
    JsonArray *records = json_array_new();
    JsonArray *part = view->catalog_result ? json_node_get_array(view->catalog_result) : NULL;
    for (guint index = 0; part && index < json_array_get_length(part); ++index)
        json_array_add_element(records, json_node_copy(json_array_get_element(part, index)));
    g_clear_pointer(&view->catalog_records, json_node_free); view->catalog_records = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(view->catalog_records, json_object_new()); json_object_set_array_member(json_node_get_object(view->catalog_records), "records", records);
    GtkListStore *store = g_object_get_data(G_OBJECT(view->catalog_table), "list-store"); gtk_list_store_clear(store);
    for (guint index = 0; index < json_array_get_length(records); ++index) {
        JsonObject *record = json_array_get_object_element(records, index), *media = catalog_object(record,"media"), *version = catalog_object(record,"version");
        JsonObject *playlist = catalog_object(record,"playlist");
        gchar *duration = catalog_duration(media ? radios_json_int(version,"duration_ms",0) : 0);
        const gchar *title = media ? radios_json_string(media,"title","") : radios_json_string(playlist,"name","");
        GtkTreeIter iter; gtk_list_store_append(store,&iter);
        gtk_list_store_set(store,&iter,0,radios_json_string(media,"artist",""),1,title,2,radios_json_string(media,"album",""),
            3,playlist ? "Playlist" : media ? radios_programme_kind_label(radios_json_string(media,"kind","music")) : "Müzik",
            4,playlist ? "—" : duration,5,"Medya arşivi",-1); g_free(duration);
    }
    gchar *count = g_strdup_printf("%u kayıt%s",json_array_get_length(records),view->catalog_pending?" · aranıyor…":"");
    gtk_label_set_text(GTK_LABEL(view->catalog_count),count); g_free(count);
    gtk_label_set_text(GTK_LABEL(view->catalog_status),view->catalog_error ? view->catalog_error : ""); catalog_actions_update(view);
}
static void catalog_query_received(CatalogQuery *query, JsonNode *result, const GError *error)
{
    GtkWidget *owner = g_weak_ref_get(&query->owner); if (!owner) return;
    BroadcastView *view = query->view; RadioSDataClient *current = radios_data_client_acquire(view->ctx);
    gboolean valid = !view->disposed && query->generation == view->catalog_generation && view->catalog_unified && current == query->client &&
        !g_cancellable_is_cancelled(query->cancel);
    if (current) radios_data_client_unref(current);
    if (!valid) { g_object_unref(owner); return; }
    JsonArray *records = json_array_new(); JsonObject *root = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    if (error || !root) { g_free(view->catalog_error); view->catalog_error = g_strdup(error ? error->message : "Arama yanıtı alınamadı."); }
    else {
        JsonArray *items = json_object_get_array_member(root,query->playlist?"playlists":"records");
        const gchar *text = radios_json_string(json_node_get_object(query->payload),"query","");
        for (guint index = 0; items && index < json_array_get_length(items); ++index) {
            JsonObject *item = json_array_get_object_element(items,index);
            if (query->playlist) {
                if (!catalog_matches(radios_json_string(item,"name",""),text)) continue;
                JsonObject *record = json_object_new(); json_object_set_object_member(record,"playlist",json_object_ref(item)); json_array_add_object_element(records,record);
            } else json_array_add_element(records,json_node_copy(json_array_get_element(items,index)));
        }
    }
    g_clear_pointer(&view->catalog_result,json_node_free);
    view->catalog_result = json_node_new(JSON_NODE_ARRAY); json_node_take_array(view->catalog_result,records);
    view->catalog_pending = FALSE; catalog_unified_render(view); g_object_unref(owner);
}
static void catalog_query_data(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{ (void)ctx; (void)owner; catalog_query_received(value,result,error); }
static void catalog_query_reset(BroadcastView *view)
{
    if (view->catalog_cancel) g_cancellable_cancel(view->catalog_cancel);
    g_clear_object(&view->catalog_cancel); view->catalog_cancel = g_cancellable_new(); ++view->catalog_generation;
    g_clear_pointer(&view->catalog_result,json_node_free); g_clear_pointer(&view->catalog_error,g_free);
    view->catalog_pending = TRUE; catalog_unified_render(view);
}
static void catalog_query_search(BroadcastView *view)
{
    catalog_query_reset(view);
    CatalogQuery *query = catalog_query_new(view);
    const gchar *path = query->playlist ? "/v1/workflows/desktop-playlists/list" : "/v1/workflows/desktop-media/list";
    json_object_set_string_member(json_node_get_object(query->payload),"kind",view->catalog_kind);
    json_object_set_int_member(json_node_get_object(query->payload),"limit",200);
    JsonNode *request_body = query->playlist ? radios_data_body(view->ctx) : json_node_copy(query->payload);
    radios_data_request(view->ctx,view->page,"POST",path,request_body,catalog_query_data,query,catalog_query_free);
    json_node_free(request_body);
}
static void
catalog_search(GtkWidget *widget, gpointer data)
{
    BroadcastView *view = data; (void)widget;
    catalog_search_prompt(view);
    if (view->catalog_switching) return;
    if (!g_strcmp0(view->catalog_kind, "playlist")) { playlist_refresh(view); return; }
    const gchar *query = gtk_entry_get_text(GTK_ENTRY(view->query));
    view->catalog_unified = gtk_combo_box_get_active(GTK_COMBO_BOX(view->catalog_field)) == 1;
    if (view->catalog_unified && radios_data_connected(view->ctx)) {
        if (g_utf8_strlen(query,-1) > 256) { catalog_message(view,"Arama metni en fazla 256 karakter olabilir."); return; }
        catalog_query_search(view); return;
    }
    if (radios_data_connected(view->ctx)) {
        CatalogRead *read = g_new0(CatalogRead, 1); read->view = view; read->generation = ++view->catalog_generation;
        /* Programlar: saved programme drafts, then program-kind media. */
        if (!g_strcmp0(view->catalog_kind, "program")) {
            JsonNode *body = radios_data_body(view->ctx); json_object_set_string_member(json_node_get_object(body), "kind", "programme");
            radios_data_request(view->ctx, view->page, "POST", "/v1/workflows/desktop/list", body, catalog_programmes_loaded, read, catalog_read_free);
            json_node_free(body);
        } else catalog_media_request(view, read);
        return;
    }
    gchar *message = g_strdup_printf("%s%sKatalog araması için veri servisi bağlantısı gerekiyor.",
                                     *query != '\0' ? query : "",
                                     *query != '\0' ? ": " : "");
    if (view->ctx->notify != NULL)
        view->ctx->notify(view->ctx->notify_data, message);
    g_free(message);
}
static void catalog_kind_changed(GtkToggleButton *button_widget, gpointer data)
{
    if (!gtk_toggle_button_get_active(button_widget)) return;
    BroadcastView *view = data; view->catalog_kind = g_object_get_data(G_OBJECT(button_widget), "catalog-kind");
    if (view->catalog_switching || !view->catalog_table) return;
    ++view->catalog_generation;
    if (view->catalog_cancel) g_cancellable_cancel(view->catalog_cancel);
    g_clear_pointer(&view->catalog_records, json_node_free);
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(g_object_get_data(G_OBJECT(view->catalog_table), "tree-view")));
    gtk_list_store_clear(g_object_get_data(G_OBJECT(view->catalog_table), "list-store"));
    gtk_label_set_text(GTK_LABEL(view->catalog_count), "— kayıt");
    gtk_label_set_text(GTK_LABEL(view->catalog_status), "");
    view->catalog_switching = TRUE;
    gtk_combo_box_set_active(GTK_COMBO_BOX(view->catalog_field), 0);
    view->catalog_switching = FALSE;
    gboolean playlist = !g_strcmp0(view->catalog_kind, "playlist");
    gtk_stack_set_visible_child_name(GTK_STACK(view->catalog_content), playlist ? "playlists" : "media");
    if (playlist) { catalog_actions_update(view); playlist_refresh(view); return; }
    catalog_search(NULL, view);
    catalog_actions_update(view);
}
/* Reclicking the active tab also restores its scope (notably Playlist → Müzik). */
static void catalog_kind_clicked(GtkButton *button_widget, gpointer data)
{ catalog_kind_changed(GTK_TOGGLE_BUTTON(button_widget), data); }

static void programme_library_detail(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    CatalogueIntent *intent = value; BroadcastView *view = g_object_get_data(G_OBJECT(owner), "radios-broadcast-view");
    if (!view || view->disposed || intent->intent != view->catalogue_intent) return;
    view->catalogue_busy = FALSE; catalog_actions_update(view);
    if (error) { catalog_message(view, error->message); return; }
    JsonObject *record = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    JsonNode *config = record ? json_object_get_member(record, "config") : NULL;
    JsonObject *selected = catalog_selected_programme(view);
    if (!catalogue_current(view, intent) || !selected || g_strcmp0(intent->media_id, radios_json_string(selected, "_id", "")) ||
        !record || !config || !JSON_NODE_HOLDS_OBJECT(config) || g_strcmp0(radios_json_string(record, "_id", ""), intent->media_id) ||
        radios_json_int(record, "_rev", 0) != intent->version_revision || g_strcmp0(radios_json_string(record, "kind", ""), "programme") ||
        g_strcmp0(radios_json_string(record, "status", ""), "active") || g_strcmp0(radios_json_string(record, "station_id", ""), radios_data_station_id(ctx))) {
        catalog_message(view, "Kaynak program kopyası, hedef taslak veya veri kimliği değişti; Mevcut program korunuyor. Listeyi yenileyin."); return;
    }
    catalog_message(view, catalogue_save(view, programme_copy(config)) ?
        "Seçili program Mevcut program taslağına yüklendi." : "Program kopyası yüklenemedi; Mevcut program korunuyor.");
}
typedef struct { BroadcastView *view; CatalogueIntent *intent; } ProgrammeLoad;
static void programme_load_free(gpointer value)
{ ProgrammeLoad *load = value; if (load->intent) catalogue_intent_free(load->intent); g_free(load); }
static void programme_library_load_response(GtkDialog *dialog, gint response, gpointer value)
{
    ProgrammeLoad *load = value; BroadcastView *view = load->view;
    if (response == GTK_RESPONSE_ACCEPT) {
        CatalogueIntent *intent = load->intent; JsonObject *selected = catalog_selected_programme(view);
        if (!catalogue_current(view, intent) || view->catalogue_busy || !selected ||
            g_strcmp0(intent->media_id, radios_json_string(selected, "_id", ""))) {
            catalog_message(view, "Onay penceresi açıldıktan sonra kaynak kopya, hedef taslak veya veri kimliği değişti; Mevcut program korunuyor."); }
        else {
            JsonNode *body = radios_data_body(view->ctx); JsonObject *object = json_node_get_object(body);
            json_object_set_string_member(object, "kind", "programme"); json_object_set_string_member(object, "document_id", intent->media_id);
            view->catalogue_busy = TRUE; catalog_actions_update(view); catalog_message(view, "Seçili programın güncel kaydı doğrulanıyor…");
            load->intent = NULL;
            if (!radios_data_request(view->ctx, view->page, "POST", "/v1/workflows/desktop/get", body, programme_library_detail, intent, catalogue_intent_free)) {
                view->catalogue_busy = FALSE; catalog_actions_update(view); catalog_message(view, "Program kopyası okunamadı; hedef taslak korunuyor."); }
            json_node_free(body);
        }
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
/* "Mevcut taslağı değiştir": replaces the working draft with a saved programme after confirmation. */
static void programme_library_load(BroadcastView *view, JsonObject *document)
{
    if (view->catalogue_busy || !document) return;
    CatalogueIntent *intent = catalogue_capture(view);
    if (!intent) { catalog_message(view, "Güncel program kopyası ve kaydedilmiş hedef seçilmedi."); return; }
    intent->media_id = g_strdup(radios_json_string(document, "_id", "")); intent->version_revision = radios_json_int(document, "_rev", 0);
    ProgrammeLoad *load = g_new0(ProgrammeLoad, 1); load->view = view; load->intent = intent;
    GtkWidget *dialog = gtk_message_dialog_new(view->ctx->parent, GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", "Seçili programla Mevcut program taslağını değiştir?");
    gtk_widget_set_name(dialog, "programme-library-load-dialog");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", "Mevcut programın içeriği seçili kayıtla değiştirilir.");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Vazgeç", GTK_RESPONSE_CANCEL, "Taslağı değiştir", GTK_RESPONSE_ACCEPT, NULL);
    g_object_set_data_full(G_OBJECT(dialog), "programme-library-load", load, programme_load_free);
    g_signal_connect(dialog, "response", G_CALLBACK(programme_library_load_response), load);
    g_signal_connect_object(view->page, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED); gtk_widget_show_all(dialog);
}

void radios_automation_playlists_changed(GtkWidget *parent);
/* "Playlist yönetimi" of the catalogue (design: ld-library): find a playlist,
 * see its tracks and send them to the flow. Editing is the Playlist profile. */
typedef struct {
    BroadcastView *view;
    GtkWidget *widget, *query, *lists, *tracks, *name, *count, *status, *add_track, *add_all;
    JsonNode *records;
    gchar *selected;
    gboolean busy, preserve_status;
} LibraryPanel;
static void library_free(gpointer value)
{ LibraryPanel *panel = value; g_clear_pointer(&panel->records, json_node_free); g_free(panel->selected); g_free(panel); }
static JsonArray *library_playlists(LibraryPanel *panel)
{ return panel->records ? json_object_get_array_member(json_node_get_object(panel->records), "playlists") : NULL; }
static JsonObject *library_lookup(LibraryPanel *panel, const gchar *id)
{
    JsonArray *lists = library_playlists(panel);
    for (guint i = 0; id && lists && i < json_array_get_length(lists); ++i)
        if (g_str_equal(id, radios_json_string(json_array_get_object_element(lists, i), "_id", "")))
            return json_array_get_object_element(lists, i);
    return NULL;
}
static JsonObject *library_current(LibraryPanel *panel)
{ return library_lookup(panel, panel->selected); }
static gboolean library_matches(JsonObject *playlist, const gchar *query)
{
    if (!*query || catalog_matches(radios_json_string(playlist, "name", ""), query)) return TRUE;
    JsonArray *tracks = json_object_get_array_member(playlist, "tracks");
    for (guint i = 0; tracks && i < json_array_get_length(tracks); ++i) {
        JsonObject *track = json_array_get_object_element(tracks, i); const gchar *keys[] = {"title", "artist", "album"};
        for (guint j = 0; j < G_N_ELEMENTS(keys); ++j) if (catalog_matches(radios_json_string(track, keys[j], ""), query)) return TRUE;
    }
    return FALSE;
}
static void library_drag_begin(GtkWidget *handle, GdkDragContext *context, gpointer value)
{
    (void)context; LibraryPanel *panel = value;
    GtkListBox *list = GTK_LIST_BOX(g_object_get_data(G_OBJECT(handle), "library-single") ? panel->tracks : panel->lists);
    gtk_list_box_select_row(list, GTK_LIST_BOX_ROW(gtk_widget_get_parent(handle)));
}
static void library_drag_get(GtkWidget *handle, GdkDragContext *context, GtkSelectionData *selection, guint info, guint time, gpointer value)
{
    (void)context; (void)time; (void)value;
    if (info != RADIOS_DRAG_INFO_PLAYLIST) return;
    const gchar *playlist_id = g_object_get_data(G_OBJECT(handle), "library-playlist-id");
    if (!g_uuid_string_is_valid(playlist_id)) return;
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
    JsonObject *object = json_node_get_object(node); json_object_set_string_member(object, "playlist_id", playlist_id);
    if (g_object_get_data(G_OBJECT(handle), "library-single"))
        json_object_set_string_member(object, "entry_id", g_object_get_data(G_OBJECT(gtk_widget_get_parent(handle)), "library-id"));
    gchar *payload = json_text(node); radios_drag_set_payload(selection, payload); g_free(payload); json_node_free(node);
}
static GtkWidget *library_row(LibraryPanel *panel, const gchar *title, const gchar *meta, const gchar *id, const gchar *playlist_id, gboolean single)
{
    GtkWidget *row = gtk_list_box_row_new(), *box = named_box(GTK_ORIENTATION_VERTICAL, 2, "ld-library-item");
    GtkWidget *handle = gtk_event_box_new(); gtk_event_box_set_visible_window(GTK_EVENT_BOX(handle), FALSE);
    GtkWidget *name = radios_ui_label(title, "ld-library-title"), *detail = radios_ui_label(meta, "ld-library-meta");
    gtk_label_set_line_wrap(GTK_LABEL(name), FALSE); gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_label_set_line_wrap(GTK_LABEL(detail), FALSE); gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END);
    pack(box, name, FALSE); pack(box, detail, FALSE);
    gtk_container_add(GTK_CONTAINER(handle), box); gtk_container_add(GTK_CONTAINER(row), handle);
    g_object_set_data_full(G_OBJECT(row), "library-id", g_strdup(id), g_free);
    g_object_set_data_full(G_OBJECT(handle), "library-playlist-id", g_strdup(playlist_id), g_free);
    g_object_set_data(G_OBJECT(handle), "library-single", GINT_TO_POINTER(single));
    gtk_drag_source_set(handle, GDK_BUTTON1_MASK, &radios_drag_targets(NULL)[RADIOS_DRAG_INFO_PLAYLIST - 1], 1, GDK_ACTION_COPY);
    g_signal_connect(handle, "drag-begin", G_CALLBACK(library_drag_begin), panel);
    g_signal_connect(handle, "drag-data-get", G_CALLBACK(library_drag_get), panel);
    return row;
}
static void library_clear(GtkWidget *list)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *item = rows; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(rows);
}
static void library_actions_update(LibraryPanel *panel)
{
    JsonObject *playlist = library_current(panel);
    JsonArray *tracks = playlist ? json_object_get_array_member(playlist, "tracks") : NULL;
    gtk_widget_set_sensitive(panel->add_all, tracks && json_array_get_length(tracks) && !panel->busy);
    gtk_widget_set_sensitive(panel->add_track, gtk_list_box_get_selected_row(GTK_LIST_BOX(panel->tracks)) && !panel->busy);
}
static void library_tracks_render(LibraryPanel *panel)
{
    library_clear(panel->tracks); JsonObject *playlist = library_current(panel);
    JsonArray *tracks = playlist ? json_object_get_array_member(playlist, "tracks") : NULL; gint64 total = 0;
    for (guint i = 0; tracks && i < json_array_get_length(tracks); ++i) {
        JsonObject *track = json_array_get_object_element(tracks, i); gint64 milliseconds = radios_json_int(track, "duration_ms", 0);
        gchar *duration = milliseconds > 0 ? catalog_duration(milliseconds) : g_strdup("—");
        gchar *meta = g_strdup_printf("%s%s%s", radios_json_string(track, "artist", ""), *radios_json_string(track, "artist", "") ? " · " : "", duration);
        gtk_container_add(GTK_CONTAINER(panel->tracks), library_row(panel, radios_json_string(track, "title", "—"), meta,
            radios_json_string(track, "_id", ""), radios_json_string(playlist, "_id", ""), TRUE));
        total += MAX(0, milliseconds); g_free(duration); g_free(meta);
    }
    gtk_widget_show_all(panel->tracks);
    gtk_label_set_text(GTK_LABEL(panel->name), playlist ? radios_json_string(playlist, "name", "") : "Playlist seçilmedi");
    gchar *duration = total > 0 ? catalog_duration(total) : g_strdup("—");
    gchar *count = playlist ? g_strdup_printf("%u parça · %s", tracks ? json_array_get_length(tracks) : 0, duration) : g_strdup("");
    gtk_label_set_text(GTK_LABEL(panel->count), count); g_free(count); g_free(duration);
    library_actions_update(panel);
}
static void library_render(LibraryPanel *panel)
{
    library_clear(panel->lists); JsonArray *lists = library_playlists(panel);
    const gchar *query = gtk_entry_get_text(GTK_ENTRY(panel->query)); guint shown = 0;
    for (guint i = 0; lists && i < json_array_get_length(lists); ++i) {
        JsonObject *playlist = json_array_get_object_element(lists, i);
        if (!library_matches(playlist, query)) continue;
        JsonArray *tracks = json_object_get_array_member(playlist, "tracks");
        gchar *meta = g_strdup_printf("%u parça", tracks ? json_array_get_length(tracks) : 0);
        const gchar *id = radios_json_string(playlist, "_id", "");
        GtkWidget *row = library_row(panel, radios_json_string(playlist, "name", ""), meta, id, id, FALSE);
        gtk_container_add(GTK_CONTAINER(panel->lists), row); g_free(meta); ++shown;
        if (!g_strcmp0(panel->selected, radios_json_string(playlist, "_id", ""))) gtk_list_box_select_row(GTK_LIST_BOX(panel->lists), GTK_LIST_BOX_ROW(row));
    }
    if (!shown) {
        GtkWidget *row = gtk_list_box_row_new(); gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_container_add(GTK_CONTAINER(row), radios_ui_label(lists && json_array_get_length(lists) ? "Sonuç bulunamadı" : "Playlist yok", "ld-empty"));
        gtk_container_add(GTK_CONTAINER(panel->lists), row);
    }
    gtk_widget_show_all(panel->lists); library_tracks_render(panel);
}
static void library_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; LibraryPanel *panel = value;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { gtk_label_set_text(GTK_LABEL(panel->status), error ? error->message : "Playlistler okunamadı."); return; }
    g_clear_pointer(&panel->records, json_node_free); panel->records = programme_copy(result);
    if (!library_current(panel)) g_clear_pointer(&panel->selected, g_free);
    if (!panel->preserve_status && !panel->busy) gtk_label_set_text(GTK_LABEL(panel->status), "");
    panel->preserve_status = FALSE; library_render(panel);
}
static void library_refresh(LibraryPanel *panel)
{
    if (panel->view->disposed || !radios_data_connected(panel->view->ctx)) return;
    JsonNode *body = radios_data_body(panel->view->ctx);
    radios_data_request(panel->view->ctx, panel->widget, "POST", "/v1/workflows/desktop-playlists/list", body, library_loaded, panel, NULL);
    json_node_free(body);
}
static void playlist_refresh(BroadcastView *view)
{
    LibraryPanel *panel = view->playlist_panel ? g_object_get_data(G_OBJECT(view->playlist_panel), "broadcast-library") : NULL;
    if (panel) library_refresh(panel);
}
static void library_mapped(GtkWidget *widget, gpointer value)
{ (void)widget; library_refresh(value); }
static void library_search(GtkWidget *widget, gpointer value) { (void)widget; library_render(value); }
static void library_list_selected(GtkListBox *list, GtkListBoxRow *row, gpointer value)
{
    (void)list; LibraryPanel *panel = value; const gchar *id = row ? g_object_get_data(G_OBJECT(row), "library-id") : NULL;
    if (!id || !g_strcmp0(id, panel->selected)) return;
    g_free(panel->selected); panel->selected = g_strdup(id); library_tracks_render(panel);
}
static void library_track_selected(GtkListBox *list, GtkListBoxRow *row, gpointer value)
{ (void)list; LibraryPanel *panel = value; gtk_widget_set_sensitive(panel->add_track, row && !panel->busy); }
typedef struct {
    LibraryPanel *panel;
    CatalogueIntent *source;
    gboolean single;
    RadioSProgrammePlacement placement;
    gchar *name, *playlist_id, *version_id;
    gint64 revision;
    JsonArray *tracks;
} LibraryAdd;
static void library_add_free(gpointer value)
{
    LibraryAdd *add = value; catalogue_intent_free(add->source);
    g_free(add->name); g_free(add->playlist_id); g_free(add->version_id);
    if (add->tracks) json_array_unref(add->tracks);
    g_free(add);
}
static gboolean library_preparation_matches(LibraryAdd *add, JsonObject *result)
{
    if (!result || radios_json_int(result, "contract_version", 0) != 1 ||
        g_strcmp0(radios_json_string(result, "kind", ""), "playlist") ||
        g_strcmp0(radios_json_string(result, "station_id", ""), add->source->station_id) ||
        g_strcmp0(radios_json_string(result, "playlist_id", ""), add->playlist_id) ||
        radios_json_int(result, "playlist_revision", 0) != add->revision ||
        g_strcmp0(radios_json_string(result, "playlist_version_id", ""), add->version_id)) return FALSE;
    JsonNode *node = json_object_get_member(result, "tracks");
    JsonArray *tracks = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
    if (!tracks || json_array_get_length(tracks) != json_array_get_length(add->tracks)) return FALSE;
    for (guint i = 0; i < json_array_get_length(tracks); ++i) {
        JsonNode *row = json_array_get_element(tracks, i);
        JsonObject *saved = json_array_get_object_element(add->tracks, i);
        if (!JSON_NODE_HOLDS_OBJECT(row) || g_strcmp0(radios_json_string(json_node_get_object(row), "entry_id", ""),
            radios_json_string(saved, "_id", ""))) return FALSE;
    }
    return TRUE;
}
static void library_prepared(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)owner; LibraryAdd *add = value; LibraryPanel *panel = add->panel; GError *failure = NULL;
    panel->busy = FALSE; panel->view->catalogue_busy = FALSE; library_actions_update(panel); catalog_actions_update(panel->view);
    if (!catalogue_current(panel->view, add->source)) {
        gtk_label_set_text(GTK_LABEL(panel->status), "Hedef program veya seçim değişti; playlist eklenmedi. Yeniden bırakın."); return;
    }
    if (error) { gtk_label_set_text(GTK_LABEL(panel->status), error->message); return; }
    JsonObject *prepared = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    if (!library_preparation_matches(add, prepared)) {
        gtk_label_set_text(GTK_LABEL(panel->status), "Playlist kaydı değişti veya doğrulama sonucu uyuşmuyor; yeniden seçip ekleyin.");
        panel->preserve_status = TRUE; library_refresh(panel); return;
    }
    gboolean added = append_tracks(ctx, json_object_get_array_member(prepared, "tracks"),
        &add->placement, add->single ? NULL : add->name, &failure);
    gtk_label_set_text(GTK_LABEL(panel->status), added ? (add->single ? "Parça yayın sırasına eklendi." :
        add->placement.block >= 0 ? "Playlist parçaları açık evente eklendi." : "Playlist yayın sırasına event olarak eklendi.") :
        failure ? failure->message : "Yayın sırası güncellenemedi.");
    g_clear_error(&failure);
}
static gboolean library_transfer(LibraryPanel *panel, JsonObject *playlist, const gchar *entry_id, RadioSProgrammePlacement placement)
{
    BroadcastView *view = panel->view;
    if (!playlist || panel->busy || !programme_can_change(view)) return FALSE;
    CatalogueIntent *source = catalogue_capture(view); if (!source) return FALSE;
    LibraryAdd *add = g_new0(LibraryAdd, 1); add->panel = panel; add->source = source; add->single = entry_id != NULL;
    add->name = g_strdup(radios_json_string(playlist, "name", ""));
    add->playlist_id = g_strdup(radios_json_string(playlist, "_id", ""));
    add->version_id = g_strdup(radios_json_string(playlist, "version_id", ""));
    add->revision = radios_json_int(playlist, "_rev", 0); add->tracks = json_array_new();
    JsonNode *tracks_node = json_object_get_member(playlist, "tracks");
    JsonArray *tracks = tracks_node && JSON_NODE_HOLDS_ARRAY(tracks_node) ? json_node_get_array(tracks_node) : NULL;
    gboolean valid = g_uuid_string_is_valid(add->playlist_id) && g_uuid_string_is_valid(add->version_id) && add->revision > 0 && tracks;
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; valid && i < json_array_get_length(tracks); ++i) {
        JsonNode *node = json_array_get_element(tracks, i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) { valid = FALSE; break; }
        JsonObject *track = json_node_get_object(node); const gchar *id = radios_json_string(track, "_id", "");
        if (!g_uuid_string_is_valid(id) || g_hash_table_contains(seen, id)) { valid = FALSE; break; }
        g_hash_table_add(seen, (gpointer)id);
        if (!entry_id || !g_strcmp0(id, entry_id)) json_array_add_object_element(add->tracks, radios_programme_clone_object(track));
    }
    g_hash_table_unref(seen);
    if (!valid || !json_array_get_length(add->tracks) || (entry_id && json_array_get_length(add->tracks) != 1)) {
        gtk_label_set_text(GTK_LABEL(panel->status), "Playlist kaydı hazır değil; yeniden seçip ekleyin.");
        library_add_free(add); return FALSE;
    }
    add->placement = placement;
    JsonNode *body = radios_data_body(view->ctx); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "playlist"); json_object_set_string_member(object, "playlist_id", add->playlist_id);
    if (add->single) {
        JsonArray *ids = json_array_new(); json_array_add_string_element(ids, entry_id);
        json_object_set_array_member(object, "entry_ids", ids);
    }
    panel->preserve_status = FALSE; panel->busy = TRUE; view->catalogue_busy = TRUE; library_actions_update(panel); catalog_actions_update(view);
    gtk_label_set_text(GTK_LABEL(panel->status), "Playlist kaynakları doğrulanıyor…");
    gboolean started = radios_data_request(view->ctx, panel->widget, "POST", "/v1/workflows/desktop-playout/prepare", body, library_prepared, add, library_add_free);
    if (!started) {
        panel->busy = FALSE; view->catalogue_busy = FALSE; library_actions_update(panel); catalog_actions_update(view);
        gtk_label_set_text(GTK_LABEL(panel->status), "Medya kontrolü başlatılamadı.");
    }
    json_node_free(body);
    return started;
}
static void library_add(GtkButton *button, gpointer value)
{
    LibraryPanel *panel = value; RadioSProgrammeTree *tree = programme_tree(panel->view);
    gboolean single = GTK_WIDGET(button) == panel->add_track;
    RadioSProgrammePlacement placement = single ? programme_target(panel->view, tree) : programme_after_event(panel->view);
    radios_programme_tree_free(tree);
    GtkListBoxRow *row = single ? gtk_list_box_get_selected_row(GTK_LIST_BOX(panel->tracks)) : NULL;
    const gchar *entry_id = row ? g_object_get_data(G_OBJECT(row), "library-id") : NULL;
    if (!single || entry_id) library_transfer(panel, library_current(panel), entry_id, placement);
}
static gboolean playlist_drop(BroadcastView *view, const gchar *payload, RadioSProgrammePlacement placement)
{
    LibraryPanel *panel = view->playlist_panel ? g_object_get_data(G_OBJECT(view->playlist_panel), "broadcast-library") : NULL;
    JsonNode *node = json_parse(payload); gboolean accepted = FALSE;
    if (panel && node && JSON_NODE_HOLDS_OBJECT(node)) {
        JsonObject *object = json_node_get_object(node);
        const gchar *id = radios_json_string(object, "playlist_id", "");
        gboolean single = json_object_has_member(object, "entry_id");
        const gchar *entry_id = single ? radios_json_string(object, "entry_id", "") : NULL;
        JsonObject *playlist = g_uuid_string_is_valid(id) ? library_lookup(panel, id) : NULL;
        gboolean valid = playlist && json_object_get_size(object) == (single ? 2 : 1);
        if (single) {
            JsonArray *tracks = playlist ? json_object_get_array_member(playlist, "tracks") : NULL; gboolean found = FALSE;
            for (guint index = 0; valid && tracks && index < json_array_get_length(tracks); ++index)
                if (!g_strcmp0(entry_id, radios_json_string(json_array_get_object_element(tracks, index), "_id", ""))) found = TRUE;
            valid = valid && g_uuid_string_is_valid(entry_id) && found;
        }
        if (valid) accepted = library_transfer(panel, playlist, entry_id, placement);
    }
    if (node) json_node_free(node);
    return accepted;
}
static void library_created(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)owner; LibraryPanel *panel = value;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { gtk_label_set_text(GTK_LABEL(panel->status), error ? error->message : "Playlist oluşturulamadı."); return; }
    g_free(panel->selected); panel->selected = g_strdup(radios_json_string(json_node_get_object(result), "_id", ""));
    gtk_entry_set_text(GTK_ENTRY(panel->query), "");
    radios_automation_playlists_changed(GTK_WIDGET(ctx->parent)); library_refresh(panel);
}
static void library_new_response(GtkDialog *dialog, gint response, gpointer value)
{
    LibraryPanel *panel = value;
    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(g_object_get_data(G_OBJECT(dialog), "library-name")))));
        if (*name && radios_data_connected(panel->view->ctx)) {
            JsonNode *body = radios_data_body(panel->view->ctx); JsonObject *object = json_node_get_object(body); gchar *key = g_uuid_string_random();
            json_object_set_string_member(object, "name", name); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
            radios_data_request(panel->view->ctx, panel->widget, "POST", "/v1/workflows/desktop-playlists/create", body, library_created, panel, NULL);
            json_node_free(body);
        }
        g_free(name);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void library_new(GtkButton *button, gpointer value)
{
    (void)button; LibraryPanel *panel = value;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Playlist oluştur", panel->view->ctx->parent, GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Oluştur", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(dialog, "broadcast-library-new"); add_class(dialog, "radios"); add_class(dialog, "task-dialog");
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_container_set_border_width(GTK_CONTAINER(body), 16);
    GtkWidget *entry = radios_ui_entry("Playlist adı"); gtk_entry_set_max_length(GTK_ENTRY(entry), 120); gtk_widget_set_name(entry, "broadcast-library-new-name");
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE); gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    pack(body, entry, FALSE); g_object_set_data(G_OBJECT(dialog), "library-name", entry);
    g_signal_connect(dialog, "response", G_CALLBACK(library_new_response), panel);
    g_signal_connect_object(panel->widget, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    gtk_widget_show_all(dialog);
}
/* Düzenle: the Playlist profile edits playlists and tracks. */
static GtkWidget *library_find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = library_find(item->data, name);
    g_list_free(children); return found;
}
static void library_edit(GtkButton *button, gpointer value)
{
    (void)button; LibraryPanel *panel = value;
    GtkWidget *tab = library_find(GTK_WIDGET(panel->view->ctx->parent), "profile-playlist-tab");
    if (GTK_IS_BUTTON(tab)) gtk_button_clicked(GTK_BUTTON(tab));
}
static GtkWidget *library_list(GtkWidget *box, const gchar *name, gint height, const gchar *style)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); add_class(scroll, style);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, height);
    GtkWidget *list = gtk_list_box_new(); gtk_widget_set_name(list, name);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(scroll), list); pack(box, scroll, TRUE); return list;
}
static GtkWidget *
playlist_tools(BroadcastView *view)
{
    LibraryPanel *panel = g_new0(LibraryPanel, 1); panel->view = view;
    GtkWidget *body = named_box(GTK_ORIENTATION_VERTICAL, 8, "ld-body ld-library-body");
    panel->widget = body;
    gtk_widget_set_name(body, "broadcast-playlist-tools");
    g_object_set_data_full(G_OBJECT(body), "broadcast-library", panel, library_free);
    GtkWidget *search = named_box(GTK_ORIENTATION_HORIZONTAL, 7, "ld-search-row");
    panel->query = radios_ui_entry("Ad, sanatçı, albüm…"); gtk_widget_set_name(panel->query, "broadcast-library-query"); pack(search, panel->query, TRUE);
    GtkWidget *find = button(search, "Ara", "ld-btn", view->ctx, NULL);
    g_signal_connect(find, "clicked", G_CALLBACK(library_search), panel); g_signal_connect(panel->query, "activate", G_CALLBACK(library_search), panel);
    GtkWidget *create = button(search, "Playlist oluştur", "ld-btn--primary", view->ctx, NULL); gtk_widget_set_name(create, "broadcast-library-new-button");
    g_signal_connect(create, "clicked", G_CALLBACK(library_new), panel);
    pack(body, search, FALSE);
    GtkWidget *columns = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(columns), 200);
    GtkWidget *lists = named_box(GTK_ORIENTATION_VERTICAL, 7, NULL);
    GtkWidget *tracks = named_box(GTK_ORIENTATION_VERTICAL, 7, NULL);
    gtk_paned_pack1(GTK_PANED(columns), lists, FALSE, TRUE);
    gtk_paned_pack2(GTK_PANED(columns), tracks, TRUE, TRUE);
    pack(body, columns, TRUE);
    GtkWidget *lists_head = named_box(GTK_ORIENTATION_HORIZONTAL, 7, "ld-subhead");
    pack(lists_head, radios_ui_label("Playlistler", "heading"), TRUE);
    pack(lists, lists_head, FALSE);
    panel->lists = library_list(lists, "broadcast-library-playlists", 190, "ld-playlists");
    g_signal_connect(panel->lists, "row-selected", G_CALLBACK(library_list_selected), panel);
    GtkWidget *tracks_head = named_box(GTK_ORIENTATION_HORIZONTAL, 7, "ld-subhead");
    GtkWidget *titles = named_box(GTK_ORIENTATION_VERTICAL, 2, NULL);
    panel->name = radios_ui_label("Playlist seçilmedi", "heading"); panel->count = radios_ui_label("", "ld-library-meta");
    gtk_label_set_line_wrap(GTK_LABEL(panel->name), FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(panel->name), PANGO_ELLIPSIZE_END);
    pack(titles, panel->name, FALSE); pack(titles, panel->count, FALSE); pack(tracks_head, titles, TRUE);
    GtkWidget *edit = button(tracks_head, "Düzenle", "ld-btn", view->ctx, NULL); gtk_widget_set_name(edit, "broadcast-library-edit");
    g_signal_connect(edit, "clicked", G_CALLBACK(library_edit), panel);
    pack(tracks, tracks_head, FALSE);
    panel->tracks = library_list(tracks, "broadcast-library-tracks", 190, "ld-track-list");
    g_signal_connect(panel->tracks, "row-selected", G_CALLBACK(library_track_selected), panel);
    GtkWidget *to_programme = named_box(GTK_ORIENTATION_HORIZONTAL, 7, "ld-to-programme");
    panel->add_track = button(to_programme, "Seçili parçayı ekle", "ld-btn--primary", view->ctx, NULL); gtk_widget_set_name(panel->add_track, "broadcast-library-add-track");
    panel->add_all = button(to_programme, "Playlisti ekle", "ld-btn", view->ctx, NULL); gtk_widget_set_name(panel->add_all, "broadcast-library-add-all");
    g_signal_connect(panel->add_track, "clicked", G_CALLBACK(library_add), panel); g_signal_connect(panel->add_all, "clicked", G_CALLBACK(library_add), panel);
    pack(body, to_programme, FALSE);
    panel->status = radios_ui_label("", "application-status"); gtk_widget_set_name(panel->status, "broadcast-library-status"); pack(body, panel->status, FALSE);
    g_signal_connect(body, "map", G_CALLBACK(library_mapped), panel);
    library_render(panel);
    return body;
}
static void playlist_open(BroadcastView *view, const gchar *id)
{
    LibraryPanel *panel = g_object_get_data(G_OBJECT(view->playlist_panel),"broadcast-library");
    g_free(panel->selected); panel->selected = g_strdup(id); gtk_entry_set_text(GTK_ENTRY(panel->query),"");
    GtkWidget *tab = library_find(view->page,"broadcast-kind-playlist");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tab),TRUE); library_refresh(panel);
}

static JsonArray *catalog_playlist_selection(gpointer data)
{
    BroadcastView *view = data;
    JsonArray *records = catalog_selected_records(view);
    JsonArray *entries = json_array_new();
    for (guint i = 0; i < json_array_get_length(records); ++i) {
        JsonObject *track = radios_catalogue_playlist_track(json_array_get_object_element(records, i));
        if (!track) { json_array_unref(entries); entries = NULL; break; }
        json_array_add_object_element(entries, track);
    }
    json_array_unref(records);
    return entries;
}
static void catalog_playlist_created(JsonObject *playlist, gpointer data)
{
    BroadcastView *view = data;
    radios_automation_playlists_changed(GTK_WIDGET(view->ctx->parent));
    playlist_open(view, radios_json_string(playlist, "_id", ""));
    catalog_message(view, "Seçilen içeriklerle playlist oluşturuldu.");
}

static GtkWidget *
catalog_view(BroadcastView *view)
{
    GtkWidget *pane = named_box(GTK_ORIENTATION_VERTICAL, 9, "broadcast-library-body");
    add_class(pane, "broadcast-right-pane");
    gtk_widget_set_name(pane, "broadcast-library-pane");
    GtkWidget *header = named_box(GTK_ORIENTATION_HORIZONTAL, 8, "broadcast-catalog-header");
    gtk_widget_set_name(header, "broadcast-catalog-header");
    GtkWidget *kinds = named_box(GTK_ORIENTATION_HORIZONTAL, 4, "broadcast-catalog-kinds");
    const gchar *names[] = {"Parçalar", "Jingle", "Anons", "Reklam", "Programlar", "Playlist"};
    const gchar *kind_ids[] = {"music", "jingle", "announcement", "advertisement", "program", "playlist"};
    const gchar *kind_names[] = {"broadcast-kind-music", "broadcast-kind-jingle", "broadcast-kind-announcement", "broadcast-kind-advert", "broadcast-kind-programme", "broadcast-kind-playlist"};
    GtkRadioButton *group = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *choice = gtk_radio_button_new_with_label_from_widget(group, names[i]);
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(choice), FALSE);
        add_class(choice, "broadcast-catalog-kind"); gtk_widget_set_name(choice, kind_names[i]);
        if (group == NULL) { group = GTK_RADIO_BUTTON(choice); view->catalog_music = choice; }
        g_object_set_data(G_OBJECT(choice), "catalog-kind", (gpointer)kind_ids[i]);
        g_signal_connect(choice, "toggled", G_CALLBACK(catalog_kind_changed), view);
        g_signal_connect(choice, "clicked", G_CALLBACK(catalog_kind_clicked), view);
        pack(kinds, choice, TRUE);
    }
    pack(header, kinds, TRUE);
    pack(header, programme_toolbar(view), FALSE);
    pack(pane, header, FALSE);
    view->catalog_content = gtk_stack_new();
    gtk_widget_set_name(view->catalog_content, "broadcast-catalog-content");
    gtk_stack_set_homogeneous(GTK_STACK(view->catalog_content), FALSE);
    gtk_stack_set_transition_type(GTK_STACK(view->catalog_content), GTK_STACK_TRANSITION_TYPE_NONE);
    pack(pane, view->catalog_content, TRUE);
    GtkWidget *body = named_box(GTK_ORIENTATION_VERTICAL, 9, "broadcast-media-body");
    gtk_stack_add_named(GTK_STACK(view->catalog_content), body, "media");
    view->playlist_panel = playlist_tools(view);
    gtk_stack_add_named(GTK_STACK(view->catalog_content), view->playlist_panel, "playlists");
    gtk_stack_set_visible_child_name(GTK_STACK(view->catalog_content), "media");
    GtkWidget *search = named_box(GTK_ORIENTATION_VERTICAL, 7, "broadcast-library-search");
    GtkWidget *source = named_box(GTK_ORIENTATION_HORIZONTAL, 7, "broadcast-catalog-source-row");
    view->catalog_kind = "music";
    const gchar *const fields[] = {"Müzik", "Playlist", NULL};
    pack(source, radios_ui_label("Arama alanı", NULL), FALSE);
    view->catalog_field = radios_ui_combo(fields, 0); gtk_widget_set_name(view->catalog_field,"broadcast-catalog-field"); pack(source, view->catalog_field, TRUE);
    g_signal_connect(view->catalog_field, "changed", G_CALLBACK(catalog_search), view);
    pack(search, source, FALSE);
    GtkWidget *row = named_box(GTK_ORIENTATION_HORIZONTAL, 7, "ld-search-row");
    view->query = radios_ui_entry("Medya arşivi · Parça, sanatçı veya albüm ara…");
    gtk_widget_set_name(view->query, "broadcast-catalog-query");
    gtk_entry_set_max_length(GTK_ENTRY(view->query),256);
    pack(row, view->query, TRUE);
    GtkWidget *find = button(row, "Ara", "ld-btn", view->ctx, NULL);
    g_signal_connect(find, "clicked", G_CALLBACK(catalog_search), view);
    g_signal_connect(view->query, "activate", G_CALLBACK(catalog_search), view);
    pack(search, row, FALSE);
    pack(body, search, FALSE);
    GtkWidget *toolbar = named_box(GTK_ORIENTATION_HORIZONTAL, 7, "broadcast-library-toolbar");
    view->catalog_count = radios_ui_label("— kayıt", NULL);
    gtk_widget_set_name(view->catalog_count, "broadcast-catalog-count");
    pack(toolbar, view->catalog_count, TRUE);
    pack(body, toolbar, FALSE);
    const gchar *headers[] = {"Sanatçı", "Başlık", "Albüm", "Tür", "Süre", "Kaynak"};
    GtkWidget *table = radios_ui_table(headers, G_N_ELEMENTS(headers));
    view->catalog_table = table;
    gtk_widget_set_name(table, "broadcast-media-table");
    add_class(table, "broadcast-library-table-scroll");
    pack(body, table, TRUE);
    GtkWidget *inspector = named_box(GTK_ORIENTATION_VERTICAL, 7, "broadcast-catalog-inspector");
    GtkWidget *heading = named_box(GTK_ORIENTATION_HORIZONTAL, 8, "broadcast-catalog-heading");
    view->catalog_title = radios_ui_label("İçerik seçilmedi", "heading"); gtk_widget_set_name(view->catalog_title, "broadcast-catalog-title");
    pack(heading, view->catalog_title, TRUE);
    GtkWidget *actions = named_box(GTK_ORIENTATION_HORIZONTAL, 5, "broadcast-catalog-actions");
    GtkWidget *edit = button(actions, "Düzenle", "ld-btn", view->ctx, NULL); view->catalog_edit = edit;
    gtk_widget_set_name(edit, "broadcast-catalog-edit");
    g_signal_connect(edit, "clicked", G_CALLBACK(catalog_open), view);
    GtkTreeView *catalog_tree = g_object_get_data(G_OBJECT(table), "tree-view");
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(catalog_tree), GTK_SELECTION_MULTIPLE);
    view->catalog_create_playlist = radios_catalogue_playlist_button_new(view->ctx, view->page,
        gtk_tree_view_get_selection(catalog_tree), catalog_playlist_selection, catalog_playlist_created, view);
    pack(toolbar, view->catalog_create_playlist, FALSE);
    g_signal_connect(gtk_tree_view_get_selection(catalog_tree), "changed", G_CALLBACK(catalog_selection), view);
    g_signal_connect(catalog_tree, "row-activated", G_CALLBACK(catalog_activated), view);
    /* Rows drag onto the flow or a player: one target, the payload names its kind. */
    gtk_tree_view_enable_model_drag_source(catalog_tree, GDK_BUTTON1_MASK, &radios_drag_targets(NULL)[RADIOS_DRAG_INFO_MEDIA - 1], 1, GDK_ACTION_COPY);
    g_signal_connect(catalog_tree, "drag-data-get", G_CALLBACK(catalog_drag_get), view);
    gtk_widget_set_sensitive(edit, FALSE);
    view->catalog_insert = button(actions, "Bloğa ekle", "ld-btn--primary", view->ctx, NULL);
    view->catalog_replace = button(actions, "Seçiliyle değiştir", "ld-btn", view->ctx, NULL);
    gtk_widget_set_name(view->catalog_insert, "broadcast-catalog-insert"); gtk_widget_set_name(view->catalog_replace, "broadcast-catalog-replace");
    g_signal_connect(view->catalog_insert, "clicked", G_CALLBACK(catalog_to_programme), view);
    g_signal_connect(view->catalog_replace, "clicked", G_CALLBACK(catalog_to_programme), view);
    pack(heading, actions, FALSE);
    pack(inspector, heading, FALSE);
    GtkWidget *details = named_box(GTK_ORIENTATION_HORIZONTAL, 14, "broadcast-catalog-details");
    const gchar *detail_names[] = {"Sanatçı", "Albüm", "Süre", "Kaynak"};
    for (guint i = 0; i < G_N_ELEMENTS(detail_names); ++i) {
        GtkWidget *pair = named_box(GTK_ORIENTATION_VERTICAL, 2, NULL);
        pack(pair, radios_ui_label(detail_names[i], "broadcast-catalog-detail-name"), FALSE);
        view->catalog_details[i] = radios_ui_label("—", "broadcast-catalog-detail-value");
        gtk_label_set_line_wrap(GTK_LABEL(view->catalog_details[i]), FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(view->catalog_details[i]), PANGO_ELLIPSIZE_END);
        pack(pair, view->catalog_details[i], FALSE); pack(details, pair, TRUE);
    }
    pack(inspector, details, FALSE);
    pack(body, inspector, FALSE);
    view->catalog_status = radios_ui_label("", "application-status");
    gtk_widget_set_name(view->catalog_status, "broadcast-catalog-status"); gtk_label_set_line_wrap(GTK_LABEL(view->catalog_status), TRUE); pack(body, view->catalog_status, FALSE);
    catalog_actions_update(view);
    return pane;
}

static void
player_count_update(BroadcastView *view)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(view->players));
    gchar *text = g_strdup_printf("%u oynatıcı", g_list_length(children));
    gtk_label_set_text(GTK_LABEL(view->player_count), text);
    queue_selector_refresh(view);
    g_free(text);
    g_list_free(children);
    if (!view->ctx->players && !view->loading && view->channel_document && radios_data_document_ready(view->channel_document)) {
        JsonNode *config = json_node_new(JSON_NODE_OBJECT); json_node_take_object(config, json_object_new());
        GList *cards = gtk_container_get_children(GTK_CONTAINER(view->players));
        json_object_set_int_member(json_node_get_object(config), "deck_count", MAX(1, g_list_length(cards)));
        radios_data_document_save(view->channel_document, config); json_node_free(config); g_list_free(cards);
    }
}

static void
player_remove(GtkButton *button_widget, gpointer data)
{
    BroadcastView *view = data;
    GList *cards = gtk_container_get_children(GTK_CONTAINER(view->players));
    guint count = g_list_length(cards); g_list_free(cards);
    if (count <= 1) { view->ctx->notify(view->ctx->notify_data, "En az bir oynatıcı kalmalı."); return; }
    GtkWidget *card = g_object_get_data(G_OBJECT(button_widget), "radios-player-card");
    if (view->ctx->players) {
        const gchar *id = g_object_get_data(G_OBJECT(card), "radios-player-id");
        guint slot = id ? (guint)g_ascii_strtoull(id, NULL, 10) : 0;
        const RadioSPlayerTopology *current = radios_player_workspace_topology(view->ctx->players);
        if (!current || !slot || !radios_player_workspace_ready(view->ctx->players)) return;
        RadioSPlayerTopology next = *current; GError *error = NULL;
        if (!radios_player_topology_remove_player(&next, slot - 1, &error) ||
            !radios_player_workspace_replace(view->ctx->players, &next, &error)) {
            programme_notice(view, error ? error->message : "Oynatıcı kaldırılamadı."); g_clear_error(&error); return;
        }
        players_reconcile(view); return;
    }
    GtkWidget *wrapper = gtk_widget_get_parent(card);
    if (GTK_IS_ASPECT_FRAME(wrapper)) wrapper = gtk_widget_get_parent(wrapper);
    gtk_widget_destroy(GTK_IS_FLOW_BOX_CHILD(wrapper) ? wrapper : card);
    player_count_update(view);
}

static void
volume_change(GtkRange *range, gpointer data)
{
    GtkLabel *value = data;
    gchar *text = g_strdup_printf("%.1f dB", gtk_range_get_value(range));
    gtk_label_set_text(value, text);
    g_free(text);
}

/* A live player card plays its inputs ("Girdiler") in order: verified library
 * media on its own deck. */
typedef struct {
    RadioSViewContext *ctx;
    BroadcastView *view;
    GtkWidget *card, *state, *elapsed, *duration, *seek, *play, *pause, *stop, *next, *cue, *gain, *main;
    GtkWidget *title, *metadata, *content_status, *next_label, *inputs, *inputs_list, *clear;
    GtkWidget *sync, *sync_target, *autodj_mode, *autodj_target, *autodj_start, *autodj_state, *bpm, *waveform, *grid_edit;
    GtkWidget *independent_controls, *autodj_target_row;
    gchar *sync_signature;
    gint sync_selected;
    GtkWidget *rotation_load, *rotation_status, *output_channel;
    gchar *output_signature;
    /* Weak pointers: the DJ page carrying "dj-channel-box", the fallback
     * channel box and the channel strips, so the 100 ms tick does not search
     * the whole window by widget name. */
    GtkWidget *output_dj_view, *output_channels_fallback, *output_strips[RADIOS_AUDIO_MIXER_CHANNELS];
    gint64 output_lookup_after;
    GtkWidget *weekly_load, *weekly_repeat, *weekly_next;
    RadioSPlayerWeekly *weekly;
    JsonNode *rotation_prepared;
    RadioSDataClient *rotation_client;
    gchar rotation_document[37], rotation_id[37];
    gchar *rotation_name;
    gint64 rotation_revision;
    gboolean rotation_loading, rotation_starting;
    gboolean controls_updating;
    gboolean seek_dragging;
    guint64 seek_generation;
    guint64 choices_signature, analysis_serial;
    GHashTable *analysis_seen;
    RadioSDataClient *analysis_client;
    GCancellable *analysis_cancel;
    gint64 analysis_retry_after;
    gchar analysis_media_id[37], analysis_version_id[37];
    gchar analysis_watched_media[37], analysis_watched_version[37];
    JsonArray *queue;
    gint current, loaded_current;
    gboolean running, destroyed, lookup_pending, queue_active;
    guint64 load_serial, assignment_serial;
    gboolean playlist_pending;
    guint deck, timer;
} PlayerAudio;

static RadioSViewContext *player_managed_context(PlayerAudio *player);
static void player_rotation_load_clicked(GtkButton *button, gpointer value);
static void player_output_refresh(PlayerAudio *player);
static void player_output_forget(PlayerAudio *player);
static gboolean player_output_ready(PlayerAudio *player);
static JsonObject *player_input(PlayerAudio *player, gint index);
static void player_schema_create_clicked(GtkButton *button, gpointer value)
{
    (void)button; PlayerAudio *player = value;
    if (player->destroyed || player_managed_context(player)) return;
    const gchar *id = radios_json_string(player_input(player, player->loaded_current >= 0 ? player->loaded_current : player->current), "playlist_id", "");
    GError *error = NULL;
    if (!radios_automation_calendar_create_player_schema(player->ctx, id, &error) && player->ctx->notify)
        player->ctx->notify(player->ctx->notify_data, error ? error->message : "Otomasyon şeması açılamadı.");
    g_clear_error(&error);
}
static gboolean player_weekly_snapshot(PlayerAudio *player, RadioSPlayerWeeklySnapshot *snapshot);
static gboolean player_weekly_armed(PlayerAudio *player);
static void player_weekly_render(PlayerAudio *player);
static void player_weekly_start(PlayerAudio *player);
/* Pango single-line mode can replace embedded line breaks with a taller
 * fallback glyph. Flatten display whitespace while keeping original tooltips. */
static void player_label_set_line(GtkWidget *label, const gchar *text)
{
    GString *line = g_string_new(NULL);
    for (const gchar *cursor = text; *cursor; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        if (character != ' ' && g_unichar_isspace(character)) character = ' ';
        g_string_append_unichar(line, character);
    }
    gtk_label_set_text(GTK_LABEL(label), line->str); g_string_free(line, TRUE);
}
static void player_button_set_label(GtkWidget *button, const gchar *text)
{
    if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(button)), text))
        gtk_button_set_label(GTK_BUTTON(button), text);
}
static void player_rotation_clear(PlayerAudio *player, const gchar *reason)
{
    if (player->weekly) { radios_player_weekly_free(player->weekly); player->weekly = NULL; }
    g_object_set_data(G_OBJECT(player->card), "radios-player-weekly", NULL);
    g_clear_pointer(&player->rotation_prepared, json_node_free);
    if (player->rotation_client) radios_data_client_unref(player->rotation_client);
    player->rotation_client = NULL;
    g_clear_pointer(&player->rotation_name, g_free);
    player->rotation_document[0] = player->rotation_id[0] = 0;
    player->rotation_revision = 0; player->rotation_starting = FALSE;
    g_object_set_data(G_OBJECT(player->card), "radios-player-rotation-id", NULL);
    g_object_set_data(G_OBJECT(player->card), "radios-player-rotation-document", NULL);
    g_object_set_data(G_OBJECT(player->card), "radios-player-rotation-name", NULL);
    if (player->rotation_status) {
        player_label_set_line(player->rotation_status, reason ? reason : "Otomasyon yüklenmedi");
        gtk_widget_set_tooltip_text(player->rotation_status, reason);
    }
    if (player->autodj_start) gtk_widget_set_sensitive(player->autodj_start, FALSE);
}
static gboolean player_rotation_current(PlayerAudio *player)
{
    if (!player->rotation_prepared || !player->rotation_client ||
        !g_uuid_string_is_valid(player->rotation_id) || !g_uuid_string_is_valid(player->rotation_document) ||
        !radios_data_connected(player->ctx)) return FALSE;
    RadioSDataClient *client = radios_data_client_acquire(player->ctx);
    gboolean same = client == player->rotation_client;
    if (client) radios_data_client_unref(client);
    return same;
}
static gboolean player_rotation_routed(JsonObject *prepared)
{
    JsonNode *node = prepared ? json_object_get_member(prepared, "tracks") : NULL;
    JsonArray *tracks = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
    for (guint i = 0; tracks && i < json_array_get_length(tracks); ++i) {
        JsonNode *row = json_array_get_element(tracks, i);
        if (JSON_NODE_HOLDS_OBJECT(row) && json_object_has_member(json_node_get_object(row), "calendar_routing")) return TRUE;
    }
    return FALSE;
}
/* A player's problem is written on its own card, beside the control that
 * failed, as well as in the header notice: no details view is needed. */
static void player_notice(PlayerAudio *player, const gchar *message)
{
    if (!message || !*message) return;
    if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, message);
    if (player->rotation_status) {
        gchar *line = g_strdup_printf("⚠ %s", message);
        gtk_label_set_text(GTK_LABEL(player->rotation_status), line); gtk_widget_set_tooltip_text(player->rotation_status, message); g_free(line);
    }
}
static void player_audio_error(PlayerAudio *player, GError *error)
{
    if (error) player_notice(player, error->message);
    g_clear_error(&error);
}
static JsonObject *player_input(PlayerAudio *player, gint index)
{ return player->queue && index >= 0 && (guint)index < json_array_get_length(player->queue) ? json_array_get_object_element(player->queue, (guint)index) : NULL; }
/* Intent IDs never repeat: a lookup from a replaced queue must not become
 * current again when the next Play starts another request. */
static void player_input_invalidate(PlayerAudio *player)
{
    ++player->load_serial;
    player->lookup_pending = FALSE;
}
/* Queue assignment has its own generation: normal playback may continue while
 * a saved playlist is read, but replacing/clearing its target cancels that read. */
static void player_assignment_invalidate(PlayerAudio *player)
{
    ++player->assignment_serial;
    if (player->weekly) radios_player_weekly_stop(player->weekly);
    player->playlist_pending = FALSE;
    player->rotation_loading = player->rotation_starting = FALSE;
}
static gboolean player_input_loaded(PlayerAudio *player, const RadioSAudioUiSnapshot *snapshot)
{
    JsonObject *input = player_input(player, player->current);
    return input && player->loaded_current == player->current && snapshot->media_id[0] &&
        g_str_equal(snapshot->media_id, radios_json_string(input, "media_id", "")) &&
        g_str_equal(snapshot->version_id, radios_json_string(input, "version_id", ""));
}
void radios_broadcast_player_library_loaded(RadioSViewContext *ctx, guint deck)
{
    if (!ctx || !ctx->parent || deck >= 32) return;
    gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
    GtkWidget *card = g_object_get_data(G_OBJECT(ctx->parent), key); g_free(key);
    PlayerAudio *player = card ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
    if (!player || player->destroyed) return;
    player_assignment_invalidate(player);
    player_rotation_clear(player, NULL);
    player_input_invalidate(player);
    player->running = player->queue_active = FALSE;
    player->loaded_current = -1;
}
static gchar *player_clock(gint64 milliseconds, gboolean tenths)
{
    milliseconds = MAX(0, milliseconds);
    return tenths ? g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ".%01" G_GINT64_FORMAT, milliseconds / 60000, milliseconds / 1000 % 60, milliseconds / 100 % 10) :
        g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, milliseconds / 60000, milliseconds / 1000 % 60);
}
static const RadioSPlayerDefinition *player_definition(PlayerAudio *player)
{
    return radios_player_workspace_ready(player->ctx->players) ?
        radios_player_topology_player_const(radios_player_workspace_topology(player->ctx->players), player->deck) : NULL;
}
static void player_group_metadata(GtkWidget *card, const RadioSPlayerTopology *topology, const RadioSPlayerDefinition *definition)
{
    const gchar *id = definition ? definition->cluster_id : "";
    const RadioSPlayerCluster *cluster = *id ? radios_player_topology_cluster_const(topology, id) : NULL;
    g_object_set_data_full(G_OBJECT(card), "radios-player-cluster-id", g_strdup(id), g_free);
    g_object_set_data_full(G_OBJECT(card), "radios-player-cluster-name", g_strdup(cluster ? cluster->name : "Kümesizler"), g_free);
}
static gboolean player_sync_peer_ready(PlayerAudio *player, gint deck)
{
    if (deck < 0 || deck >= 32 || deck == (gint)player->deck) return FALSE;
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(player->ctx->players);
    if (player->ctx->players && (!radios_player_workspace_ready(player->ctx->players) ||
        !radios_player_topology_same_group(topology, player->deck, (guint)deck))) return FALSE;
    if (!player->ctx->players) {
        gchar *key = g_strdup_printf("radios-deck-owner-%d", deck);
        gboolean present = g_object_get_data(G_OBJECT(player->ctx->parent), key) != NULL;
        g_free(key); if (!present) return FALSE;
    }
    RadioSAudioUiSnapshot candidate = {0};
    return radios_audio_ui_get_snapshot(player->ctx, (guint)deck, &candidate) &&
        candidate.state == RADIOS_AUDIO_UI_PLAYING && !candidate.runner_paused && candidate.media_id[0] && candidate.bpm > 0;
}
static void player_sync_targets_refresh(PlayerAudio *player, const RadioSAudioUiSnapshot *snapshot)
{
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(player->ctx->players);
    GString *signature = g_string_new(NULL); guint32 available = 0;
    for (guint deck = 0; deck < 32; ++deck) {
        if (!player_sync_peer_ready(player, (gint)deck)) continue;
        const RadioSPlayerDefinition *peer = radios_player_topology_player_const(topology, deck);
        available |= (guint32)1 << deck;
        g_string_append_printf(signature, "%u:%s;", deck, peer ? peer->name : "");
    }
    if (snapshot->sync_master >= 0) player->sync_selected = snapshot->sync_master;
    if (player->sync_selected < -1 || player->sync_selected >= 32 ||
        (player->sync_selected >= 0 && !(available & ((guint32)1 << player->sync_selected)))) player->sync_selected = -1;
    g_string_append_printf(signature, "selected:%d", player->sync_selected);
    if (g_strcmp0(player->sync_signature, signature->str)) {
        gboolean updating = player->controls_updating; player->controls_updating = TRUE;
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(player->sync_target));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(player->sync_target), "-1", available ? "Oynatıcı seçin" : "Çalan eş yok");
        for (guint deck = 0; deck < 32; ++deck) {
            if (!(available & ((guint32)1 << deck))) continue;
            const RadioSPlayerDefinition *peer = radios_player_topology_player_const(topology, deck);
            gchar *id = g_strdup_printf("%u", deck), *label = peer && *peer->name ? g_strdup(peer->name) : g_strdup_printf("Oynatıcı %u", deck + 1);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(player->sync_target), id, label); g_free(id); g_free(label);
        }
        gchar *id = g_strdup_printf("%d", player->sync_selected);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(player->sync_target), id); g_free(id);
        player->controls_updating = updating;
        g_free(player->sync_signature); player->sync_signature = g_strdup(signature->str);
    }
    g_string_free(signature, TRUE);
    gtk_widget_set_sensitive(player->sync_target, snapshot->media_id[0] &&
        snapshot->state == RADIOS_AUDIO_UI_PLAYING && !snapshot->runner_paused && snapshot->bpm > 0 && available != 0);
}
static gboolean player_workspace_queue_ready(PlayerAudio *player)
{
    if (!player_definition(player)) return FALSE;
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = player->deck};
    JsonObject *programme = NULL; GError *error = NULL;
    gboolean ready = radios_player_workspace_get_programme(player->ctx->players, &scope, &programme, &error);
    g_clear_error(&error);
    JsonNode *events = programme ? json_object_get_member(programme, "events") : NULL;
    ready = ready && events && JSON_NODE_HOLDS_ARRAY(events) && json_array_get_length(json_node_get_array(events)) > 0;
    if (programme) json_object_unref(programme);
    return ready;
}
typedef struct { GWeakRef card; } PlayerScopeCall;
static void player_scope_call_free(gpointer data)
{ PlayerScopeCall *call = data; g_weak_ref_clear(&call->card); g_free(call); }
static void player_scope_done(RadioSViewContext *ctx, const RadioSAutomationScope *scope,
    guint source, const GError *error, gpointer data)
{
    (void)ctx; (void)scope; (void)source; PlayerScopeCall *call = data;
    GtkWidget *card = g_weak_ref_get(&call->card);
    PlayerAudio *player = card ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
    if (player && !player->destroyed) {
        player->rotation_starting = FALSE;
        if (error) player_audio_error(player, g_error_copy(error)); else player->running = FALSE;
    }
    g_clear_object(&card);
}
static JsonObject *player_inputs_programme(PlayerAudio *player)
{
    if (!player->queue || !json_array_get_length(player->queue)) return NULL;
    JsonObject *config = json_object_new();
    const gchar *name = g_object_get_data(G_OBJECT(player->card), "radios-player-name");
    json_object_set_string_member(config, "title", name ? name : "Oynatıcı playlisti"); json_object_set_string_member(config, "note", "");
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d"); g_date_time_unref(now);
    json_object_set_string_member(config, "date", date); g_free(date);
    JsonArray *events = json_array_new(); json_object_set_array_member(config, "events", events);
    JsonArray *blocks = json_array_new(); json_object_set_array_member(config, "blocks", blocks);
    JsonObject *block = NULL;
    guint count = json_array_get_length(player->queue), first = player->current >= 0 ? (guint)player->current : 0;
    for (guint n = 0; n < count; ++n) {
        JsonObject *input = json_array_get_object_element(player->queue, (first + n) % count);
        if (!g_uuid_string_is_valid(radios_json_string(input, "media_id", ""))) { json_object_unref(config); return NULL; }
        JsonObject *event = json_object_new(); gchar *id = g_uuid_string_random(); json_object_set_string_member(event, "id", id); g_free(id);
        const gchar *keys[] = {"title", "artist", "album", "media_id", "version_id", "media_file_id", "media_sha256"};
        for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) json_object_set_string_member(event, keys[i], radios_json_string(input, keys[i], ""));
        const gchar *kind = radios_json_string(input, "kind", "music");
        kind = radios_programme_kind_valid(kind) ? kind : "music";
        json_object_set_string_member(event, "kind", kind);
        const gchar *playlist = radios_json_string(input, "playlist_id", "");
        if (!g_uuid_string_is_valid(playlist)) playlist = "";
        if (!block || g_strcmp0(radios_json_string(block, "kind", ""), kind) ||
            (!g_str_equal(kind, "music") && g_strcmp0(radios_json_string(block, "playlist_id", ""), playlist))) {
            block = json_object_new(); gchar *block_id = g_uuid_string_random();
            json_object_set_string_member(block, "id", block_id); g_free(block_id);
            json_object_set_string_member(block, "kind", kind);
            if (*playlist) json_object_set_string_member(block, "playlist_id", playlist);
            json_object_set_string_member(block, "title", radios_programme_event_title(kind));
            json_object_set_string_member(block, "start_time", ""); json_object_set_string_member(block, "note", "");
            json_array_add_object_element(blocks, block);
        }
        /* A music event may span several source playlists. Keep the input's
         * own binding; its block must not claim one playlist for every song. */
        if (g_str_equal(kind, "music") && g_strcmp0(radios_json_string(block, "playlist_id", ""), playlist))
            json_object_remove_member(block, "playlist_id");
        json_object_set_string_member(event, "block_id", radios_json_string(block, "id", ""));
        json_object_set_int_member(event, "duration_ms", radios_json_int(input, "duration_ms", 0));
        const gchar *empty[] = {"start_time", "version_label", "primary_file", "backup_file", "note"};
        for (guint i = 0; i < G_N_ELEMENTS(empty); ++i) json_object_set_string_member(event, empty[i], "");
        json_array_add_object_element(events, event);
    }
    return config;
}
static gboolean player_scope_start(PlayerAudio *player, RadioSAutoDJMode mode)
{
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(player->ctx);
    if (!execution) return FALSE;
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = player->deck};
    JsonObject *programme = player_inputs_programme(player); GError *error = NULL;
    PlayerScopeCall *call = g_new0(PlayerScopeCall, 1); g_weak_ref_init(&call->card, player->card);
    const gchar *assignment = radios_player_topology_automation(radios_player_workspace_topology(player->ctx->players), player->deck);
    player->rotation_starting = TRUE;
    gboolean accepted = !programme && *assignment && !player_workspace_queue_ready(player) ?
        radios_player_scope_execution_play_schema(execution, &scope, mode, player_scope_done, call, player_scope_call_free, &error) :
        radios_player_scope_execution_play_programme(execution, &scope, programme, mode, player_scope_done, call, player_scope_call_free, &error);
    if (programme) json_object_unref(programme);
    if (!accepted) player->rotation_starting = FALSE;
    player_audio_error(player, error); return accepted;
}
static void player_mode_changed(GtkComboBox *combo, gpointer data)
{
    (void)combo; PlayerAudio *player = data;
    if (player->controls_updating || !player_definition(player) || player_weekly_armed(player) || player_managed_context(player)) return;
    RadioSPlayerTopology next = *radios_player_workspace_topology(player->ctx->players);
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(player->autodj_mode));
    RadioSAutoDJMode mode = id ? (RadioSAutoDJMode)g_ascii_strtoll(id, NULL, 10) : RADIOS_AUTODJ_OFF;
    gint target = mode == RADIOS_AUTODJ_TRANSITIONS ? next.players[player->deck].autodj_target : -1;
    GError *error = NULL;
    if (!radios_player_topology_set_autodj(&next, player->deck, mode, target, &error) ||
        !radios_player_workspace_replace(player->ctx->players, &next, &error)) player_audio_error(player, error);
    player->choices_signature = G_MAXUINT64;
}
/* Loading an automation source means "play it automatically": a workspace
 * player left on Manuel switches to Yarı AutoDJ so Başlat is ready at once.
 * Choosing a target player afterwards turns it into a transition pair. */
static void player_autodj_default(PlayerAudio *player)
{
    const RadioSPlayerDefinition *definition = player_definition(player);
    if (!definition || definition->autodj_mode != RADIOS_AUTODJ_OFF || player_managed_context(player) || player_weekly_armed(player)) return;
    RadioSPlayerTopology next = *radios_player_workspace_topology(player->ctx->players); GError *error = NULL;
    if (!radios_player_topology_set_autodj(&next, player->deck, RADIOS_AUTODJ_SEMI, -1, &error) ||
        !radios_player_workspace_replace(player->ctx->players, &next, &error)) { player_audio_error(player, error); return; }
    player->choices_signature = G_MAXUINT64;
}
static void player_options_refresh(PlayerAudio *player)
{

    RadioSAudioPlayerOptions options = {-1, -1}; radios_audio_ui_player_options(player->ctx, player->deck, &options);
    const RadioSPlayerDefinition *definition = player_definition(player);
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(player->ctx->players);
    guint64 mask = 0;
    for (guint deck = 0; deck < 32; ++deck) {
        if (definition) {
            if (radios_player_topology_same_group(topology, player->deck, deck)) mask |= G_GUINT64_CONSTANT(1) << deck;
        } else {
            gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
            if (g_object_get_data(G_OBJECT(player->ctx->parent), key)) mask |= G_GUINT64_CONSTANT(1) << deck;
            g_free(key);
        }
    }
    gint selection = definition ? definition->autodj_target : options.autodj_target;
    guint mode = definition ? (guint)definition->autodj_mode : 0;
    guint64 signature = mask | ((guint64)(selection + 1) << 38) | ((guint64)mode << 44);
    if (signature == player->choices_signature) return;
    player->choices_signature = signature; player->controls_updating = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(player->autodj_target));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(player->autodj_target), "-1",
        !definition ? "Kapalı" : mode == RADIOS_AUTODJ_TRANSITIONS ? "Otomatik devam" : "Yalnız bu oynatıcı");
    for (guint deck = 0; deck < 32; ++deck) {
        if (deck == player->deck || !(mask & (G_GUINT64_CONSTANT(1) << deck))) continue;
        const RadioSPlayerDefinition *peer = radios_player_topology_player_const(topology, deck);
        gchar *id = g_strdup_printf("%u", deck), *label = peer && *peer->name ? g_strdup(peer->name) : g_strdup_printf("Oynatıcı %u", deck + 1);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(player->autodj_target), id, label); g_free(id); g_free(label);
    }
    gchar *id = g_strdup_printf("%d", selection); gtk_combo_box_set_active_id(GTK_COMBO_BOX(player->autodj_target), id); g_free(id);
    if (player->autodj_mode) {
        id = g_strdup_printf("%u", mode); gtk_combo_box_set_active_id(GTK_COMBO_BOX(player->autodj_mode), id); g_free(id);
    }
    player->controls_updating = FALSE;
}
static void player_options_changed(GtkComboBox *combo, gpointer data)
{
    (void)combo; PlayerAudio *player = data; if (player->controls_updating) return;
    if (player_weekly_armed(player)) { player->choices_signature = G_MAXUINT64; player_options_refresh(player); return; }
    const gchar *target = gtk_combo_box_get_active_id(GTK_COMBO_BOX(player->autodj_target)); if (!target) return;
    gint selected = (gint)g_ascii_strtoll(target, NULL, 10); GError *error = NULL;
    const RadioSPlayerDefinition *definition = player_definition(player);
    if (definition) {
        RadioSPlayerTopology next = *radios_player_workspace_topology(player->ctx->players);
        RadioSAutoDJMode mode = selected >= 0 ? RADIOS_AUTODJ_TRANSITIONS : definition->autodj_mode;
        if (!radios_player_topology_set_autodj(&next, player->deck, mode, selected, &error) ||
            !radios_player_workspace_replace(player->ctx->players, &next, &error)) {
            player_audio_error(player, error); player->choices_signature = G_MAXUINT64; player_options_refresh(player); return;
        }
        player->choices_signature = G_MAXUINT64; player_options_refresh(player); return;
    }
    RadioSAudioPlayerOptions options = {-1, selected}; radios_audio_ui_player_options(player->ctx, player->deck, &options); options.autodj_target = selected;
    if (!radios_audio_ui_set_player_options(player->ctx, player->deck, &options, &error)) player_audio_error(player, error);
    player->choices_signature = G_MAXUINT64; player_options_refresh(player);
}
static void player_sync_changed(GtkToggleButton *button, gpointer data)
{
    PlayerAudio *player = data; if (player->controls_updating) return;
    RadioSAudioUiSnapshot snapshot = {0}; radios_audio_ui_get_snapshot(player->ctx, player->deck, &snapshot);
    GError *error = NULL;
    gboolean enabled = gtk_toggle_button_get_active(button);
    gint target = enabled ? player->sync_selected : snapshot.sync_master;
    gboolean eligible = !enabled || (snapshot.media_id[0] && snapshot.bpm > 0 &&
        snapshot.state == RADIOS_AUDIO_UI_PLAYING && !snapshot.runner_paused && player_sync_peer_ready(player, target));
    if (!eligible) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "SYNC için Play durumunda aynı kümeden veya Kümesizler içinden çalan bir oynatıcı seçin.");
    if (!eligible || !radios_audio_ui_sync(player->ctx, player->deck, target, enabled, &error)) {
        player_audio_error(player, error); player->controls_updating = TRUE;
        gtk_toggle_button_set_active(button, snapshot.sync_master >= 0); player->controls_updating = FALSE;
    }
}
static void player_sync_target_changed(GtkComboBox *combo, gpointer data)
{
    PlayerAudio *player = data; if (player->controls_updating) return;
    const gchar *id = gtk_combo_box_get_active_id(combo); if (!id) return;
    player->sync_selected = (gint)g_ascii_strtoll(id, NULL, 10);
    RadioSAudioUiSnapshot snapshot = {0}; radios_audio_ui_get_snapshot(player->ctx, player->deck, &snapshot);
    if (snapshot.sync_master >= 0 && snapshot.sync_master != player->sync_selected) {
        if (player->sync_selected < 0) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(player->sync), FALSE);
        else player_sync_changed(GTK_TOGGLE_BUTTON(player->sync), player);
    }
}
static void player_grid_clicked(GtkButton *button, gpointer data)
{
    (void)button; PlayerAudio *player = data;
    if (!radios_player_waveform_edit_grid(player->waveform) && player->ctx->notify)
        player->ctx->notify(player->ctx->notify_data,
            "Önce yerel bir parça yükleyin; dalga biçimi yüklenince BPM ve dörtlükler düzenlenebilir.");
}
typedef struct { guint64 serial, generation; gchar *key; } PlayerAnalysis;
static void player_analysis_free(gpointer value)
{ PlayerAnalysis *job = value; g_free(job->key); g_free(job); }
static void player_analysis_ready(RadioSViewContext *ctx, GtkWidget *card, JsonNode *node, const GError *error, gpointer data)
{
    PlayerAnalysis *job = data; PlayerAudio *player = g_object_get_data(G_OBJECT(card), "radios-player-audio");
    if (!player || job->serial != player->analysis_serial) return;
    g_clear_object(&player->analysis_cancel);
    player->analysis_media_id[0] = player->analysis_version_id[0] = 0;
    if (error) {
        gboolean permanent = g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_FORBIDDEN) ||
            g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_NOT_FOUND) ||
            g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_VALIDATION) ||
            g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) ||
            g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        if (!permanent) {
            g_hash_table_remove(player->analysis_seen, job->key);
            player->analysis_retry_after = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
        }
        if (ctx->notify) ctx->notify(ctx->notify_data, error->message);
    }
    if (node && JSON_NODE_HOLDS_OBJECT(node)) {
        JsonObject *detail = json_node_get_object(node), *media = json_object_get_object_member(detail, "media");
        RadioSAudioUiSnapshot snapshot = {0}; radios_audio_ui_get_snapshot(ctx, player->deck, &snapshot);
        JsonObject *analysis = json_object_get_object_member(detail, "analysis"); RadioSRhythmAnalysis rhythm;
        if (analysis && snapshot.generation == job->generation && g_str_equal(snapshot.media_id, radios_json_string(media, "_id", "")) &&
            g_str_equal(snapshot.version_id, radios_json_string(media, "version_id", ""))) {
            JsonNode *rhythm_node = json_object_get_member(analysis, "rhythm");
            RadioSLoudness loudness; gboolean measured = radios_loudness_read(analysis, &loudness);
            gboolean grid = rhythm_node && JSON_NODE_HOLDS_OBJECT(rhythm_node) && radios_rhythm_analysis_read(json_node_get_object(rhythm_node), &rhythm);
            RadioSAudioEngine *engine = grid || measured ? radios_audio_ui_engine_acquire(ctx) : NULL;
            if (engine && grid) radios_audio_engine_set_deck_rhythm(engine, player->deck, snapshot.generation, &rhythm, NULL);
            if (engine && measured) radios_audio_engine_set_deck_loudness(engine, player->deck, snapshot.generation, &loudness, NULL);
            if (engine) radios_audio_engine_unref(engine);
            JsonNode *previous_node = g_object_get_data(G_OBJECT(card), "radios-deck-media-detail");
            JsonObject *previous = previous_node && JSON_NODE_HOLDS_OBJECT(previous_node) ? json_node_get_object(previous_node) : NULL;
            JsonObject *previous_media = previous ? json_object_get_object_member(previous, "media") : NULL;
            JsonObject *merged = radios_programme_clone_object(detail);
            if (previous_media && g_str_equal(snapshot.media_id, radios_json_string(previous_media, "_id", "")) &&
                g_str_equal(snapshot.version_id, radios_json_string(previous_media, "version_id", ""))) {
                const gchar *objects[] = {"media", "markers"};
                for (guint index = 0; index < G_N_ELEMENTS(objects); ++index) {
                    JsonNode *old_node = json_object_get_member(previous, objects[index]);
                    JsonNode *new_node = json_object_get_member(merged, objects[index]);
                    JsonObject *old_object = old_node && JSON_NODE_HOLDS_OBJECT(old_node) ? json_node_get_object(old_node) : NULL;
                    JsonObject *new_object = new_node && JSON_NODE_HOLDS_OBJECT(new_node) ? json_node_get_object(new_node) : NULL;
                    if (old_object && radios_json_int(old_object, "_rev", 0) > radios_json_int(new_object, "_rev", 0))
                        json_object_set_object_member(merged, objects[index], radios_programme_clone_object(old_object));
                }
                if (radios_json_int(previous, "analysis_revision", 0) > radios_json_int(merged, "analysis_revision", 0)) {
                    JsonNode *old_analysis = json_object_get_member(previous, "analysis");
                    if (old_analysis) json_object_set_member(merged, "analysis", json_node_copy(old_analysis));
                    json_object_set_int_member(merged, "analysis_revision", radios_json_int(previous, "analysis_revision", 0));
                }
            }
            JsonNode *cache = json_node_new(JSON_NODE_OBJECT); json_node_take_object(cache, merged);
            g_object_set_data_full(G_OBJECT(card), "radios-deck-media-detail", cache, (GDestroyNotify)json_node_free);
        }
    }
}
static gboolean player_analysis_start(PlayerAudio *player, const gchar *id, const gchar *version, guint64 generation)
{
    if (!g_uuid_string_is_valid(id) || !g_uuid_string_is_valid(version)) return FALSE;
    gchar *key = g_strdup_printf("%s:%s:%" G_GUINT64_FORMAT, id, version, generation);
    if (g_hash_table_contains(player->analysis_seen, key)) { g_free(key); return FALSE; }
    if (g_hash_table_size(player->analysis_seen) >= 2048) g_hash_table_remove_all(player->analysis_seen);
    g_hash_table_add(player->analysis_seen, g_strdup(key));
    PlayerAnalysis *job = g_new0(PlayerAnalysis, 1); job->serial = ++player->analysis_serial; job->generation = generation; job->key = key;
    g_strlcpy(player->analysis_media_id, id, sizeof player->analysis_media_id);
    g_strlcpy(player->analysis_version_id, version, sizeof player->analysis_version_id);
    player->analysis_cancel = radios_media_rhythm_ensure_async(player->ctx, player->card, id, player_analysis_ready, job, player_analysis_free);
    if (!player->analysis_cancel) {
        /* rhythm_start has already freed job; keep only the independent key. */
        gchar *failed = g_strdup_printf("%s:%s:%" G_GUINT64_FORMAT, id, version, generation);
        g_hash_table_remove(player->analysis_seen, failed); g_free(failed);
        player->analysis_retry_after = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    }
    return TRUE;
}
static void player_analysis_update(PlayerAudio *player, const RadioSAudioUiSnapshot *snapshot)
{
    RadioSDataClient *client = radios_data_client_acquire(player->ctx);
    if (client != player->analysis_client) {
        if (player->analysis_cancel) g_cancellable_cancel(player->analysis_cancel);
        g_clear_object(&player->analysis_cancel); ++player->analysis_serial;
        g_hash_table_remove_all(player->analysis_seen); player->analysis_retry_after = 0;
        if (player->analysis_client) radios_data_client_unref(player->analysis_client);
        player->analysis_client = client; client = NULL;
    }
    if (client) radios_data_client_unref(client);
    gboolean media_changed = !g_str_equal(snapshot->media_id, player->analysis_watched_media) ||
        !g_str_equal(snapshot->version_id, player->analysis_watched_version);
    if (media_changed && player->analysis_cancel && snapshot->media_id[0] &&
        (!g_str_equal(snapshot->media_id, player->analysis_media_id) ||
         !g_str_equal(snapshot->version_id, player->analysis_version_id))) {
        /* A newly loaded deck takes priority over a queued/previous file. */
        g_cancellable_cancel(player->analysis_cancel); g_clear_object(&player->analysis_cancel);
        ++player->analysis_serial; g_hash_table_remove_all(player->analysis_seen);
        player->analysis_retry_after = 0;
    }
    g_strlcpy(player->analysis_watched_media, snapshot->media_id, sizeof player->analysis_watched_media);
    g_strlcpy(player->analysis_watched_version, snapshot->version_id, sizeof player->analysis_watched_version);
    if (player->analysis_cancel || player->destroyed || !radios_data_connected(player->ctx) ||
        g_get_monotonic_time() < player->analysis_retry_after) return;
    JsonNode *cached = g_object_get_data(G_OBJECT(player->card), "radios-deck-media-detail");
    JsonObject *detail = cached && JSON_NODE_HOLDS_OBJECT(cached) ? json_node_get_object(cached) : NULL;
    JsonObject *media = detail ? json_object_get_object_member(detail, "media") : NULL;
    JsonObject *analysis = detail ? json_object_get_object_member(detail, "analysis") : NULL;
    JsonNode *rhythm_node = analysis ? json_object_get_member(analysis, "rhythm") : NULL;
    JsonObject *rhythm = rhythm_node && JSON_NODE_HOLDS_OBJECT(rhythm_node) ? json_node_get_object(rhythm_node) : NULL;
    gboolean complete = media && rhythm &&
        g_str_equal(snapshot->media_id, radios_json_string(media, "_id", "")) &&
        g_str_equal(snapshot->version_id, radios_json_string(media, "version_id", "")) &&
        g_str_equal(radios_data_station_id(player->ctx), radios_json_string(media, "station_id", "")) &&
        radios_media_analysis_complete(analysis);
    if (complete && (snapshot->bpm <= 0 || !snapshot->loudness_measured)) {
        /* Another card may have finished this media's analysis while our
         * generation changed. Bind the validated cached rhythm and loudness to
         * this generation instead of skipping them for the new load. */
        RadioSRhythmAnalysis measured; RadioSLoudness loudness;
        gboolean grid = snapshot->bpm <= 0 && radios_rhythm_analysis_read(rhythm, &measured);
        gboolean level = !snapshot->loudness_measured && radios_loudness_read(analysis, &loudness) && loudness.measured;
        RadioSAudioEngine *engine = grid || level ? radios_audio_ui_engine_acquire(player->ctx) : NULL;
        if (engine && grid) radios_audio_engine_set_deck_rhythm(engine, player->deck, snapshot->generation, &measured, NULL);
        if (engine && level) radios_audio_engine_set_deck_loudness(engine, player->deck, snapshot->generation, &loudness, NULL);
        if (engine) radios_audio_engine_unref(engine);
    }
    if (!complete && player_analysis_start(player, snapshot->media_id, snapshot->version_id, snapshot->generation)) return;
    for (guint index = 0; player->queue && index < json_array_get_length(player->queue); ++index) {
        JsonObject *input = json_array_get_object_element(player->queue, index);
        if (player_analysis_start(player, radios_json_string(input, "media_id", ""), radios_json_string(input, "version_id", ""), 0)) return;
    }
}
typedef struct {
    guint64 assignment, input_serial, intent, generations[32], source_generation;
    GWeakRef cards[32];
    guint source, target, decks[32], count;
    gboolean routed;
    gint current;
} PlayerRotationStart;
static void player_rotation_start_free(gpointer value)
{
    PlayerRotationStart *job = value;
    for (guint i = 0; i < job->count; ++i) g_weak_ref_clear(&job->cards[i]);
    g_free(job);
}
static void player_rotation_start_ready(RadioSViewContext *ctx, GtkWidget *card,
    JsonNode *response, const GError *error, gpointer value)
{
    PlayerRotationStart *job = value;
    PlayerAudio *player = g_object_get_data(G_OBJECT(card), "radios-player-audio");
    if (!player || player->destroyed || player->assignment_serial != job->assignment) return;
    player->rotation_starting = FALSE;
    if (error) {
        gboolean invalidated = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA) ||
            g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_NOT_FOUND) ||
            g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CONFLICT) ||
            g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_FORBIDDEN) ||
            g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_VALIDATION);
        if (invalidated) player_rotation_clear(player, "Rotasyon değişti · yeniden yükleyin");
        if (ctx->notify) ctx->notify(ctx->notify_data, error->message);
        return;
    }
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx); RadioSAudioSnapshot snapshot = {0};
    if (engine) { radios_audio_engine_snapshot(engine, &snapshot); radios_audio_engine_unref(engine); }
    gboolean current_cards = TRUE;
    for (guint i = 0; i < job->count; ++i) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", job->decks[i]);
        GtkWidget *target = g_object_get_data(G_OBJECT(ctx->parent), key); g_free(key);
        GtkWidget *original = g_weak_ref_get(&job->cards[i]);
        current_cards = current_cards && target && target == original && !gtk_widget_in_destruction(target) &&
            snapshot.decks[job->decks[i]].generation == job->generations[i];
        g_clear_object(&original);
    }
    RadioSAudioPlayerOptions options = {-1, -1}; radios_audio_ui_player_options(ctx, player->deck, &options);
    if (!engine || !current_cards || (!job->routed && options.autodj_target != (gint)job->target) || !player_rotation_current(player) ||
        player->load_serial != job->input_serial || player->current != job->current ||
        radios_playout_ui_intent(ctx) != job->intent ||
        snapshot.decks[job->source].generation != job->source_generation ||
        !response || !JSON_NODE_HOLDS_OBJECT(response)) {
        if (ctx->notify) ctx->notify(ctx->notify_data, "Player veya rotasyon hazırlığı değişti; AutoDJ'yi yeniden başlatın.");
        return;
    }
    guint decks[32], count = 0; gboolean routed = FALSE; GError *pool_error = NULL;
    if (!radios_playout_prepared_rotation_pool(ctx, json_node_get_object(response), job->source, job->target,
            decks, &count, &routed, &pool_error)) {
        player_audio_error(player, pool_error); return;
    }
    if (count != job->count || routed != job->routed || memcmp(decks, job->decks, count * sizeof(guint))) {
        if (ctx->notify) ctx->notify(ctx->notify_data, "Rotasyonun player dağılımı değişti; yeniden yükleyin.");
        return;
    }
    GError *failure = NULL;
    if (!radios_playout_ui_start_prepared_player_rotation(ctx, card, json_node_get_object(response),
            job->source, job->target, &failure)) player_audio_error(player, failure);
    else {
        player->running = FALSE;
        if (ctx->notify) ctx->notify(ctx->notify_data, job->routed ?
            "AutoDJ: rotasyon kayıtlı içerik/player dağılımıyla başlatılıyor." :
            "AutoDJ: yüklenen rotasyon seçili iki oynatıcıda başlatılıyor.");
    }
}
static gboolean player_playlist_autodj_ready(PlayerAudio *player)
{
    if (!player->queue || json_array_get_length(player->queue) < 2 || player->current < 0 ||
        (guint)player->current >= json_array_get_length(player->queue)) return FALSE;
    for (guint i = 0; i < json_array_get_length(player->queue); ++i) {
        JsonObject *input = json_array_get_object_element(player->queue, i);
        if (!g_uuid_string_is_valid(radios_json_string(input, "media_id", "")) ||
            !g_uuid_string_is_valid(radios_json_string(input, "version_id", "")) ||
            !g_uuid_string_is_valid(radios_json_string(input, "media_file_id", "")) ||
            strlen(radios_json_string(input, "media_sha256", "")) != 64) return FALSE;
    }
    return TRUE;
}
typedef struct {
    PlayerScopeCall call;
    RadioSAutomationBinding binding;
    RadioSAutoDJMode mode;
    guint64 assignment, input_serial, intent, generations[32];
    GWeakRef pool_cards[32];
    guint captured_cards;
} PlayerRotationScopeCall;
static void player_rotation_scope_free(gpointer data)
{
    PlayerRotationScopeCall *job = data;
    for (guint i = 0; i < job->captured_cards; ++i) g_weak_ref_clear(&job->pool_cards[i]);
    player_scope_call_free(job);
}
static gboolean player_rotation_scope_binding(PlayerAudio *player, JsonObject *prepared,
    RadioSAutoDJMode mode, RadioSAutomationBinding *binding, GError **error)
{
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = player->deck};
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(player->ctx->players);
    if (!radios_automation_scope_resolve(topology, &scope, binding, error)) return FALSE;
    if (mode != RADIOS_AUTODJ_TRANSITIONS || !player_rotation_routed(prepared)) return TRUE;
    guint decks[32], count = 0; gboolean routed = FALSE;
    guint target = binding->autodj_target >= 0 ? (guint)binding->autodj_target : G_MAXUINT;
    if (!radios_playout_prepared_rotation_pool(player->ctx, prepared, binding->source, target,
            decks, &count, &routed, error)) return FALSE;
    return !routed || radios_automation_scope_resolve_routed(topology, &scope, decks, count, binding, error);
}
static void player_rotation_scope_ready(RadioSViewContext *ctx, GtkWidget *owner,
    JsonNode *response, const GError *error, gpointer data)
{
    (void)owner; PlayerRotationScopeCall *job = data;
    GtkWidget *card = g_weak_ref_get(&job->call.card);
    PlayerAudio *player = card ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
    if (!player || player->destroyed) { g_clear_object(&card); return; }
    player->rotation_starting = FALSE;
    GError *failure = error ? g_error_copy(error) : NULL;
    if (!failure && (player->assignment_serial != job->assignment || player->load_serial != job->input_serial))
        g_set_error_literal(&failure, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Oynatıcı girdileri değişti; rotasyonu yeniden başlatın.");
    if (!failure) radios_automation_scope_binding_current(radios_player_workspace_topology(ctx->players), &job->binding, &failure);
    if (!failure && (!response || !JSON_NODE_HOLDS_OBJECT(response)))
        g_set_error_literal(&failure, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Rotasyon hazırlanamadı.");
    if (!failure) {
        RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx); RadioSAudioSnapshot audio = {0};
        if (engine) { radios_audio_engine_snapshot(engine, &audio); radios_audio_engine_unref(engine); }
        gboolean current = engine != NULL && radios_playout_ui_intent(ctx) == job->intent;
        for (guint i = 0; i < job->captured_cards; ++i) {
            guint deck = job->binding.decks[i];
            gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
            GtkWidget *now = g_object_get_data(G_OBJECT(ctx->parent), key); g_free(key);
            GtkWidget *original = g_weak_ref_get(&job->pool_cards[i]);
            current = current && now && now == original && !gtk_widget_in_destruction(now) &&
                audio.decks[deck].generation == job->generations[deck];
            g_clear_object(&original);
        }
        if (!current) g_set_error_literal(&failure, G_IO_ERROR, G_IO_ERROR_CANCELLED,
            "Hazırlık sırasında oynatıcı veya yayın değişti; mevcut ses korundu.");
    }
    if (!failure) {
        RadioSAutomationBinding current;
        player_rotation_scope_binding(player, json_node_get_object(response), job->mode, &current, &failure);
        if (!failure && (current.routed_pool != job->binding.routed_pool || current.count != job->binding.count ||
                memcmp(current.decks, job->binding.decks, current.count * sizeof(guint))))
            g_set_error_literal(&failure, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                "Rotasyonun oynatıcı dağılımı değişti; yeniden yükleyin.");
    }
    if (!failure) {
        PlayerScopeCall *call = g_new0(PlayerScopeCall, 1); g_weak_ref_init(&call->card, card);
        player->rotation_starting = TRUE;
        if (!radios_player_scope_execution_play_prepared(radios_player_scope_execution_get(ctx), &job->binding.scope,
                json_node_get_object(response), job->mode, player_scope_done, call, player_scope_call_free, &failure))
            player->rotation_starting = FALSE;
    }
    player_audio_error(player, failure); g_clear_object(&card);
}
static void player_rotation_scope_start(PlayerAudio *player, RadioSAutoDJMode mode)
{
    PlayerRotationScopeCall *job = g_new0(PlayerRotationScopeCall, 1); g_weak_ref_init(&job->call.card, player->card);
    GError *error = NULL;
    if (!player_rotation_scope_binding(player, json_node_get_object(player->rotation_prepared), mode, &job->binding, &error)) {
        player_rotation_scope_free(job); player_audio_error(player, error); return;
    }
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(player->ctx); RadioSAudioSnapshot audio = {0};
    if (!engine) { player_rotation_scope_free(job); return; }
    radios_audio_engine_snapshot(engine, &audio); radios_audio_engine_unref(engine);
    for (guint i = 0; i < job->binding.count; ++i) {
        guint deck = job->binding.decks[i]; gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
        GtkWidget *card = g_object_get_data(G_OBJECT(player->ctx->parent), key); g_free(key);
        if (!card || gtk_widget_in_destruction(card)) {
            player_rotation_scope_free(job);
            programme_notice(player->view, "Atanmış oynatıcı bulunamadı; rotasyonu yeniden yükleyin."); return;
        }
        g_weak_ref_init(&job->pool_cards[i], card); ++job->captured_cards;
        job->generations[deck] = audio.decks[deck].generation;
    }
    job->mode = mode; job->assignment = player->assignment_serial; job->input_serial = player->load_serial;
    job->intent = radios_playout_ui_intent(player->ctx);
    player->rotation_starting = TRUE;
    if (!radios_player_rotation_prepare_loaded(player->ctx, player->card, json_node_get_object(player->rotation_prepared),
            player->rotation_document, player->rotation_revision, player->rotation_id, player->rotation_client,
            player_rotation_scope_ready, job, player_rotation_scope_free)) {
        player->rotation_starting = FALSE; programme_notice(player->view, "Rotasyon bağlantısı hazır değil; yeniden yükleyin.");
    }
}
static void player_autodj_clicked(GtkButton *button, gpointer data)
{
    (void)button; PlayerAudio *player = data;
    const RadioSPlayerDefinition *definition = player_definition(player);
    if (definition && !player->weekly) {
        /* A start the card does not offer right now (programmatic click). */
        if (!gtk_widget_is_sensitive(player->autodj_start)) return;
        if (definition->autodj_mode == RADIOS_AUTODJ_OFF) { programme_notice(player->view, "Önce Yarı AutoDJ veya oynatıcılar arası AutoDJ seçin."); return; }
        if (player->rotation_starting || player->rotation_loading || player->playlist_pending || player_managed_context(player)) return;
        if (player_rotation_current(player)) player_rotation_scope_start(player, definition->autodj_mode);
        else player_scope_start(player, definition->autodj_mode);
        return;
    }
    if (player->weekly) { player_weekly_start(player); return; }
    if (player->rotation_starting || player->rotation_loading || player->playlist_pending || player_managed_context(player)) return;
    RadioSAudioPlayerOptions options = {-1, -1};
    radios_audio_ui_player_options(player->ctx, player->deck, &options);
    if (!player_rotation_current(player)) {
        if (!player_playlist_autodj_ready(player) || options.autodj_target < 0 || options.autodj_target >= 32 || options.autodj_target == (gint)player->deck) {
            if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "AutoDJ için en az iki yerel parça ve farklı bir hedef oynatıcı seçin.");
            return;
        }
        JsonArray *ordered = json_array_new(); guint count = json_array_get_length(player->queue);
        for (guint n = 0; n < count; ++n) json_array_add_object_element(ordered,
            json_object_ref(json_array_get_object_element(player->queue, ((guint)player->current + n) % count)));
        GError *error = NULL;
        gboolean accepted = radios_playout_ui_start_player_autodj(player->ctx, player->card, ordered,
            player->deck, (guint)options.autodj_target, &error);
        json_array_unref(ordered); player_audio_error(player, error);
        if (accepted) player->running = FALSE;
        return;
    }
    JsonObject *prepared = json_node_get_object(player->rotation_prepared);
    gboolean routed = player_rotation_routed(prepared);
    if (!routed && (options.autodj_target < 0 || options.autodj_target >= 32 || options.autodj_target == (gint)player->deck)) {
        if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "AutoDJ hedef oynatıcısını seçin.");
        return;
    }
    JsonNode *entries = json_object_get_member(prepared, "tracks");
    if (!entries || !JSON_NODE_HOLDS_ARRAY(entries) || json_array_get_length(json_node_get_array(entries)) < 2) {
        if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "Rotasyon kurallarına uygun en az iki yerel parça gerekli.");
        return;
    }
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(player->ctx); RadioSAudioSnapshot snapshot = {0};
    if (!engine) return;
    radios_audio_engine_snapshot(engine, &snapshot); radios_audio_engine_unref(engine);
    guint decks[32], count = 0; GError *pool_error = NULL;
    guint target = options.autodj_target >= 0 ? (guint)options.autodj_target : G_MAXUINT;
    if (!radios_playout_prepared_rotation_pool(player->ctx, prepared, player->deck, target,
            decks, &count, &routed, &pool_error)) {
        player_audio_error(player, pool_error); return;
    }
    PlayerRotationStart *job = g_new0(PlayerRotationStart, 1);
    job->assignment = player->assignment_serial; job->input_serial = player->load_serial;
    job->intent = radios_playout_ui_intent(player->ctx); job->current = player->current;
    job->source = player->deck; job->target = target; job->routed = routed;
    job->source_generation = snapshot.decks[job->source].generation;
    for (guint i = 0; i < count; ++i) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", decks[i]);
        GtkWidget *card = g_object_get_data(G_OBJECT(player->ctx->parent), key); g_free(key);
        if (!card || gtk_widget_in_destruction(card)) {
            player_rotation_start_free(job);
            if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "Atanmış player bulunamadı; rotasyondaki player dağılımını güncelleyin.");
            return;
        }
        job->decks[i] = decks[i]; job->generations[i] = snapshot.decks[decks[i]].generation;
        g_weak_ref_init(&job->cards[i], card); ++job->count;
    }
    player->rotation_starting = TRUE; gtk_widget_set_sensitive(player->autodj_start, FALSE);
    gboolean accepted = radios_player_rotation_prepare_loaded(player->ctx, player->card, prepared,
        player->rotation_document, player->rotation_revision, player->rotation_id, player->rotation_client,
        player_rotation_start_ready, job, player_rotation_start_free);
    if (!accepted) {
        player->rotation_starting = FALSE;
        if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "Rotasyon doğrulaması başlatılamadı; yeniden yükleyin.");
    }
}
static RadioSViewContext *player_managed_context(PlayerAudio *player)
{
    RadioSViewContext *scoped = radios_player_workspace_running_context(player->ctx->players, player->deck);
    if (scoped) return scoped;
    PlayerQueue *queue = &player->view->queues[player->deck + 1];
    if (queue->context.playout && radios_playout_ui_owns_deck(&queue->context, player->deck)) return &queue->context;
    return radios_playout_ui_owns_deck(player->ctx, player->deck) ? player->ctx : NULL;
}

static gboolean player_content_snapshot(PlayerAudio *player, RadioSPlayoutDeckContent *content)
{
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(player->ctx);
    if (!engine) return FALSE;
    gint source = -2;
    gboolean owned = radios_audio_engine_deck_runner_owner(engine, player->deck, &source);
    radios_audio_engine_unref(engine);
    if (!owned) return FALSE;
    if (source < 0) return radios_playout_ui_deck_content(player->ctx, player->deck, content);
    RadioSViewContext *scope = radios_player_workspace_source_context(player->ctx->players, (guint)source);
    if (scope && radios_playout_ui_deck_content(scope, player->deck, content)) return TRUE;
    PlayerQueue *queue = &player->view->queues[(guint)source + 1];
    return queue->initialized && radios_playout_ui_deck_content(&queue->context, player->deck, content);
}
static void player_content_render(PlayerAudio *player, const RadioSAudioUiSnapshot *audio,
    const RadioSPlayoutDeckContent *content, gboolean managed)
{
    const gchar *kind = managed ? content->kind : audio->kind;
    RadioSPlayoutContentState state = managed ? content->state :
        audio->state == RADIOS_AUDIO_UI_PLAYING && !audio->runner_paused && audio->main_enabled &&
            audio->main_path_ready && audio->position_ms > audio->cue_in_ms ? RADIOS_PLAYOUT_CONTENT_PLAYING :
        audio->state == RADIOS_AUDIO_UI_PAUSED || audio->runner_paused ? RADIOS_PLAYOUT_CONTENT_PAUSED :
        audio->state == RADIOS_AUDIO_UI_READY || audio->state == RADIOS_AUDIO_UI_LOADING ? RADIOS_PLAYOUT_CONTENT_READY :
        RADIOS_PLAYOUT_CONTENT_WAITING;
    GString *line = g_string_new(NULL);
    if (radios_programme_kind_valid(kind) && (managed || audio->media_id[0])) {
        const gchar *status = state == RADIOS_PLAYOUT_CONTENT_PLAYING ? "Çalıyor" :
            state == RADIOS_PLAYOUT_CONTENT_PAUSED ? "Duraklatıldı" :
            state == RADIOS_PLAYOUT_CONTENT_READY ? "Hazır" : "Bekleniyor";
        g_string_append_printf(line, "%s · %s%s", status, managed && content->auxiliary ? "Üzerine " : "",
            radios_programme_kind_label(kind));
    }
    if (managed && content->overlay_state != RADIOS_PLAYOUT_CONTENT_NONE && content->overlay_title[0]) {
        if (line->len) g_string_append(line, " · ");
        gchar *overlay = g_utf8_strdown(radios_programme_kind_label(content->overlay_kind), -1);
        if (content->overlay_state == RADIOS_PLAYOUT_CONTENT_PLAYING)
            g_string_append_printf(line, "Üzerine %s çalıyor: %s", overlay, content->overlay_title);
        else if (content->overlay_state == RADIOS_PLAYOUT_CONTENT_PAUSED)
            g_string_append_printf(line, "Üzerine %s duraklatıldı: %s", overlay, content->overlay_title);
        else if (content->overlay_state == RADIOS_PLAYOUT_CONTENT_READY)
            g_string_append_printf(line, "Planlanan üzerine %s: %s", overlay, content->overlay_title);
        else g_string_append_printf(line, "Üzerine %s bekleniyor: %s", overlay, content->overlay_title);
        g_string_append_printf(line, " (Oynatıcı %u)", content->overlay_player); g_free(overlay);
    }
    player_label_set_line(player->content_status, line->str);
    gtk_widget_set_tooltip_text(player->content_status, line->len ? line->str : NULL);
    g_string_free(line, TRUE);
}
static void player_metadata_render(PlayerAudio *player, RadioSAudioUiSnapshot *snapshot)
{
    RadioSPlayoutDeckMetadata programme = {0};
    RadioSPlayoutDeckContent content = {0};
    gboolean content_managed = player_content_snapshot(player, &content);
    PlayerQueue *queue = &player->view->queues[player->deck + 1];
    RadioSViewContext *scope_context = radios_player_workspace_running_context(player->ctx->players, player->deck);
    gboolean managed = (scope_context && radios_playout_ui_deck_metadata(scope_context, player->deck, &programme)) ||
        (queue->initialized && radios_playout_ui_deck_metadata(&queue->context, player->deck, &programme)) ||
        radios_playout_ui_deck_metadata(player->ctx, player->deck, &programme);
    const gchar *title = "", *artist = "", *album = "", *source = "", *context = "", *next_title = "", *next_artist = "";
    JsonObject *current = player_input(player, player->current), *next = NULL;
    if (managed) {
        if (player->lookup_pending) player_input_invalidate(player);
        player->queue_active = FALSE; player->loaded_current = -1;
        player->running = FALSE;
        title = programme.title; artist = programme.artist; album = programme.album;
        source = "Medya arşivi"; context = programme.source_name;
        next_title = programme.next_title; next_artist = programme.next_artist;
        snapshot->duration_ms = programme.duration_ms;
    } else if (content_managed && content.auxiliary && content.title[0]) {
        title = content.title; artist = content.artist; source = "Medya arşivi";
        context = "Üzerine içerik";
    } else {
        gboolean input_loaded = player_input_loaded(player, snapshot);
        if (snapshot->title[0] && (!player->queue_active || input_loaded)) {
            title = snapshot->title; artist = snapshot->artist; album = snapshot->album; source = "Medya arşivi";
            if (input_loaded) next = player_input(player, player->current + 1);
        } else if (current && player->queue_active) {
            title = radios_json_string(current, "title", ""); artist = radios_json_string(current, "artist", "");
            album = radios_json_string(current, "album", ""); source = "Medya arşivi";
            next = player_input(player, player->current + 1);
        }
    }
    if (next) { next_title = radios_json_string(next, "title", ""); next_artist = radios_json_string(next, "artist", ""); }
    player_content_render(player, snapshot, &content, content_managed);
    player_label_set_line(player->title, *title ? title : "—");
    gtk_widget_set_tooltip_text(player->title, *title ? title : NULL);
    const gchar *details[] = {artist, album, source, context}; GString *information = g_string_new(NULL);
    for (guint index = 0; index < G_N_ELEMENTS(details); ++index) if (*details[index]) {
        if (information->len) g_string_append(information, " · ");
        g_string_append(information, details[index]);
    }
    player_label_set_line(player->metadata, information->str);
    gtk_widget_set_tooltip_text(player->metadata, information->len ? information->str : NULL);
    /* Reserve the metadata line while empty so loading cannot resize the square card. */
    gtk_widget_set_visible(player->metadata, TRUE); g_string_free(information, TRUE);
    gchar *following = g_strdup_printf("Sıradaki  %s%s%s", *next_artist && *next_title ? next_artist : "",
        *next_artist && *next_title ? " — " : "", *next_title ? next_title : "—");
    player_label_set_line(player->next_label, following);
    gtk_widget_set_tooltip_text(player->next_label, *next_title ? following : NULL); g_free(following);
}
static void player_inputs_render(PlayerAudio *player);
static void player_input_remove(GtkButton *button, gpointer value)
{
    PlayerAudio *player = value; gint index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "player-input-index"));
    if (!player_input(player, index)) return;
    if (index == player->current && player->running) return;
    if (player_managed_context(player)) return;
    player_assignment_invalidate(player);
    player_rotation_clear(player, NULL);
    if (index == player->current) {
        player->loaded_current = -1;
        player_input_invalidate(player);
        radios_audio_ui_preview_stop(player->ctx, player->card);
        if (player->queue_active && player->ctx->audio) radios_audio_ui_stop(player->ctx, player->deck, NULL);
    }
    json_array_remove_element(player->queue, (guint)index);
    if (index < player->current) --player->current;
    if (index < player->loaded_current) --player->loaded_current;
    if (player->current >= (gint)json_array_get_length(player->queue)) player->current = (gint)json_array_get_length(player->queue) - 1;
    player_inputs_render(player);
}
static void player_inputs_render(PlayerAudio *player)
{
    gtk_container_foreach(GTK_CONTAINER(player->inputs_list), (GtkCallback)(void (*)(void))gtk_widget_destroy, NULL);
    guint count = player->queue ? json_array_get_length(player->queue) : 0;
    for (guint i = 0; i < count; ++i) {
        JsonObject *input = json_array_get_object_element(player->queue, i);
        GtkWidget *row = named_box(GTK_ORIENTATION_HORIZONTAL, 6, "lp-input");
        if ((gint)i == player->current) add_class(row, "is-current");
        GtkWidget *info = named_box(GTK_ORIENTATION_VERTICAL, 1, "lp-input-info");
        gchar *heading = g_strdup_printf("%u. %s", i + 1, radios_json_string(input, "title", ""));
        gchar *duration = player_clock(radios_json_int(input, "duration_ms", 0), FALSE);
        gchar *meta = g_strdup_printf("%s%s%s", radios_json_string(input, "artist", ""), *radios_json_string(input, "artist", "") ? " · " : "",
            radios_json_int(input, "duration_ms", 0) > 0 ? duration : "—");
        GtkWidget *name = radios_ui_label(heading, "lp-input-title"); gtk_label_set_line_wrap(GTK_LABEL(name), FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END); pack(info, name, FALSE);
        GtkWidget *detail = radios_ui_label(meta, "lp-input-meta"); gtk_label_set_line_wrap(GTK_LABEL(detail), FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END); pack(info, detail, FALSE);
        pack(row, info, TRUE);
        GtkWidget *remove = button(row, "×", "lp-input-remove", player->ctx, NULL);
        gtk_widget_set_tooltip_text(remove, "Girdiyi kaldır");
        g_object_set_data(G_OBJECT(remove), "player-input-index", GINT_TO_POINTER((gint)i));
        g_signal_connect(remove, "clicked", G_CALLBACK(player_input_remove), player);
        gtk_widget_set_sensitive(remove, !((gint)i == player->current && player->running));
        pack(player->inputs_list, row, FALSE);
        g_free(heading); g_free(duration); g_free(meta);
    }
    if (!count) pack(player->inputs_list, radios_ui_label("Girdi yok", "lp-input-empty"), FALSE);
    gtk_widget_show_all(player->inputs_list);
    gchar *label = g_strdup_printf("Girdiler (%u)", count); gtk_expander_set_label(GTK_EXPANDER(player->inputs), label); g_free(label);
    gtk_widget_set_sensitive(player->clear, count > 0 && !player->running);
    RadioSAudioUiSnapshot snapshot = {0}; radios_audio_ui_get_snapshot(player->ctx, player->deck, &snapshot);
    player_metadata_render(player, &snapshot);
}

/* ---- Card transport ----------------------------------------------------- */
static void player_input_start(PlayerAudio *player);
static void player_advance(PlayerAudio *player);
static gboolean player_copyright_allowed(PlayerAudio *player, JsonObject *metadata)
{
    RadioSAudioCopyrightWindow window = radios_copyright_policy_window(
        radios_playout_ui_copyright_policy_get(player->ctx),
        radios_json_string(metadata, "artist", ""), radios_json_string(metadata, "album", ""),
        radios_json_string(metadata, "title", ""));
    return radios_copyright_window_allowed(&window, g_get_real_time());
}
typedef struct { PlayerAudio *player; guint64 serial; } PlayerLoad;
static void player_media_ready(RadioSViewContext *ctx, GtkWidget *card, JsonNode *result, const GError *error, gpointer value)
{
    PlayerLoad *load = value; PlayerAudio *player = g_object_get_data(G_OBJECT(card), "radios-player-audio");
    if (!player || player != load->player || load->serial != player->load_serial) return;
    /* Playout ownership can change between polls and this callback. */
    if (player_managed_context(player)) {
        player_input_invalidate(player);
        player->running = player->queue_active = FALSE;
        player->loaded_current = -1; return;
    }
    player->lookup_pending = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        if (ctx->notify) ctx->notify(ctx->notify_data, error ? error->message : "Girdi medyası okunamadı.");
        player->running = FALSE; player_inputs_render(player); return;
    }
    JsonObject *detail = json_node_get_object(result), *media = catalog_object(detail, "media"), *version = catalog_object(detail, "version");
    JsonObject *input = player_input(player, player->current);
    if (!input || !g_str_equal(radios_json_string(media, "_id", ""), radios_json_string(input, "media_id", "")) ||
        !g_str_equal(radios_json_string(version, "_id", ""), radios_json_string(input, "version_id", ""))) {
        player->running = FALSE;
        if (ctx->notify) ctx->notify(ctx->notify_data, "Girdinin medya sürümü değişmiş; güncel listeyi yeniden yükleyin.");
        player_inputs_render(player); return;
    }
    JsonObject *copyright_policy = catalog_object(detail, "copyright_policy");
    if (copyright_policy) radios_playout_ui_copyright_policy_record(ctx, copyright_policy,
        radios_json_string(detail, "copyright_policy_document_id", ""),
        radios_json_int(detail, "copyright_policy_revision", 0));
    const gchar *copyright_metadata[] = {"title", "artist", "album"};
    for (guint i = 0; i < G_N_ELEMENTS(copyright_metadata); ++i)
        json_object_set_string_member(input, copyright_metadata[i], radios_json_string(media, copyright_metadata[i], ""));
    if (!player_copyright_allowed(player, media)) {
        if (ctx->notify) ctx->notify(ctx->notify_data, "Telif geçerlilik süresi dışındaki parça atlandı.");
        player_advance(player); return;
    }
    GError *failure = NULL;
    if (!radios_audio_ui_load_media(ctx, card, json_node_get_object(result), player->deck, player->running, &failure)) {
        player->running = FALSE; player_audio_error(player, failure); player_inputs_render(player); return;
    }
    player->loaded_current = player->current;
    g_object_set_data_full(G_OBJECT(card), "radios-deck-media-detail", json_node_copy(result), (GDestroyNotify)json_node_free);
}
/* Loads (and with running, plays) the current input. */
static void player_input_start(PlayerAudio *player)
{
    JsonObject *input = player_input(player, player->current);
    if (!input) { player->running = FALSE; player_inputs_render(player); return; }
    if (player->running && !player_output_ready(player)) {
        player->running = FALSE;
        if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "Önce bu rotasyon için bir çıkış kanalı seçin.");
        player_inputs_render(player); return;
    }
    player_input_invalidate(player); player->queue_active = TRUE; player->loaded_current = -1;
    /* Stop the previous source before looking up its replacement. Its
     * EOF/error is not the completion of this pending input. */
    if (player->ctx->audio) radios_audio_ui_stop(player->ctx, player->deck, NULL);
    player->lookup_pending = TRUE;
    PlayerLoad *load = g_new0(PlayerLoad, 1); load->player = player; load->serial = player->load_serial;
    if (!radios_media_library_get(player->ctx, player->card, radios_json_string(input, "media_id", ""), player_media_ready, load, g_free)) {
        player->running = FALSE; player->lookup_pending = FALSE;
        if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "Girdi medyası okunamadı.");
    }
    player_inputs_render(player);
}
static void player_advance(PlayerAudio *player)
{
    if (player->current + 1 >= (gint)(player->queue ? json_array_get_length(player->queue) : 0)) {
        player->running = FALSE; player_inputs_render(player); return;
    }
    ++player->current;
    player_input_start(player);
}
static void player_autodj_status_refresh(PlayerAudio *player)
{
    RadioSViewContext *managed = player_managed_context(player);
    const RadioSPlayerDefinition *definition = player_definition(player);
    gboolean event_active = radios_event_playout_busy(player->ctx, player->deck);
    const gchar *mode = event_active ? "Yarı AutoDJ · Event" :
        definition && definition->autodj_mode == RADIOS_AUTODJ_SEMI ? "Yarı AutoDJ" :
        definition && definition->autodj_mode == RADIOS_AUTODJ_OFF ? "Yayın sırası" : "AutoDJ";
    gchar *text = NULL; const gchar *detail = NULL;
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = player->deck};
    RadioSPlayerScopeExecutionSnapshot execution = {0};
    RadioSPlayoutSnapshot run = {0};
    RadioSPlayerScopeExecution *controller = radios_player_scope_execution_get(player->ctx);
    gboolean observed = definition && controller && radios_player_scope_execution_snapshot(controller, &scope, &execution);
    if (managed) {
        radios_playout_ui_get_snapshot(managed, &run);
        const gchar *state = "hazır";
        switch (run.state) {
        case RADIOS_PLAYOUT_PREPARING: state = "hazırlanıyor"; break;
        case RADIOS_PLAYOUT_SCHEDULED: state = "başlangıç saati bekleniyor"; break;
        case RADIOS_PLAYOUT_WAITING: state = "bekleniyor"; break;
        case RADIOS_PLAYOUT_MANUAL_WAIT: state = "Play bekleniyor"; break;
        case RADIOS_PLAYOUT_PLAYING: state = "çalıyor"; break;
        case RADIOS_PLAYOUT_OVERLAP: state = "geçiş yapılıyor"; break;
        case RADIOS_PLAYOUT_PAUSED: state = "duraklatıldı"; break;
        case RADIOS_PLAYOUT_FINISHED: state = "tamamlandı"; break;
        case RADIOS_PLAYOUT_ERROR: state = "hata"; detail = run.error; break;
        case RADIOS_PLAYOUT_IDLE: break;
        }
        text = run.next_deck >= 0 && run.next_deck != run.deck ?
            g_strdup_printf("%s · %s · %u → %u", mode, state, (guint)MAX(0, run.deck) + 1, (guint)run.next_deck + 1) :
            g_strdup_printf("%s · %s", mode, state);
    } else if (event_active || player->rotation_starting || (observed && execution.preparing)) text = g_strdup_printf("%s · hazırlanıyor", mode);
    else if (observed && execution.state == RADIOS_SCOPE_EXECUTION_ERROR) {
        text = g_strdup("Otomasyon · hata"); detail = execution.error;
    } else if (observed && execution.schema_active) {
        text = g_strdup(execution.state == RADIOS_SCOPE_EXECUTION_PAUSED ? "Otomasyon · duraklatıldı" : "Otomasyon · sıradaki program bekleniyor");
        detail = execution.event_name;
    } else if (definition && definition->autodj_mode == RADIOS_AUTODJ_SEMI) text = g_strdup("Yarı AutoDJ · tek oynatıcı");
    else if (definition && definition->autodj_mode == RADIOS_AUTODJ_TRANSITIONS) text = g_strdup("AutoDJ · oynatıcılar arası geçiş");
    else text = g_strdup("Manuel · AutoDJ kapalı");
    player_label_set_line(player->autodj_state, text);
    gtk_widget_set_tooltip_text(player->autodj_state, detail && *detail ? detail : text); g_free(text);
}
/* The BPM line's tooltip: the measured level and whether the player's mixer
 * channel applies the automatic correction right now; read only on hover. */
static gboolean player_loudness_tooltip(GtkWidget *widget, gint x, gint y, gboolean keyboard, GtkTooltip *tooltip, gpointer value)
{
    (void)widget; (void)x; (void)y; (void)keyboard;
    PlayerAudio *player = value; RadioSAudioUiSnapshot snapshot = {0};
    if (!radios_audio_ui_get_snapshot(player->ctx, player->deck, &snapshot)) return FALSE;
    gchar *text;
    if (!snapshot.media_id[0]) text = g_strdup("BPM ve ses yüksekliği parça yüklenince gösterilir.");
    else if (!snapshot.loudness_measured) text = g_strdup("Ses yüksekliği henüz ölçülmedi; mixer kanalında ölçüm bekleniyor.");
    else {
        RadioSAudioMixerConfig mixer = {0}; RadioSAudioDspMeters meters = {0}; gint channel = -1;
        if (radios_audio_ui_get_mixer(player->ctx, &mixer, &meters, NULL))
            for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i)
                if (mixer.channels[i].source_deck == (gint)player->deck && (channel < 0 || meters.channel_loudness_active[i])) {
                    channel = (gint)i; if (meters.channel_loudness_active[i]) break;
                }
        text = channel < 0 ? g_strdup_printf("Ses yüksekliği %.1f LUFS · oynatıcı bir mixer kanalına bağlı değil; otomatik eşitleme uygulanmıyor.", snapshot.loudness_lufs) :
            meters.channel_loudness_active[channel] ? g_strdup_printf("Ses yüksekliği %.1f LUFS · %d. mixer kanalında otomatik eşitleme %+.1f dB uygulanıyor.",
                snapshot.loudness_lufs, channel + 1, meters.channel_loudness_gain_db[channel]) :
            g_strdup_printf("Ses yüksekliği %.1f LUFS · %d. mixer kanalında otomatik eşitleme şu an uygulanmıyor.", snapshot.loudness_lufs, channel + 1);
    }
    gtk_tooltip_set_text(tooltip, text); g_free(text);
    return TRUE;
}
static gboolean player_audio_tick(gpointer value)
{
    PlayerAudio *player = value; RadioSAudioUiSnapshot snapshot = {0};
    if (!radios_audio_ui_get_snapshot(player->ctx, player->deck, &snapshot)) return G_SOURCE_CONTINUE;
    player_metadata_render(player, &snapshot);
    player_options_refresh(player);
    player_sync_targets_refresh(player, &snapshot);
    player_autodj_status_refresh(player);
    player_output_refresh(player);
    player_weekly_render(player);
    if (player->rotation_prepared && !player_rotation_current(player)) {
        player_assignment_invalidate(player);
        player_rotation_clear(player, "Veri bağlantısı değişti · rotasyonu yeniden yükleyin");
    }
    player_analysis_update(player, &snapshot);
    radios_player_waveform_update(player->waveform, &snapshot, FALSE);
    RadioSAudioPlayerOptions options = {-1, -1}; radios_audio_ui_player_options(player->ctx, player->deck, &options);
    gchar *tempo = snapshot.bpm > 0 ? g_strdup_printf("%.1f BPM · %+.1f%%", snapshot.effective_bpm, (snapshot.tempo_ratio - 1) * 100) : g_strdup("BPM —");
    gtk_label_set_text(GTK_LABEL(player->bpm), tempo); g_free(tempo);
    player->controls_updating = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(player->sync), snapshot.sync_master >= 0);
    player->controls_updating = FALSE;
    gboolean local = snapshot.media_id[0] != '\0';
    gboolean ready = snapshot.state == RADIOS_AUDIO_UI_READY || snapshot.state == RADIOS_AUDIO_UI_PLAYING || snapshot.state == RADIOS_AUDIO_UI_PAUSED;
    gtk_widget_set_sensitive(player->sync, local && ready && snapshot.state == RADIOS_AUDIO_UI_PLAYING && !snapshot.runner_paused &&
        snapshot.bpm > 0 && (snapshot.sync_master >= 0 || player_sync_peer_ready(player, player->sync_selected)));
    gtk_widget_set_sensitive(player->grid_edit, local);
    const RadioSPlayerDefinition *definition = player_definition(player);
    RadioSViewContext *managed = player_managed_context(player);
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = player->deck};
    RadioSPlayerScopeExecutionSnapshot execution = {0};
    RadioSPlayerScopeExecution *runner = radios_player_scope_execution_get(player->ctx);
    gboolean execution_active = definition && runner &&
        radios_player_scope_execution_snapshot(runner, &scope, &execution) &&
        (execution.schema_active || execution.preparing || execution.state == RADIOS_SCOPE_EXECUTION_WAITING);
    /* Independent controls belong to Kümesizler. Cluster membership can
     * change without recreating the card, so visibility follows topology. */
    gtk_widget_set_visible(player->independent_controls, !player->ctx->players || (definition && !*definition->cluster_id));
    if (player->autodj_target_row)
        gtk_widget_set_visible(player->autodj_target_row, definition && !*definition->cluster_id &&
            definition->autodj_mode != RADIOS_AUTODJ_OFF);
    if (player->autodj_mode) gtk_widget_set_sensitive(player->autodj_mode, definition && !managed && !execution_active && !player->rotation_starting && !player_weekly_armed(player));
    JsonNode *rotation_entries = player->rotation_prepared ? json_object_get_member(json_node_get_object(player->rotation_prepared), "tracks") : NULL;
    gboolean rotation_ready = player_rotation_current(player) && rotation_entries && JSON_NODE_HOLDS_ARRAY(rotation_entries) &&
        json_array_get_length(json_node_get_array(rotation_entries)) >= 2;
    gboolean routed_rotation = rotation_ready && player_rotation_routed(json_node_get_object(player->rotation_prepared));
    gboolean target_sensitive = !definition || definition->autodj_mode != RADIOS_AUTODJ_OFF;
    gboolean rotation_sensitive = !player->rotation_loading && !player->rotation_starting &&
        !player->playlist_pending && radios_data_connected(player->ctx) && !player_managed_context(player);
    gboolean start_sensitive;
    const gchar *start_label = player->rotation_starting ? "Hazırlanıyor…" : "Başlat";
    const gchar *start_tooltip = player->rotation_prepared ?
        (routed_rotation ? "Rotasyonun kayıtlı tür/player dağılımıyla çal; hedef kutusu bu dağılımı değiştirmez." :
         "Yüklenen rotasyonu hedef oynatıcıyla, rotasyonun geçiş ve seçim ayarlarıyla çal.") :
        "Yüklenen yerel playlisti seçili iki oynatıcıda genel yayın geçiş ayarlarıyla çal.";
    /* Apply only the final state so unchanged polls cannot animate controls
     * through intermediate disabled/enabled states. */
    if (player->weekly) {
        RadioSPlayerWeeklySnapshot weekly = {0}; player_weekly_snapshot(player, &weekly);
        start_sensitive = !weekly.armed && !player->rotation_loading && !player->playlist_pending &&
            radios_data_connected(player->ctx) && player_output_ready(player);
        start_label = weekly.armed ? "Program etkin" : "Programı başlat";
        target_sensitive = !weekly.armed;
        rotation_sensitive = !weekly.armed && !player->rotation_loading && !player_managed_context(player);
    } else if (definition) {
        const gchar *assignment = radios_player_topology_automation(radios_player_workspace_topology(player->ctx->players), player->deck);
        gboolean playable = *assignment || player_workspace_queue_ready(player) || (player->queue && json_array_get_length(player->queue) > 0) || player_rotation_current(player);
        start_sensitive = definition->autodj_mode != RADIOS_AUTODJ_OFF && playable &&
            !player->rotation_starting && !player->rotation_loading && !player->playlist_pending && !player_managed_context(player) &&
            radios_data_connected(player->ctx) && player_output_ready(player);
        start_tooltip = definition->autodj_mode == RADIOS_AUTODJ_SEMI ?
            "Yarı AutoDJ: playlisti veya atanmış şemayı yalnız bu oynatıcıda yürüt." :
            "AutoDJ: bu oynatıcıdan seçili AutoDJ oynatıcısına geçiş yap.";
    } else {
        start_sensitive = (rotation_ready || player_playlist_autodj_ready(player)) && (routed_rotation || options.autodj_target >= 0) &&
            !player->rotation_starting && !player->rotation_loading && !player->playlist_pending && player->ctx->playout && player_output_ready(player);
    }
    if (definition && !*definition->cluster_id && definition->autodj_mode == RADIOS_AUTODJ_TRANSITIONS &&
        radios_player_topology_members(radios_player_workspace_topology(player->ctx->players), "", NULL, 0) < 2) {
        start_sensitive = FALSE;
        start_tooltip = "Geçiş için ikinci bir kümesiz oynatıcı ekleyin veya Yarı AutoDJ seçin.";
    }
    if (execution_active || managed) target_sensitive = FALSE;
    if (managed) start_sensitive = FALSE;
    gtk_widget_set_sensitive(player->autodj_start, start_sensitive);
    gtk_widget_set_sensitive(player->autodj_target, target_sensitive);
    gtk_widget_set_sensitive(player->rotation_load, rotation_sensitive);
    player_button_set_label(player->autodj_start, start_label);
    gchar *shown_start_tooltip = gtk_widget_get_tooltip_text(player->autodj_start);
    if (g_strcmp0(shown_start_tooltip, start_tooltip)) gtk_widget_set_tooltip_text(player->autodj_start, start_tooltip);
    g_free(shown_start_tooltip);
    gtk_widget_set_sensitive(player->weekly_load, !player->rotation_loading && !player->rotation_starting && !player->playlist_pending &&
        radios_data_connected(player->ctx) && !player_managed_context(player) && !player_weekly_armed(player));
    if (!managed && player->queue_active && player_input(player, player->current) &&
        !player_copyright_allowed(player, player_input(player, player->current))) {
        gboolean paused = snapshot.state == RADIOS_AUDIO_UI_PAUSED;
        gboolean sounding = !player->lookup_pending && player_input_loaded(player, &snapshot) && (player->running || paused);
        if (sounding) {
            player_input_invalidate(player);
            radios_audio_ui_stop(player->ctx, player->deck, NULL);
            if (paused) player->running = FALSE;
            if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "Telif süresi dolan parça atlandı.");
            player_advance(player); return G_SOURCE_CONTINUE;
        }
    }
    static const gchar *const states[] = { "Boş", "Yükleniyor", "Hazır", "Oynuyor", "Duraklatıldı", "Bitti", "Hata" };
    if (player->running && !player->lookup_pending && player_input_loaded(player, &snapshot) &&
        (snapshot.state == RADIOS_AUDIO_UI_EOF || snapshot.state == RADIOS_AUDIO_UI_ERROR)) {
        if (snapshot.state == RADIOS_AUDIO_UI_ERROR && player->ctx->notify && snapshot.error[0]) player->ctx->notify(player->ctx->notify_data, snapshot.error);
        player_advance(player); return G_SOURCE_CONTINUE;
    }
    gboolean selected_loaded = !player->queue_active || player_input_loaded(player, &snapshot);
    const gchar *state_text = player->lookup_pending ? "Yükleniyor" :
        selected_loaded ? states[CLAMP((gint)snapshot.state, 0, 6)] : player_input(player, player->current) ? "Hazır" : "Boş";
    if (snapshot.state == RADIOS_AUDIO_UI_PLAYING && !snapshot.main_path_ready && snapshot.main_block_reason[0])
        state_text = "Çalıyor · MAIN sessiz";
    gtk_widget_set_tooltip_text(player->state, snapshot.error[0] ? snapshot.error : snapshot.main_block_reason[0] ? snapshot.main_block_reason : NULL);
    guint64 position = selected_loaded ? snapshot.position_ms : 0;
    guint64 duration = selected_loaded ? snapshot.duration_ms : (guint64)MAX(0, radios_json_int(player_input(player, player->current), "duration_ms", 0));
    gchar *time = player_clock((gint64)position, TRUE), *text = g_strdup_printf("Geçen  %s", time);
    gtk_label_set_text(GTK_LABEL(player->elapsed), text); g_free(time); g_free(text);
    time = player_clock((gint64)duration, FALSE); text = g_strdup_printf("Süre  %s", time);
    gtk_label_set_text(GTK_LABEL(player->duration), text); g_free(time); g_free(text);
    gtk_range_set_range(GTK_RANGE(player->seek), 0, MAX(1, duration));
    if (!player->seek_dragging) gtk_range_set_value(GTK_RANGE(player->seek), (gdouble)position);
    gboolean loaded = selected_loaded && !player->lookup_pending && snapshot.state != RADIOS_AUDIO_UI_EMPTY && snapshot.state != RADIOS_AUDIO_UI_LOADING && snapshot.state != RADIOS_AUDIO_UI_ERROR;
    gboolean queued = player->queue_active && player_input(player, player->current) != NULL;
    gboolean play_sensitive = player_output_ready(player) &&
        ((loaded && snapshot.output_ready && snapshot.state != RADIOS_AUDIO_UI_PLAYING) ||
         (queued && !player->lookup_pending && (!selected_loaded || (snapshot.state != RADIOS_AUDIO_UI_PLAYING && snapshot.state != RADIOS_AUDIO_UI_LOADING))));
    gboolean pause_sensitive = selected_loaded && snapshot.state == RADIOS_AUDIO_UI_PLAYING;
    gboolean stop_sensitive = loaded || player->lookup_pending || (selected_loaded && snapshot.state == RADIOS_AUDIO_UI_LOADING) || player->running;
    gboolean seek_sensitive = loaded;
    gboolean next_sensitive = player_input(player, player->current + 1) != NULL;
    gtk_widget_set_sensitive(player->cue, loaded);
    if (player->weekly) {
        RadioSPlayerWeeklySnapshot weekly = {0}; player_weekly_snapshot(player, &weekly);
        play_sensitive = !weekly.armed && player_output_ready(player) && radios_data_connected(player->ctx);
        stop_sensitive = weekly.armed;
        if (!managed) { pause_sensitive = FALSE; next_sensitive = FALSE; }
    }
    if (definition) {
        if (execution_active) {
            stop_sensitive = TRUE;
            if (!managed) {
                play_sensitive = execution.state == RADIOS_SCOPE_EXECUTION_PAUSED;
                state_text = execution.state == RADIOS_SCOPE_EXECUTION_PREPARING ? "Hazırlanıyor" : "Şema bekliyor";
            }
        } else if (!managed && !player_input(player, player->current) && player_workspace_queue_ready(player)) {
            play_sensitive = player_output_ready(player) && radios_data_connected(player->ctx);
        } else if (!managed && !player_input(player, player->current) &&
            *radios_player_topology_automation(radios_player_workspace_topology(player->ctx->players), player->deck)) {
            play_sensitive = player_output_ready(player) && radios_data_connected(player->ctx);
        }
    }
    if (managed) {
        RadioSPlayoutSnapshot run = {0}; radios_playout_ui_get_snapshot(managed, &run);
        play_sensitive = run.state == RADIOS_PLAYOUT_PAUSED && radios_audio_ui_ready(player->ctx, RADIOS_AUDIO_UI_MAIN);
        pause_sensitive = run.state == RADIOS_PLAYOUT_PLAYING || run.state == RADIOS_PLAYOUT_OVERLAP;
        stop_sensitive = TRUE;
        next_sensitive = run.index + 1 < run.total;
        seek_sensitive = run.deck == (gint)player->deck && snapshot.output_ready && loaded &&
            (run.state == RADIOS_PLAYOUT_PLAYING || run.state == RADIOS_PLAYOUT_PAUSED);
    }
    gtk_label_set_text(GTK_LABEL(player->state), state_text);
    gtk_widget_set_sensitive(player->play, play_sensitive);
    gtk_widget_set_sensitive(player->pause, pause_sensitive);
    gtk_widget_set_sensitive(player->stop, stop_sensitive || radios_event_playout_busy(player->ctx, player->deck));
    gtk_widget_set_sensitive(player->next, next_sensitive);
    gtk_widget_set_sensitive(player->seek, seek_sensitive);
    return G_SOURCE_CONTINUE;
}
static void player_audio_destroy(GtkWidget *widget, gpointer value)
{
    PlayerAudio *player = value; player->destroyed = TRUE;
    if (player->weekly) { radios_player_weekly_free(player->weekly); player->weekly = NULL; }
    g_object_set_data(G_OBJECT(player->card), "radios-player-weekly", NULL);
    player_input_invalidate(player);
    if (player->analysis_cancel) g_cancellable_cancel(player->analysis_cancel);
    ++player->analysis_serial;
    if (player->timer) { g_source_remove(player->timer); player->timer = 0; }
    gchar *key = g_strdup_printf("radios-deck-owner-%u", player->deck);
    if (g_object_get_data(G_OBJECT(player->ctx->parent), key) == widget)
        g_object_set_data(G_OBJECT(player->ctx->parent), key, NULL);
    g_free(key);
    if (!player->view->disposed) {
        RadioSViewContext *managed = player_managed_context(player);
        if (managed) radios_playout_ui_action(managed, RADIOS_PLAYOUT_STOP, NULL);
    }
    radios_audio_ui_preview_stop(player->ctx, widget);
    if (player->ctx->audio) {
        radios_audio_ui_stop(player->ctx, player->deck, NULL);
        radios_audio_ui_set_deck_main(player->ctx, player->deck, FALSE, NULL);
    }
}
static void player_audio_free(gpointer value)
{
    PlayerAudio *player = value;
    player_output_forget(player);
    if (player->weekly) radios_player_weekly_free(player->weekly);
    if (player->queue) json_array_unref(player->queue);
    g_clear_pointer(&player->rotation_prepared, json_node_free);
    if (player->rotation_client) radios_data_client_unref(player->rotation_client);
    g_free(player->rotation_name); g_free(player->output_signature); g_free(player->sync_signature);
    g_clear_object(&player->analysis_cancel); g_clear_pointer(&player->analysis_seen, g_hash_table_unref);
    if (player->analysis_client) radios_data_client_unref(player->analysis_client);
    g_free(player);
}
static void player_loaded(JsonArray *items, const gchar *name, gboolean append, gpointer value);
static void player_audio_action(GtkButton *widget, gpointer value)
{
    PlayerAudio *player = value; GError *error = NULL; GtkWidget *control = GTK_WIDGET(widget);
    if (control == player->stop) radios_event_playout_cancel(player->ctx, player->deck);
    if (player_definition(player) && (control == player->play || control == player->pause || control == player->stop || control == player->next)) {
        RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = player->deck};
        RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(player->ctx);
        RadioSPlayerScopeExecutionSnapshot state = {0};
        gboolean observed = execution && radios_player_scope_execution_snapshot(execution, &scope, &state);
        if (observed && state.state != RADIOS_SCOPE_EXECUTION_IDLE && state.state != RADIOS_SCOPE_EXECUTION_ERROR) {
            RadioSPlayoutAction action = control == player->stop ? RADIOS_PLAYOUT_STOP : control == player->pause ? RADIOS_PLAYOUT_PAUSE :
                control == player->next ? RADIOS_PLAYOUT_NEXT : RADIOS_PLAYOUT_RESUME;
            radios_player_scope_execution_action(execution, &scope, action, &error); player_audio_error(player, error); return;
        }
        if (control == player->play && !player->weekly && !player_input(player, player->current) &&
            (player_workspace_queue_ready(player) || *radios_player_topology_automation(radios_player_workspace_topology(player->ctx->players), player->deck))) {
            player_scope_start(player, RADIOS_AUTODJ_OFF); return;
        }
    }
    if (player->weekly && control == player->stop) { radios_player_weekly_stop(player->weekly); player_weekly_render(player); return; }
    if (player->weekly && control == player->play && !player_managed_context(player)) { player_weekly_start(player); return; }
    RadioSViewContext *managed = player_managed_context(player);
    if (managed && (control == player->play || control == player->pause || control == player->stop || control == player->next)) {
        RadioSPlayoutAction action = control == player->play ? RADIOS_PLAYOUT_RESUME : control == player->pause ? RADIOS_PLAYOUT_PAUSE :
            control == player->stop ? RADIOS_PLAYOUT_STOP : RADIOS_PLAYOUT_NEXT;
        radios_playout_ui_action(managed, action, &error); player_audio_error(player, error); return;
    }
    if (control == player->play) {
        if (!player_output_ready(player)) {
            if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "Önce bu rotasyon için bir çıkış kanalı seçin.");
            return;
        }
        RadioSAudioUiSnapshot snapshot = {0}; radios_audio_ui_get_snapshot(player->ctx, player->deck, &snapshot);
        gboolean loaded = snapshot.state == RADIOS_AUDIO_UI_READY || snapshot.state == RADIOS_AUDIO_UI_PAUSED;
        if (player->queue_active && player_input(player, player->current) &&
            !player_copyright_allowed(player, player_input(player, player->current))) {
            radios_audio_ui_stop(player->ctx, player->deck, NULL);
            player->running = TRUE; player_advance(player); return;
        }
        if (player->queue_active && player_input(player, player->current) && (!loaded || !player_input_loaded(player, &snapshot))) { player->running = TRUE; player_input_start(player); return; }
        player->running = player->queue_active && player_input_loaded(player, &snapshot);
        if (!radios_audio_ui_play(player->ctx, player->deck, &error)) { player->running = FALSE; player_inputs_render(player); }
    } else if (control == player->pause) {
        radios_audio_ui_pause(player->ctx, player->deck, &error);
    } else if (control == player->stop) {
        player->running = FALSE; player_input_invalidate(player);
        radios_audio_ui_stop(player->ctx, player->deck, &error);
        player_inputs_render(player);
    } else if (control == player->next) {
        if (!player_input(player, player->current + 1)) return;
        gboolean running = player->running; player_input_invalidate(player); ++player->current; player->queue_active = TRUE; player->loaded_current = -1;
        if (running) player_input_start(player);
        else {
            if (player->ctx->audio) radios_audio_ui_stop(player->ctx, player->deck, NULL);
            radios_audio_ui_preview_stop(player->ctx, player->card);
            player_inputs_render(player);
        }
    } else if (g_object_get_data(G_OBJECT(control), "player-loader")) {
        gchar *title = g_strdup_printf("%s · Playlist yükle", (const gchar *)g_object_get_data(G_OBJECT(player->card), "radios-player-name"));
        radios_player_loader_open(player->ctx, player->card, title, player_loaded, player); g_free(title);
        return;
    } else {
        /* Kütüphaneden yükle: one library media; the inputs pause. */
        player_assignment_invalidate(player);
        player_rotation_clear(player, NULL);
        player->running = FALSE; player->queue_active = FALSE; player->loaded_current = -1; player_input_invalidate(player);
        radios_media_library_open_for_deck(player->ctx, player->deck);
    }
    player_audio_error(player, error);
}
/* Inputs from the loader ("Yükle" replaces, "Sona ekle" appends). */
static void player_loaded(JsonArray *items, const gchar *name, gboolean append, gpointer value)
{
    PlayerAudio *player = value; guint added = 0;
    player_assignment_invalidate(player);
    player_rotation_clear(player, NULL);
    if (name) g_object_set_data_full(G_OBJECT(player->card), "radios-player-playlist-name", g_strdup(name), g_free);
    if (!append) {
        player->running = FALSE; player_input_invalidate(player); player->queue_active = TRUE; player->loaded_current = -1;
        radios_audio_ui_preview_stop(player->ctx, player->card);
        if (player->ctx->audio) radios_audio_ui_stop(player->ctx, player->deck, NULL);
        if (player->queue) json_array_unref(player->queue);
        player->queue = NULL; player->current = -1;
    }
    if (!player->queue) player->queue = json_array_new();
    for (guint i = 0; items && i < json_array_get_length(items) && json_array_get_length(player->queue) < 1000; ++i, ++added) {
        JsonObject *input = radios_programme_clone_object(json_array_get_object_element(items, i));
        if (name && *name) json_object_set_string_member(input, "playlist_name", name);
        json_array_add_object_element(player->queue, input);
    }
    if (player->current < 0 && json_array_get_length(player->queue)) {
        player->current = 0; player->queue_active = TRUE;
    }
    gtk_expander_set_expanded(GTK_EXPANDER(player->inputs), TRUE);
    player_inputs_render(player);
    gchar *message = g_strdup_printf("%s: %s%s%u girdi %s.", (const gchar *)g_object_get_data(G_OBJECT(player->card), "radios-player-name"),
        name && *name ? name : "", name && *name ? ", " : "", added, append ? "eklendi" : "yüklendi");
    if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, message);
    g_free(message);
}
static void player_clear(GtkButton *button, gpointer value)
{
    (void)button; PlayerAudio *player = value;
    if (player->running || player_managed_context(player)) return;
    player_assignment_invalidate(player);
    player_rotation_clear(player, NULL);
    player_input_invalidate(player);
    radios_audio_ui_preview_stop(player->ctx, player->card);
    if (player->queue_active && player->ctx->audio) radios_audio_ui_stop(player->ctx, player->deck, NULL);
    if (player->queue) json_array_unref(player->queue);
    player->queue = NULL; player->current = player->loaded_current = -1; player_inputs_render(player);
}
typedef struct { GWeakRef card; guint64 assignment; } PlayerRotationLoad;
static void player_rotation_load_destroyed(GtkWidget *dialog, gpointer value)
{
    (void)dialog; PlayerRotationLoad *job = value;
    GtkWidget *card = g_weak_ref_get(&job->card);
    PlayerAudio *player = card && !gtk_widget_in_destruction(card) ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
    if (player && !player->destroyed && player->assignment_serial == job->assignment) player->rotation_loading = FALSE;
    g_clear_object(&card); g_weak_ref_clear(&job->card); g_free(job);
}
static void player_rotation_loaded(JsonObject *prepared, const gchar *document, gint64 revision,
    const gchar *rotation, const gchar *name, RadioSDataClient *client, gpointer value)
{
    PlayerRotationLoad *job = value; GtkWidget *card = g_weak_ref_get(&job->card);
    PlayerAudio *player = card && !gtk_widget_in_destruction(card) ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
    if (!player || player->destroyed || player->assignment_serial != job->assignment) { g_clear_object(&card); return; }
    RadioSDataClient *current = radios_data_client_acquire(player->ctx);
    gboolean same = current == client && radios_data_connected(player->ctx) && !player_managed_context(player);
    if (current) radios_data_client_unref(current);
    JsonNode *entries_node = prepared ? json_object_get_member(prepared, "tracks") : NULL;
    JsonArray *entries = entries_node && JSON_NODE_HOLDS_ARRAY(entries_node) ? json_node_get_array(entries_node) : NULL;
    JsonArray *items = json_array_new();
    gboolean valid = same && entries && g_uuid_string_is_valid(document) && g_uuid_string_is_valid(rotation) && revision > 0;
    for (guint i = 0; valid && i < json_array_get_length(entries); ++i) {
        JsonNode *node = json_array_get_element(entries, i);
        JsonObject *track = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        JsonObject *detail = catalog_object(track, "detail");
        JsonObject *media = catalog_object(detail, "media"), *version = catalog_object(detail, "version");
        JsonObject *source = track && media && version ? radios_programme_clone_object(track) : NULL;
        if (source) {
            /* Prepared calendar rows keep the authenticated catalogue origin
             * under calendar_* keys. Preserve it in the card input so its
             * schema actions can resolve the same real playlist after load. */
            const gchar *playlist = radios_json_string(track, "calendar_playlist_id", "");
            if (g_uuid_string_is_valid(playlist))
                json_object_set_string_member(source, "playlist_id", playlist);
            const gchar *playlist_name = radios_json_string(track, "calendar_source_name", "");
            if (*playlist_name) json_object_set_string_member(source, "playlist_name", playlist_name);
            json_object_set_string_member(source, "media_id", radios_json_string(media, "_id", ""));
            json_object_set_string_member(source, "version_id", radios_json_string(version, "_id", ""));
            json_object_set_string_member(source, "media_file_id", radios_json_string(media, "media_file_id", ""));
            json_object_set_string_member(source, "media_sha256", radios_json_string(version, "sha256", ""));
        }
        JsonObject *input = source ? radios_player_input_new(source) : NULL;
        if (source) json_object_unref(source);
        if (!input || !g_uuid_string_is_valid(radios_json_string(input, "media_id", ""))) {
            if (input) json_object_unref(input);
            valid = FALSE; break;
        }
        json_array_add_object_element(items, input);
    }
    guint count = json_array_get_length(items);
    if (valid && count && count <= 200) {
        player_loaded(items, name, FALSE, player);
        player->rotation_prepared = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(player->rotation_prepared, radios_programme_clone_object(prepared));
        player->rotation_client = radios_data_client_ref(client);
        g_strlcpy(player->rotation_document, document, sizeof player->rotation_document);
        g_strlcpy(player->rotation_id, rotation, sizeof player->rotation_id);
        player->rotation_revision = revision; player->rotation_name = g_strdup(name);
        g_object_set_data_full(G_OBJECT(card), "radios-player-rotation-id", g_strdup(rotation), g_free);
        g_object_set_data_full(G_OBJECT(card), "radios-player-rotation-document", g_strdup(document), g_free);
        g_object_set_data_full(G_OBJECT(card), "radios-player-rotation-name", g_strdup(name), g_free);
        gchar *label = g_strdup_printf("%s · %u parça hazır", name, count);
        player_label_set_line(player->rotation_status, label);
        gtk_widget_set_tooltip_text(player->rotation_status, label); g_free(label);
        player_autodj_default(player);
        RadioSAudioPlayerOptions options = {-1, -1}; radios_audio_ui_player_options(player->ctx, player->deck, &options);
        if (options.autodj_target < 0 && !player->ctx->players) {
            for (guint deck = 0; deck < 32; ++deck) {
                if (deck == player->deck) continue;
                gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
                GtkWidget *other = g_object_get_data(G_OBJECT(player->ctx->parent), key); g_free(key);
                RadioSAudioUiSnapshot snapshot = {0};
                if (other && radios_audio_ui_get_snapshot(player->ctx, deck, &snapshot) &&
                    snapshot.state != RADIOS_AUDIO_UI_PLAYING && snapshot.state != RADIOS_AUDIO_UI_PAUSED &&
                    snapshot.state != RADIOS_AUDIO_UI_LOADING) {
                    options.autodj_target = (gint)deck;
                    radios_audio_ui_set_player_options(player->ctx, player->deck, &options, NULL); break;
                }
            }
        }
        player_options_refresh(player);
    } else if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "Rotasyon yüklenemedi; mevcut player sırası korundu.");
    json_array_unref(items); g_clear_object(&card);
}
static void player_rotation_load_clicked(GtkButton *button, gpointer value)
{
    (void)button; PlayerAudio *player = value;
    if (player->destroyed || player->rotation_loading || player->rotation_starting || player_managed_context(player)) return;
    player_assignment_invalidate(player);
    PlayerRotationLoad *job = g_new0(PlayerRotationLoad, 1);
    g_weak_ref_init(&job->card, player->card); job->assignment = player->assignment_serial;
    player->rotation_loading = TRUE; gtk_widget_set_sensitive(player->rotation_load, FALSE);
    const gchar *first = radios_json_string(player_input(player, player->loaded_current >= 0 ? player->loaded_current : player->current), "playlist_id", "");
    gboolean schema = g_object_get_data(G_OBJECT(button), "automation-schema-load") != NULL;
    GtkWidget *dialog = schema ? radios_player_rotation_open_schema(player->ctx, player->card, first, player_rotation_loaded, job) :
        radios_player_rotation_open(player->ctx, player->card, player_rotation_loaded, job);
    if (!dialog && schema && player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "Şema yüklemek için bu player'a kayıtlı bir playlist yükle.");
    if (dialog) g_signal_connect(dialog, "destroy", G_CALLBACK(player_rotation_load_destroyed), job);
    else player_rotation_load_destroyed(NULL, job);
}
static gboolean player_weekly_snapshot(PlayerAudio *player, RadioSPlayerWeeklySnapshot *snapshot)
{
    gboolean loaded = player->weekly && radios_player_weekly_get_snapshot(player->weekly, snapshot);
    if (!player->ctx->players) return loaded;
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = player->deck};
    RadioSPlayerScopeExecutionSnapshot run = {0};
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(player->ctx);
    if (execution && radios_player_scope_execution_snapshot(execution, &scope, &run) && run.schema_active) {
        snapshot->armed = TRUE; snapshot->repeat = run.repeat; snapshot->paused = run.state == RADIOS_SCOPE_EXECUTION_PAUSED;
        snapshot->state = run.state == RADIOS_SCOPE_EXECUTION_PREPARING ? RADIOS_PLAYER_WEEKLY_PREPARING :
            run.state == RADIOS_SCOPE_EXECUTION_WAITING ? RADIOS_PLAYER_WEEKLY_WAITING :
            run.state == RADIOS_SCOPE_EXECUTION_ERROR ? RADIOS_PLAYER_WEEKLY_ERROR : RADIOS_PLAYER_WEEKLY_PLAYING;
        snapshot->start_utc_us = run.starts_at_utc_us; snapshot->end_utc_us = run.ends_at_utc_us;
        gint64 now = g_get_real_time();
        snapshot->remaining_us = MAX((gint64)0, run.ends_at_utc_us - now);
        g_strlcpy(snapshot->current_event_name, run.event_name, sizeof snapshot->current_event_name);
        if (run.state == RADIOS_SCOPE_EXECUTION_WAITING && run.starts_at_utc_us > now) {
            snapshot->remaining_us = run.starts_at_utc_us - now;
            snapshot->next_start_utc_us = run.starts_at_utc_us;
            snapshot->current_event_name[0] = '\0';
            g_strlcpy(snapshot->next_event_name, run.event_name, sizeof snapshot->next_event_name);
        }
        g_strlcpy(snapshot->error, run.error, sizeof snapshot->error); return TRUE;
    }
    return loaded;
}
static gboolean player_weekly_armed(PlayerAudio *player)
{
    RadioSPlayerWeeklySnapshot snapshot = {0};
    return player_weekly_snapshot(player, &snapshot) && snapshot.armed;
}
static void player_weekly_render(PlayerAudio *player)
{
    RadioSPlayerWeeklySnapshot snapshot = {0};
    gboolean loaded = player_weekly_snapshot(player, &snapshot);
    gboolean previous = player->controls_updating; player->controls_updating = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(player->weekly_repeat), loaded && snapshot.repeat);
    gtk_widget_set_sensitive(player->weekly_repeat, loaded);
    /* Programme-only controls exist on the card only while a programme is loaded. */
    gtk_widget_set_visible(player->weekly_repeat, loaded); gtk_widget_set_visible(player->weekly_next, loaded);
    player->controls_updating = previous;
    if (!loaded) { player_label_set_line(player->weekly_next, "Sıradaki program —"); return; }
    gint64 seconds = MAX((gint64)0, snapshot.remaining_us / G_USEC_PER_SEC);
    gchar *clock = g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, seconds / 3600, seconds / 60 % 60, seconds % 60);
    const gchar *state[] = {"Boş", "Hazır", "Bekliyor", "Hazırlanıyor", "Oynuyor", "Tamamlandı", "Hata"};
    const gchar *event = snapshot.current_event_name[0] ? snapshot.current_event_name : snapshot.next_event_name;
    gchar *current = g_strdup_printf("%s%s · %s · Kalan %s", state[snapshot.state], snapshot.paused ? " · Duraklatıldı" : "",
        event[0] ? event : "Program bloğu yok", clock);
    gchar *next;
    if (snapshot.next_start_utc_us > 0) {
        GDateTime *date = g_date_time_new_from_unix_utc(snapshot.next_start_utc_us / G_USEC_PER_SEC);
        gchar *time = date ? g_date_time_format(date, "%Y-%m-%d %H:%M UTC") : g_strdup("");
        gint64 delta = MAX((gint64)0, (snapshot.next_start_utc_us - g_get_real_time()) / G_USEC_PER_SEC);
        next = g_strdup_printf("Sonraki: %s · %02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT " kaldı · %s",
            snapshot.next_event_name, delta / 3600, delta / 60 % 60, delta % 60, time);
        g_free(time); if (date) g_date_time_unref(date);
    } else next = g_strdup("Sonraki program yok");
    gchar *line = g_strdup_printf("%s\n%s", current, next);
    gtk_label_set_text(GTK_LABEL(player->weekly_next), line);
    gchar *tooltip = snapshot.error[0] ? g_strdup_printf("%s\n%s", line, snapshot.error) : g_strdup(line);
    gtk_widget_set_tooltip_text(player->weekly_next, tooltip);
    g_free(tooltip); g_free(line); g_free(current); g_free(next); g_free(clock);
}
static void player_weekly_repeat_changed(GtkToggleButton *toggle, gpointer value)
{
    PlayerAudio *player = value; if (player->controls_updating) return;
    gboolean repeat = gtk_toggle_button_get_active(toggle);
    if (player->weekly) radios_player_weekly_set_repeat(player->weekly, repeat);
    if (player->ctx->players && player_weekly_armed(player)) {
        RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = player->deck}; GError *error = NULL;
        radios_player_scope_execution_set_repeat(radios_player_scope_execution_get(player->ctx), &scope, repeat, &error);
        player_audio_error(player, error);
    }
    player_weekly_render(player);
}
static void player_weekly_loaded(JsonObject *record, RadioSDataClient *client, gpointer value)
{
    PlayerRotationLoad *job = value; GtkWidget *card = g_weak_ref_get(&job->card);
    PlayerAudio *player = card && !gtk_widget_in_destruction(card) ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
    if (!player || player->destroyed || player->assignment_serial != job->assignment || player_managed_context(player)) { g_clear_object(&card); return; }
    RadioSPlayerWeekly *weekly = radios_player_weekly_new(player->ctx, card, player->deck); GError *error = NULL;
    if (!radios_player_weekly_load(weekly, record, client, &error)) {
        radios_player_weekly_free(weekly); player_audio_error(player, error); g_clear_object(&card); return;
    }
    if (player->ctx->players) {
        RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = player->deck};
        if (!radios_player_workspace_assign(player->ctx->players, &scope, radios_json_string(record, "_id", ""), &error)) {
            radios_player_weekly_free(weekly); player_audio_error(player, error); g_clear_object(&card); return;
        }
    }
    JsonArray *empty = json_array_new(); player_loaded(empty, radios_json_string(record, "name", "Program"), FALSE, player); json_array_unref(empty);
    player->weekly = weekly; g_object_set_data(G_OBJECT(card), "radios-player-weekly", weekly);
    gchar *message = g_strdup_printf("%s · program hazır", radios_json_string(record, "name", "Program"));
    player_label_set_line(player->rotation_status, message); gtk_widget_set_tooltip_text(player->rotation_status, message); g_free(message);
    player_weekly_render(player); g_clear_object(&card);
}
static void player_weekly_load_clicked(GtkButton *button, gpointer value)
{
    (void)button; PlayerAudio *player = value;
    if (player->destroyed || player->rotation_loading || player->rotation_starting || player_managed_context(player) || player_weekly_armed(player)) return;
    player_assignment_invalidate(player);
    PlayerRotationLoad *job = g_new0(PlayerRotationLoad, 1); g_weak_ref_init(&job->card, player->card); job->assignment = player->assignment_serial;
    player->rotation_loading = TRUE;
    GtkWidget *dialog = radios_player_weekly_open(player->ctx, player->card, player_weekly_loaded, job);
    if (dialog) g_signal_connect(dialog, "destroy", G_CALLBACK(player_rotation_load_destroyed), job);
    else player_rotation_load_destroyed(NULL, job);
}
static void player_weekly_start(PlayerAudio *player)
{
    if (player_definition(player)) {
        RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = player->deck}; GError *error = NULL;
        RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(player->ctx);
        PlayerScopeCall *call = g_new0(PlayerScopeCall, 1); g_weak_ref_init(&call->card, player->card);
        player->rotation_starting = TRUE;
        if (radios_player_scope_execution_play_schema(execution, &scope, RADIOS_AUTODJ_OFF,
                player_scope_done, call, player_scope_call_free, &error)) {
            radios_player_scope_execution_set_repeat(execution, &scope, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(player->weekly_repeat)), &error);
        } else player->rotation_starting = FALSE;
        player_audio_error(player, error); player_weekly_render(player); return;
    }
    RadioSAudioPlayerOptions options = {-1, -1}; radios_audio_ui_player_options(player->ctx, player->deck, &options);
    GError *error = NULL;
    if (!player_output_ready(player)) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_PENDING, "Önce program için bir çıkış kanalı seçin.");
    else if (player->running || player->lookup_pending || player->playlist_pending || player_managed_context(player))
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_BUSY, "Bu rotasyon veya başka yayın sırası kullanımda.");
    else radios_player_weekly_start(player->weekly, options.autodj_target >= 0 ? (guint)options.autodj_target : G_MAXUINT, &error);
    player_audio_error(player, error); player_weekly_render(player);
}
typedef struct {
    GWeakRef card;
    RadioSDataClient *client;
    gchar *playlist_id, *entry_id;
    guint64 serial;
} PlayerPlaylistDrop;
static void player_playlist_drop_free(gpointer value)
{
    PlayerPlaylistDrop *drop = value;
    GtkWidget *card = g_weak_ref_get(&drop->card);
    PlayerAudio *player = card && !gtk_widget_in_destruction(card) ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
    if (player && !player->destroyed && player->assignment_serial == drop->serial) player->playlist_pending = FALSE;
    g_clear_object(&card); g_weak_ref_clear(&drop->card);
    radios_data_client_unref(drop->client); g_free(drop->playlist_id); g_free(drop->entry_id); g_free(drop);
}
static void player_playlist_dropped(RadioSViewContext *ctx, GtkWidget *card, JsonNode *result, const GError *error, gpointer value)
{
    PlayerPlaylistDrop *drop = value;
    PlayerAudio *player = g_object_get_data(G_OBJECT(card), "radios-player-audio");
    if (!player || player->destroyed || player->assignment_serial != drop->serial) return;
    player->playlist_pending = FALSE;
    RadioSDataClient *current = radios_data_client_acquire(ctx);
    gboolean same_target = current == drop->client && radios_data_connected(ctx) && !player_managed_context(player);
    if (current) radios_data_client_unref(current);
    if (!same_target) {
        if (ctx->notify) ctx->notify(ctx->notify_data, "Player veya veri bağlantısı değişti; playlisti yeniden bırakın.");
        return;
    }
    JsonObject *response = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    JsonNode *lists_node = response ? json_object_get_member(response, "playlists") : NULL;
    JsonArray *lists = lists_node && JSON_NODE_HOLDS_ARRAY(lists_node) ? json_node_get_array(lists_node) : NULL;
    JsonObject *playlist = NULL;
    for (guint i = 0; lists && i < json_array_get_length(lists); ++i) {
        JsonNode *node = json_array_get_element(lists, i);
        if (JSON_NODE_HOLDS_OBJECT(node) && !g_strcmp0(radios_json_string(json_node_get_object(node), "_id", ""), drop->playlist_id)) {
            playlist = json_node_get_object(node); break;
        }
    }
    JsonNode *tracks_node = playlist ? json_object_get_member(playlist, "tracks") : NULL;
    JsonArray *tracks = tracks_node && JSON_NODE_HOLDS_ARRAY(tracks_node) ? json_node_get_array(tracks_node) : NULL;
    JsonArray *items = json_array_new(); gboolean valid = !error && tracks;
    for (guint i = 0; valid && i < json_array_get_length(tracks); ++i) {
        JsonNode *node = json_array_get_element(tracks, i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) { valid = FALSE; break; }
        JsonObject *track = json_node_get_object(node);
        const gchar *entry_id = radios_json_string(track, "_id", radios_json_string(track, "id", ""));
        if (drop->entry_id && g_strcmp0(drop->entry_id, entry_id)) continue;
        /* Use the same saved snapshot and conversion as Playlist yükle. Local
         * media is verified by the existing deck loader before audio starts. */
        JsonObject *input = radios_player_input_new(track);
        if (!input) { valid = FALSE; break; }
        json_object_set_string_member(input, "playlist_id", drop->playlist_id);
        json_array_add_object_element(items, input);
    }
    guint count = json_array_get_length(items), queued = player->queue ? json_array_get_length(player->queue) : 0;
    guint available = queued <= 1000 ? 1000 - queued : 0;
    if (valid && count && count <= available && (!drop->entry_id || count == 1))
        player_loaded(items, radios_json_string(playlist, "name", "Playlist"), TRUE, player);
    else if (ctx->notify) ctx->notify(ctx->notify_data, error ? error->message : count > available ?
        "Player sırası en fazla 1000 parça içerebilir; mevcut sıra korundu." :
        "Playlist veya parçaları yüklenemedi; mevcut player sırası korundu.");
    json_array_unref(items);
}
static gboolean player_playlist_drop(PlayerAudio *player, JsonObject *record)
{
    const gchar *id = radios_json_string(record, "playlist_id", "");
    const gchar *entry = radios_json_string(record, "entry_id", "");
    if (!g_uuid_string_is_valid(id) || (*entry && !g_uuid_string_is_valid(entry))) return FALSE;
    if (player->playlist_pending) {
        if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "Playlist yükleniyor; tamamlandıktan sonra yeniden bırakın.");
        return FALSE;
    }
    RadioSDataClient *client = radios_data_client_acquire(player->ctx);
    if (!client || !radios_data_connected(player->ctx)) {
        if (client) radios_data_client_unref(client);
        if (player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "Playlist yüklemek için veritabanına bağlanın.");
        return FALSE;
    }
    PlayerPlaylistDrop *drop = g_new0(PlayerPlaylistDrop, 1);
    g_weak_ref_init(&drop->card, player->card); drop->client = client;
    drop->playlist_id = g_strdup(id); drop->entry_id = *entry ? g_strdup(entry) : NULL;
    drop->serial = ++player->assignment_serial; player->playlist_pending = TRUE;
    JsonNode *body = radios_data_body(player->ctx);
    gboolean started = radios_data_request(player->ctx, player->card, "POST", "/v1/workflows/desktop-playlists/list",
        body, player_playlist_dropped, drop, player_playlist_drop_free);
    json_node_free(body);
    if (started && player->ctx->notify) player->ctx->notify(player->ctx->notify_data, "Kaydedilmiş playlist player'a yükleniyor…");
    return started;
}

/* Catalogue rows, flow events and contents dropped on a card become inputs. */
static void player_drop(GtkWidget *card, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection,
    guint info, guint time, gpointer value)
{
    (void)x; (void)y; PlayerAudio *player = value; JsonArray *items = json_array_new(); const gchar *name = NULL;
    gchar *payload = radios_drag_payload(selection); JsonNode *node = NULL; gboolean requested = FALSE;
    if (payload && info == RADIOS_DRAG_INFO_PLAYLIST) {
        node = json_parse(payload);
        if (node && JSON_NODE_HOLDS_OBJECT(node)) requested = player_playlist_drop(player, json_node_get_object(node));
    } else if (payload && info == RADIOS_DRAG_INFO_ROW) {
        RadioSProgrammeTree *tree = programme_tree(player->view); guint block = 0; gint leaf = -1;
        if (radios_programme_tree_find(tree, payload, &block, &leaf)) {
            RadioSProgrammeBlock *owner = g_ptr_array_index(tree->blocks, block);
            for (guint i = leaf < 0 ? 0 : (guint)leaf; i < owner->leaves->len && (leaf < 0 || i == (guint)leaf); ++i) {
                JsonObject *input = radios_player_input_new(g_ptr_array_index(owner->leaves, i));
                if (input) json_array_add_object_element(items, input);
            }
        }
        radios_programme_tree_free(tree);
    } else if (payload && (node = json_parse(payload))) {
        if (JSON_NODE_HOLDS_ARRAY(node)) {
            for (guint i = 0; i < json_array_get_length(json_node_get_array(node)); ++i) {
                JsonNode *element = json_array_get_element(json_node_get_array(node), i);
                JsonObject *input = JSON_NODE_HOLDS_OBJECT(element) ? radios_player_input_new(json_node_get_object(element)) : NULL;
                if (input) json_array_add_object_element(items, input);
            }
        } else if (JSON_NODE_HOLDS_OBJECT(node)) {
            JsonObject *record = json_node_get_object(node), *programme = catalog_object(record, "programme");
            JsonObject *media = catalog_object(record, "media"), *version = catalog_object(record, "version");
            if (programme) {
                RadioSProgrammeTree *tree = radios_programme_tree_new(catalog_object(programme, "config"));
                for (guint i = 0; i < radios_programme_tree_leaf_count(tree); ++i) {
                    JsonObject *input = radios_player_input_new(radios_programme_tree_leaf_at(tree, i, NULL));
                    if (input) json_array_add_object_element(items, input);
                }
                radios_programme_tree_free(tree); name = radios_json_string(programme, "name", "");
            } else if (media && version) {
                JsonObject *source = json_object_new();
                json_object_set_string_member(source, "kind", radios_json_string(media, "kind", "music"));
                json_object_set_string_member(source, "media_id", radios_json_string(media, "_id", ""));
                json_object_set_string_member(source, "version_id", radios_json_string(version, "_id", ""));
                json_object_set_string_member(source, "media_file_id", radios_json_string(version, "media_file_id", ""));
                json_object_set_string_member(source, "media_sha256", radios_json_string(version, "sha256", ""));
                const gchar *keys[] = {"title", "artist", "album"};
                for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) json_object_set_string_member(source, keys[i], radios_json_string(media, keys[i], ""));
                json_object_set_int_member(source, "duration_ms", radios_json_int(version, "duration_ms", 0));
                JsonObject *input = radios_player_input_new(source); json_object_unref(source);
                if (input) json_array_add_object_element(items, input);
            }
        }
    }
    gboolean accepted = requested || json_array_get_length(items) > 0;
    if (json_array_get_length(items)) player_loaded(items, name, TRUE, player);
    remove_class(card, "is-drop-target");
    if (node) json_node_free(node);
    g_free(payload); json_array_unref(items);
    if (context) gtk_drag_finish(context, accepted, FALSE, time);
}
gboolean radios_broadcast_play_on_player(RadioSViewContext *ctx, JsonArray *items, const gchar *name, gchar **player, GError **error)
{
    BroadcastView *view = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context") : NULL;
    if (!view || view->disposed || !view->players) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "Canlı oynatıcılar hazır değil."); return FALSE;
    }
    JsonArray *inputs = json_array_new();
    for (guint i = 0; items && i < json_array_get_length(items); ++i) {
        JsonObject *input = radios_player_input_new(json_array_get_object_element(items, i));
        if (input) json_array_add_object_element(inputs, input);
    }
    if (!json_array_get_length(inputs)) {
        json_array_unref(inputs); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Listede çalınabilir parça yok."); return FALSE;
    }
    GList *children = gtk_container_get_children(GTK_CONTAINER(view->players)); PlayerAudio *target = NULL;
    for (GList *item = children; item && !target; item = item->next) {
        GtkWidget *card = GTK_IS_FLOW_BOX_CHILD(item->data) ? gtk_bin_get_child(GTK_BIN(item->data)) : item->data;
        if (GTK_IS_ASPECT_FRAME(card)) card = gtk_bin_get_child(GTK_BIN(card));
        PlayerAudio *candidate = card ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
        RadioSAudioUiSnapshot snapshot = {0};
        if (!candidate || candidate->running) continue;
        radios_audio_ui_get_snapshot(ctx, candidate->deck, &snapshot);
        if (snapshot.state != RADIOS_AUDIO_UI_PLAYING && snapshot.state != RADIOS_AUDIO_UI_LOADING) target = candidate;
    }
    g_list_free(children);
    if (!target) { json_array_unref(inputs); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Boş canlı oynatıcı yok."); return FALSE; }
    player_loaded(inputs, name, FALSE, target); json_array_unref(inputs);
    target->running = TRUE; player_input_start(target);
    if (player) *player = g_strdup(g_object_get_data(G_OBJECT(target->card), "radios-player-name"));
    return TRUE;
}
gboolean radios_broadcast_player_register_playlist(RadioSViewContext *ctx, JsonArray *items,
    const gchar *name, guint index, gint preferred_deck, guint *deck, GError **error)
{
    BroadcastView *view = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context") : NULL;
    if (!view || view->disposed || !view->players || !items || index >= json_array_get_length(items)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "Playlist/player altyapısı hazır değil."); return FALSE;
    }
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx); RadioSAudioSnapshot snapshot = {0};
    if (!engine) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "Player ses motoru hazır değil."); return FALSE; }
    radios_audio_engine_snapshot(engine, &snapshot); radios_audio_engine_unref(engine);
    PlayerAudio *target = NULL;
    for (guint i = 0; i < 32 && !target; ++i) {
        if (preferred_deck >= 0 && i != (guint)preferred_deck) continue;
        gchar *key = g_strdup_printf("radios-deck-owner-%u", i);
        GtkWidget *card = g_object_get_data(G_OBJECT(ctx->parent), key); g_free(key);
        PlayerAudio *candidate = card ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
        if (!candidate || candidate->destroyed || snapshot.decks[i].state == RADIOS_AUDIO_PLAYING ||
            snapshot.decks[i].state == RADIOS_AUDIO_PAUSED || snapshot.decks[i].state == RADIOS_AUDIO_LOADING) continue;
        if (preferred_deck < 0 && candidate->running) continue;
        target = candidate;
    }
    if (!target) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Playlist için uygun bağlı player bulunamadı."); return FALSE; }
    player_assignment_invalidate(target);
    player_rotation_clear(target, NULL);
    player_input_invalidate(target); target->queue_active = TRUE;
    target->loaded_current = -1; target->current = (gint)index;
    if (target->queue) json_array_unref(target->queue);
    target->queue = json_array_ref(items);
    g_object_set_data_full(G_OBJECT(target->card), "radios-player-playlist-name", g_strdup(name ? name : "Playlist"), g_free);
    player_inputs_render(target);
    if (deck) *deck = target->deck;
    return TRUE;
}
gboolean radios_broadcast_player_playlist(RadioSViewContext *ctx, GtkWidget *card,
    JsonArray **items, gchar **name, guint *index, guint *deck)
{
    PlayerAudio *player = card ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
    if (!player || player->destroyed || player->ctx->parent != ctx->parent || player->current < 0 ||
        !player->queue || (guint)player->current >= json_array_get_length(player->queue)) return FALSE;
    if (items) *items = json_array_ref(player->queue);
    if (name) *name = g_strdup(g_object_get_data(G_OBJECT(card), "radios-player-playlist-name"));
    if (index) *index = (guint)player->current;
    if (deck) *deck = player->deck;
    return TRUE;
}
static gboolean player_drag_motion(GtkWidget *card, GdkDragContext *context, gint x, gint y, guint time, gpointer value)
{ (void)context; (void)x; (void)y; (void)time; (void)value; add_class(card, "is-drop-target"); return FALSE; }
static void player_drag_leave(GtkWidget *card, GdkDragContext *context, guint time, gpointer value)
{ (void)context; (void)time; (void)value; remove_class(card, "is-drop-target"); }
static gboolean player_audio_seek(GtkRange *range, GtkScrollType scroll, gdouble position, gpointer value)
{
    (void)range; (void)scroll; PlayerAudio *player = value; GError *error = NULL;
    if (player->seek_dragging) return FALSE;
    RadioSAudioUiSnapshot snapshot = {0};
    if (radios_audio_ui_get_snapshot(player->ctx, player->deck, &snapshot))
        radios_audio_ui_seek_if_generation(player->ctx, player->deck, snapshot.generation, (guint64)MAX(0, llround(position)), &error);
    player_audio_error(player, error); return FALSE;
}
static gboolean player_seek_press(GtkWidget *widget, GdkEventButton *event, gpointer value)
{
    (void)widget; PlayerAudio *player = value; RadioSAudioUiSnapshot snapshot = {0};
    if (event->button == 1 && radios_audio_ui_get_snapshot(player->ctx, player->deck, &snapshot)) {
        player->seek_dragging = TRUE; player->seek_generation = snapshot.generation;
    }
    return FALSE;
}
static gboolean player_seek_release(GtkWidget *widget, GdkEventButton *event, gpointer value)
{
    PlayerAudio *player = value;
    if (event->button == 1 && player->seek_dragging) {
        player->seek_dragging = FALSE; GError *error = NULL;
        radios_audio_ui_seek_if_generation(player->ctx, player->deck, player->seek_generation,
            (guint64)MAX(0, llround(gtk_range_get_value(GTK_RANGE(widget)))), &error);
        player_audio_error(player, error);
    }
    return FALSE;
}
static gboolean player_seek_cancel(GtkWidget *widget, GdkEventGrabBroken *event, gpointer value)
{
    (void)widget; (void)event; ((PlayerAudio *)value)->seek_dragging = FALSE; return FALSE;
}
static void player_seek_unmapped(GtkWidget *widget, gpointer value)
{ (void)widget; ((PlayerAudio *)value)->seek_dragging = FALSE; }
static void player_audio_gain(GtkRange *range, gpointer value)
{
    PlayerAudio *player = value; GError *error = NULL;
    radios_audio_ui_set_deck_gain(player->ctx, player->deck, gtk_range_get_value(range), &error);
    player_audio_error(player, error);
}
static gboolean player_output_ready(PlayerAudio *player)
{
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(player->ctx);
    if (!engine) return FALSE;
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot); radios_audio_engine_unref(engine);
    if (!snapshot.decks[player->deck].main_routed) return FALSE;
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
        if (snapshot.mixer.channels[channel].source_deck == (gint)player->deck) return TRUE;
    return FALSE;
}
static void player_output_weak(GtkWidget **slot, GtkWidget *widget)
{
    if (*slot == widget) return;
    if (*slot) g_object_remove_weak_pointer(G_OBJECT(*slot), (gpointer *)slot);
    *slot = widget;
    if (widget) g_object_add_weak_pointer(G_OBJECT(widget), (gpointer *)slot);
}
static void player_output_forget(PlayerAudio *player)
{
    player_output_weak(&player->output_dj_view, NULL);
    player_output_weak(&player->output_channels_fallback, NULL);
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel) player_output_weak(&player->output_strips[channel], NULL);
}
/* The mixer channel box: found by name once and kept as weak pointers; a
 * failed search is retried at most once a second, because walking the whole
 * window tree on every 100 ms tick kept the main thread busy. */
static GtkWidget *player_output_channels(PlayerAudio *player)
{
    GtkWidget *dj = player->output_dj_view;
    gboolean retry = g_get_monotonic_time() >= player->output_lookup_after;
    if (!dj && retry) {
        dj = library_find(GTK_WIDGET(player->ctx->parent), "dj-task-view");
        /* The profile scroller uses the DJ page's name as well. */
        if (GTK_IS_BIN(dj) && !g_object_get_data(G_OBJECT(dj), "dj-channel-box")) {
            GtkWidget *content = gtk_bin_get_child(GTK_BIN(dj));
            GtkWidget *page = content ? library_find(content, "dj-task-view") : NULL;
            if (page) dj = page;
        }
        player_output_weak(&player->output_dj_view, dj);
    }
    /* The DJ page owns the same channel box while its mixer is in a separate window. */
    GtkWidget *channels = dj ? g_object_get_data(G_OBJECT(dj), "dj-channel-box") : NULL;
    if (!channels) {
        if (!player->output_channels_fallback && retry)
            player_output_weak(&player->output_channels_fallback, library_find(GTK_WIDGET(player->ctx->parent), "dj-mixer-channels"));
        channels = player->output_channels_fallback;
    }
    if (retry) player->output_lookup_after = g_get_monotonic_time() + G_USEC_PER_SEC;
    return channels;
}
static GtkWidget *player_output_strip(PlayerAudio *player, GtkWidget *channels, guint channel, gboolean retry)
{
    GtkWidget *strip = player->output_strips[channel];
    if (!strip && channels && retry) {
        gchar *name = g_strdup_printf("dj-channel-%u", channel + 1);
        player_output_weak(&player->output_strips[channel], library_find(channels, name)); g_free(name);
        strip = player->output_strips[channel];
    }
    return strip;
}
static void player_output_refresh(PlayerAudio *player)
{
    if (!player->output_channel || !player->main) return;
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(player->ctx);
    if (!engine) return;
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot); radios_audio_engine_unref(engine);
    GString *signature = g_string_new(NULL); gint selected = -1;
    gchar *labels[RADIOS_AUDIO_MIXER_CHANNELS] = {0};
    gboolean retry = g_get_monotonic_time() >= player->output_lookup_after;
    GtkWidget *channels = player_output_channels(player);
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel) {
        GtkWidget *strip = player_output_strip(player, channels, channel, retry);
        const gchar *title = strip ? g_object_get_data(G_OBJECT(strip), "dj-channel-name") : NULL;
        gint source = snapshot.mixer.channels[channel].source_deck;
        labels[channel] = title ? g_strdup_printf("%02u · %s", channel + 1, title) : g_strdup_printf("Kanal %u", channel + 1);
        g_string_append_printf(signature, "%d:%s;", source, labels[channel]);
        if (selected < 0 && snapshot.decks[player->deck].main_routed && source == (gint)player->deck) selected = (gint)channel;
    }
    g_string_append_printf(signature, "route:%d:selection:%d", snapshot.decks[player->deck].main_routed, selected);
    gboolean previous = player->controls_updating; player->controls_updating = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(player->main), snapshot.decks[player->deck].main_routed);
    if (g_strcmp0(player->output_signature, signature->str)) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(player->output_channel));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(player->output_channel), "-1", "Bağlantı yok");
        for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel) {
            gchar *id = g_strdup_printf("%u", channel);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(player->output_channel), id, labels[channel]); g_free(id);
        }
        gchar *id = g_strdup_printf("%d", selected); gtk_combo_box_set_active_id(GTK_COMBO_BOX(player->output_channel), id); g_free(id);
        g_free(player->output_signature); player->output_signature = g_strdup(signature->str);
    }
    player->controls_updating = previous;
    gtk_widget_set_sensitive(player->output_channel, radios_audio_ui_mixer_ready(player->ctx) && !player_managed_context(player) && !player->lookup_pending && !player_weekly_armed(player));
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel) g_free(labels[channel]);
    g_string_free(signature, TRUE);
}
static void player_output_changed(GtkComboBox *combo, gpointer value)
{
    PlayerAudio *player = value; if (player->controls_updating || player->destroyed) return;
    const gchar *id = gtk_combo_box_get_active_id(combo); if (!id) return;
    gint selected = (gint)g_ascii_strtoll(id, NULL, 10); GError *error = NULL;
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(player->ctx);
    if (!engine) { player_output_refresh(player); return; }
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(engine, &run);
    RadioSAudioRunSnapshot own; radios_audio_engine_player_runner_snapshot(engine, player->deck, &own);
    gboolean blocked = selected < -1 || selected >= (gint)RADIOS_AUDIO_MIXER_CHANNELS || run.owned[player->deck] || own.owned[player->deck] ||
        player->lookup_pending || player->rotation_starting || player->rotation_loading || player_weekly_armed(player);
    gint previous_source = selected >= 0 ? snapshot.mixer.channels[selected].source_deck : -1;
    if (previous_source == -2) blocked = TRUE; /* Keep the studio microphone strip. */
    if (previous_source >= 0 && previous_source != (gint)player->deck) {
        RadioSAudioRunSnapshot other; radios_audio_engine_player_runner_snapshot(engine, (guint)previous_source, &other);
        RadioSAudioState state = snapshot.decks[previous_source].state;
        blocked |= run.owned[previous_source] || other.owned[previous_source] || state == RADIOS_AUDIO_PLAYING || state == RADIOS_AUDIO_PAUSED || state == RADIOS_AUDIO_LOADING;
        gchar *key = g_strdup_printf("radios-deck-owner-%d", previous_source);
        GtkWidget *other_card = g_object_get_data(G_OBJECT(player->ctx->parent), key); g_free(key);
        PlayerAudio *other_player = other_card ? g_object_get_data(G_OBJECT(other_card), "radios-player-audio") : NULL;
        if (other_player) blocked |= other_player->running || other_player->lookup_pending || other_player->playlist_pending ||
            other_player->rotation_loading || other_player->rotation_starting || player_weekly_armed(other_player);
    }
    radios_audio_engine_unref(engine);
    if (blocked) {
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_BUSY, "Bu kanal veya rotasyon kullanımda; mevcut kaynak korunuyor.");
    } else {
        RadioSAudioMixerConfig config = snapshot.mixer;
        for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
            if (config.channels[channel].source_deck == (gint)player->deck) config.channels[channel].source_deck = -1;
        if (selected >= 0) config.channels[selected].source_deck = (gint)player->deck;
        if (radios_audio_ui_set_mixer(player->ctx, &config, &error) &&
            !radios_audio_ui_set_deck_main(player->ctx, player->deck, selected >= 0, &error))
            radios_audio_ui_set_mixer(player->ctx, &snapshot.mixer, NULL);
    }
    g_clear_pointer(&player->output_signature, g_free); player_output_refresh(player); player_audio_error(player, error);
}
static void player_audio_main(GtkToggleButton *widget, gpointer value)
{
    PlayerAudio *player = value; if (player->controls_updating) return; GError *error = NULL;
    radios_audio_ui_set_deck_main(player->ctx, player->deck, gtk_toggle_button_get_active(widget), &error);
    player_audio_error(player, error);
}
static void player_audio_cue(GtkButton *widget, gpointer value)
{
    (void)widget; PlayerAudio *player = value; RadioSAudioUiSnapshot snapshot = {0};
    if (!radios_audio_ui_get_snapshot(player->ctx, player->deck, &snapshot) || !snapshot.media_id[0]) return;
    JsonNode *detail = g_object_get_data(G_OBJECT(player->card), "radios-deck-media-detail");
    JsonObject *object = detail && JSON_NODE_HOLDS_OBJECT(detail) ? json_node_get_object(detail) : NULL;
    JsonObject *media = object ? json_object_get_object_member(object, "media") : NULL;
    if (!media || !g_str_equal(radios_json_string(media, "_id", ""), snapshot.media_id) ||
        !g_str_equal(radios_json_string(media, "version_id", ""), snapshot.version_id)) return;
    GError *error = NULL;
    radios_audio_ui_preview(player->ctx, player->card, object, 0, 0, &error);
    player_audio_error(player, error);
}

/* Card geometry follows its allocated width. Audio state never changes when
 * the operator resizes the window. */
static void player_combo_compact(GtkWidget *combo)
{
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(combo));
    for (GList *item = cells; item; item = item->next)
        if (GTK_IS_CELL_RENDERER_TEXT(item->data))
            g_object_set(item->data, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 10, NULL);
    g_list_free(cells);
    gtk_widget_set_hexpand(combo, TRUE);
}
static void player_card_size(GtkWidget *card, GtkAllocation *allocation, gpointer unused)
{
    (void)unused;
    GtkWidget *source = g_object_get_data(G_OBJECT(card), "live-card-source-row");
    gboolean compact = allocation->width < 360;
    if (source) gtk_orientable_set_orientation(GTK_ORIENTABLE(source),
        compact ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *autodj = g_object_get_data(G_OBJECT(card), "live-card-autodj-row");
    if (autodj) {
        GtkOrientation orientation = compact ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;
        if (gtk_orientable_get_orientation(GTK_ORIENTABLE(autodj)) != orientation)
            gtk_orientable_set_orientation(GTK_ORIENTABLE(autodj), orientation);
    }
    GtkStyleContext *style = gtk_widget_get_style_context(card);
    if (compact) gtk_style_context_add_class(style, "lp-compact-card");
    else gtk_style_context_remove_class(style, "lp-compact-card");
}
static gboolean live_players_layout(gpointer value)
{
    GtkWidget *players = value;
    g_object_set_data(G_OBJECT(players), "live-layout-idle", NULL);
    gint width = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(players), "live-layout-width"));
    guint columns = width >= 620 ? 2 : 1;
    /* Reflow source controls before FlowBox measures the next row. Otherwise
     * their wide natural width can keep the grid stuck in one column. */
    GList *children = gtk_container_get_children(GTK_CONTAINER(players));
    for (GList *item = children; item; item = item->next) {
        GtkWidget *card = gtk_bin_get_child(GTK_BIN(item->data));
        GtkAllocation proposed = {0, 0, width / (gint)columns - 9, 0};
        if (card) player_card_size(card, &proposed, NULL);
    }
    g_list_free(children);
    if (columns > gtk_flow_box_get_max_children_per_line(GTK_FLOW_BOX(players)))
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(players), columns);
    if (gtk_flow_box_get_min_children_per_line(GTK_FLOW_BOX(players)) != columns)
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(players), columns);
    if (gtk_flow_box_get_max_children_per_line(GTK_FLOW_BOX(players)) != columns)
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(players), columns);
    gtk_widget_queue_resize(players);
    GtkWidget *toplevel = gtk_widget_get_toplevel(players);
    if (GTK_IS_WINDOW(toplevel)) gtk_widget_queue_resize(toplevel);
    return G_SOURCE_REMOVE;
}
static gint live_available_width(GtkWidget *page, BroadcastView *view)
{
    /* GtkViewport allocates a wider virtual canvas when a child's old
     * minimum is still large. The scroller owns the actual visible width. */
    for (GtkWidget *parent = gtk_widget_get_parent(page); parent; parent = gtk_widget_get_parent(parent))
        if (GTK_IS_SCROLLED_WINDOW(parent)) return gtk_widget_get_allocated_width(parent);
    return gtk_widget_get_allocated_width(GTK_WIDGET(view->ctx->parent));
}
static void live_players_size(GtkWidget *players, GtkAllocation *allocation, gpointer value)
{
    GtkWidget *page = value;
    BroadcastView *view = g_object_get_data(G_OBJECT(page), "radios-broadcast-view");
    if (!view || view->disposed) return;
    gint width = allocation->width;
    if (gtk_orientable_get_orientation(GTK_ORIENTABLE(page)) == GTK_ORIENTATION_VERTICAL) {
        gint visible = live_available_width(page, view);
        if (visible > 100) width = MIN(width, visible);
    }
    gint previous = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(players), "live-layout-width"));
    if (previous == width) return;
    g_object_set_data(G_OBJECT(players), "live-layout-width", GINT_TO_POINTER(width));
    if (!g_object_get_data(G_OBJECT(players), "live-layout-idle")) {
        guint source = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, live_players_layout, g_object_ref(players), g_object_unref);
        g_object_set_data(G_OBJECT(players), "live-layout-idle", GUINT_TO_POINTER(source));
    }
}
static void live_page_size(GtkWidget *widget, GtkAllocation *allocation, gpointer value)
{
    (void)widget; (void)allocation;
    GtkWidget *page = value;
    BroadcastView *view = g_object_get_data(G_OBJECT(page), "radios-broadcast-view");
    if (!view || view->disposed) return;
    gint width = live_available_width(page, view);
    if (width < 100) width = gtk_widget_get_allocated_width(GTK_WIDGET(view->ctx->parent));
    gboolean stacked = width <= 900;
    GtkOrientation orientation = stacked ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;
    if (gtk_orientable_get_orientation(GTK_ORIENTABLE(page)) != orientation)
        gtk_orientable_set_orientation(GTK_ORIENTABLE(page), orientation);
    gtk_box_set_homogeneous(GTK_BOX(page), !stacked);
    GtkStyleContext *style = gtk_widget_get_style_context(page);
    if (stacked) gtk_style_context_add_class(style, "live-stacked");
    else gtk_style_context_remove_class(style, "live-stacked");
}

static void
player_add(GtkButton *button_widget, gpointer data)
{
    (void)button_widget; BroadcastView *view = data;
    if (view->ctx->players && radios_player_workspace_ready(view->ctx->players) && !view->loading) {
        RadioSPlayerTopology next = *radios_player_workspace_topology(view->ctx->players); GError *error = NULL;
        const gchar *cluster = "";
        if (view->live_scope_set) {
            if (view->live_scope.kind == RADIOS_AUTOMATION_SCOPE_CLUSTER) cluster = view->live_scope.cluster_id;
            else if (view->live_scope.kind == RADIOS_AUTOMATION_SCOPE_PLAYER) {
                const RadioSPlayerDefinition *selected = radios_player_topology_player_const(&next, view->live_scope.deck);
                if (selected) cluster = selected->cluster_id;
            }
        }
        RadioSAutomationScope folder = {.kind = *cluster ? RADIOS_AUTOMATION_SCOPE_CLUSTER : RADIOS_AUTOMATION_SCOPE_UNCLUSTERED};
        g_strlcpy(folder.cluster_id, cluster, sizeof folder.cluster_id);
        if (!radios_player_topology_add_player(&next, cluster, NULL, &error) ||
            !radios_player_workspace_replace(view->ctx->players, &next, &error)) {
            programme_notice(view, error ? error->message : "Oynatıcı eklenemedi."); g_clear_error(&error); return;
        }
        queue_scope_selected(&folder, view); players_reconcile(view); return;
    }
    guint deck = 0;
    for (; deck < 32; ++deck) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
        gboolean occupied = g_object_get_data(G_OBJECT(view->ctx->parent), key) != NULL;
        g_free(key); if (!occupied) break;
    }
    if (deck < 32) player_card_add(view, deck);
}
static GtkWidget *player_menu_item(GtkWidget *items, GtkWidget *menu,
    const gchar *label, const gchar *tooltip)
{
    GtkWidget *item = gtk_button_new_with_label(label);
    gtk_button_set_relief(GTK_BUTTON(item), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(item, tooltip);
    GtkWidget *caption = gtk_bin_get_child(GTK_BIN(item));
    if (GTK_IS_LABEL(caption)) gtk_label_set_xalign(GTK_LABEL(caption), 0);
    pack(items, item, FALSE);
    g_signal_connect_swapped(item, "clicked", G_CALLBACK(gtk_popover_popdown), menu);
    return item;
}
static void player_card_add(BroadcastView *view, guint deck)
{
    GList *cards = gtk_container_get_children(GTK_CONTAINER(view->players));
    guint count = g_list_length(cards); g_list_free(cards);
    if (count >= 32) { view->ctx->notify(view->ctx->notify_data, "En fazla 32 oynatıcı eklenebilir."); return; }
    GtkWidget *card = named_box(GTK_ORIENTATION_VERTICAL, 3, "lp-card");
    add_class(card, "lp-square-card");
    gtk_widget_set_size_request(card, 300, -1);
    gtk_widget_set_hexpand(card, TRUE);
    gtk_widget_set_halign(card, GTK_ALIGN_FILL);
    g_signal_connect(card, "size-allocate", G_CALLBACK(player_card_size), NULL);
    PlayerAudio *audio = g_new0(PlayerAudio, 1); audio->ctx = view->ctx; audio->view = view; audio->card = card; audio->deck = deck; audio->current = audio->loaded_current = -1;
    audio->sync_selected = -1;
    audio->analysis_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    audio->choices_signature = G_MAXUINT64;
    gchar *owner_key = g_strdup_printf("radios-deck-owner-%u", deck);
    g_object_set_data(G_OBJECT(view->ctx->parent), owner_key, card); g_free(owner_key);
    g_object_set_data_full(G_OBJECT(card), "radios-player-audio", audio, player_audio_free);
    g_signal_connect(card, "destroy", G_CALLBACK(player_audio_destroy), audio);
    GtkWidget *heading = named_box(GTK_ORIENTATION_HORIZONTAL, 6, "lp-card-heading");
    gchar *number = g_strdup_printf("%u", deck + 1);
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(view->ctx->players);
    const RadioSPlayerDefinition *definition = radios_player_topology_player_const(topology, deck);
    gchar *title = definition && *definition->name ? g_strdup(definition->name) : g_strdup_printf("Oynatıcı %u", deck + 1);
    g_object_set_data_full(G_OBJECT(card), "radios-player-id",
                          g_strdup(number), g_free);
    g_object_set_data_full(G_OBJECT(card), "radios-player-name",
                          g_strdup(title), g_free);
    player_group_metadata(card, topology, definition);
    pack(heading, radios_ui_label(number, "lp-card-number"), FALSE);
    GtkWidget *card_title = radios_ui_label(title, "heading");
    g_object_set_data(G_OBJECT(card), "radios-player-caption-widget", card_title);
    gtk_label_set_single_line_mode(GTK_LABEL(card_title), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(card_title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(card_title), 16);
    pack(heading, card_title, TRUE);
    audio->state = radios_ui_label("Beklemede", "lp-state"); pack(heading, audio->state, FALSE);
    gtk_label_set_single_line_mode(GTK_LABEL(audio->state), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(audio->state), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(audio->state), 14);
    GtkWidget *remove = button(heading, "×", "lp-remove", view->ctx, NULL);
    g_object_set_data(G_OBJECT(remove), "radios-player-card", card);
    g_signal_connect(remove, "clicked", G_CALLBACK(player_remove), view);
    pack(card, heading, FALSE);
    g_free(number);
    g_free(title);
    GtkWidget *current = named_box(GTK_ORIENTATION_VERTICAL, 3, "lp-current");
    GtkWidget *media_title = radios_ui_label("—", "lp-current-title"); audio->title = media_title;
    gchar *metadata_name = g_strdup_printf("deck-%u-title", deck); gtk_widget_set_name(media_title, metadata_name); g_free(metadata_name);
    gtk_label_set_line_wrap(GTK_LABEL(media_title), FALSE); gtk_label_set_ellipsize(GTK_LABEL(media_title), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(media_title), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(media_title), 32);
    g_object_set_data(G_OBJECT(card), "radios-deck-title-widget", media_title);
    pack(current, media_title, FALSE);
    audio->metadata = radios_ui_label("", "lp-current-meta");
    metadata_name = g_strdup_printf("deck-%u-metadata", deck); gtk_widget_set_name(audio->metadata, metadata_name); g_free(metadata_name);
    gtk_label_set_line_wrap(GTK_LABEL(audio->metadata), FALSE); gtk_label_set_ellipsize(GTK_LABEL(audio->metadata), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(audio->metadata), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(audio->metadata), 32);
    pack(current, audio->metadata, FALSE);
    audio->content_status = radios_ui_label("", "lp-current-meta");
    metadata_name = g_strdup_printf("deck-%u-content-status", deck);
    gtk_widget_set_name(audio->content_status, metadata_name); g_free(metadata_name);
    gtk_label_set_line_wrap(GTK_LABEL(audio->content_status), FALSE);
    gtk_label_set_ellipsize(GTK_LABEL(audio->content_status), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(audio->content_status), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(audio->content_status), 32);
    pack(current, audio->content_status, FALSE);
    audio->next_label = radios_ui_label("Sıradaki  —", "lp-next");
    metadata_name = g_strdup_printf("deck-%u-next-title", deck); gtk_widget_set_name(audio->next_label, metadata_name); g_free(metadata_name);
    gtk_label_set_line_wrap(GTK_LABEL(audio->next_label), FALSE); gtk_label_set_ellipsize(GTK_LABEL(audio->next_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(audio->next_label), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(audio->next_label), 32);
    pack(current, audio->next_label, FALSE);
    pack(card, current, FALSE);
    GtkWidget *source = named_box(GTK_ORIENTATION_HORIZONTAL, 5, "lp-source-row");
    g_object_set_data(G_OBJECT(card), "live-card-source-row", source);
    GtkWidget *loader = button(source, "Playlist yükle", "lp-add-input", view->ctx, NULL);
    gchar *control_name = g_strdup_printf("deck-%u-playlist", deck); gtk_widget_set_name(loader, control_name); g_free(control_name);
    g_object_set_data(G_OBJECT(loader), "player-loader", GINT_TO_POINTER(1));
    gtk_widget_set_sensitive(loader, view->ctx->data != NULL);
    g_signal_connect(loader, "clicked", G_CALLBACK(player_audio_action), audio);
    GtkWidget *add = button(source, "Kütüphaneden yükle", "lp-add-input", view->ctx, NULL);
    control_name = g_strdup_printf("deck-%u-load", deck); gtk_widget_set_name(add, control_name); g_free(control_name);
    gtk_widget_set_sensitive(add, view->ctx->audio != NULL);
    g_signal_connect(add, "clicked", G_CALLBACK(player_audio_action), audio);
    pack(card, source, FALSE);
    /* Normal form of a player's automation sources: one menu button replaces
     * the rotation, schema and programme buttons, and the line beside it says
     * what is loaded or, in plain Turkish, why it cannot play. Programme-only
     * controls appear while a programme is loaded; nothing holds their space. */
    GtkWidget *rotation_row = named_box(GTK_ORIENTATION_HORIZONTAL, 5, "lp-rotation-row");
    GtkWidget *automation = gtk_menu_button_new(); gtk_button_set_label(GTK_BUTTON(automation), "Otomasyon ▾"); add_class(automation, "lp-add-input");
    control_name = g_strdup_printf("deck-%u-automation", deck); gtk_widget_set_name(automation, control_name); g_free(control_name);
    gtk_widget_set_tooltip_text(automation, "Bu oynatıcıya rotasyon, otomasyon şeması ya da haftalık program yükle.");
    GtkWidget *menu = gtk_popover_new(automation), *items = radios_ui_box(GTK_ORIENTATION_VERTICAL, 2, NULL);
    gtk_container_set_border_width(GTK_CONTAINER(items), 6); gtk_container_add(GTK_CONTAINER(menu), items);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(automation), menu); g_object_set_data(G_OBJECT(card), "radios-player-automation-menu", menu);
    audio->rotation_load = player_menu_item(items, menu, "Rotasyon yükle…", "Kayıtlı bir rotasyonu bu oynatıcıda sırayla çalar.");
    control_name = g_strdup_printf("deck-%u-rotation", deck); gtk_widget_set_name(audio->rotation_load, control_name); g_free(control_name);
    GtkWidget *schema_load = player_menu_item(items, menu, "Otomasyon şeması yükle…",
        "Oynatıcıdaki playlist şemanın ilk playlisti olur (şemada yoksa başa eklenir); diğer playlistler şemadaki sırayla izler.");
    /* Kullanıcı isteği U2: a song the schema selects now loads here like "Kütüphaneden yükle". */
    GtkWidget *schema_tracks = player_menu_item(items, menu, "Şemadan yükle…", "Şemanın bu saat için seçtiği parçalardan birini bu oynatıcıya yükler.");
    gtk_widget_set_name(schema_tracks, "player-automation-schema-tracks");
    radios_schema_track_picker_bind(schema_tracks, view->ctx, card, deck);
    audio->weekly_load = player_menu_item(items, menu, "Haftalık program yükle…", "Kayıtlı bir haftalık programı saatine göre bu oynatıcıda çalar.");
    control_name = g_strdup_printf("deck-%u-weekly-programme", deck); gtk_widget_set_name(audio->weekly_load, control_name); g_free(control_name);
    gtk_box_pack_start(GTK_BOX(items), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
    GtkWidget *schema_create = player_menu_item(items, menu, "Bu playlistten şema oluştur…", "Oynatıcıdaki playlistle başlayan yeni bir otomasyon şeması açar.");
    gtk_widget_set_name(schema_create, "player-automation-schema-create"); gtk_widget_set_name(schema_load, "player-automation-schema-load");
    g_object_set_data(G_OBJECT(schema_load), "automation-schema-load", GINT_TO_POINTER(TRUE));
    g_signal_connect(schema_create, "clicked", G_CALLBACK(player_schema_create_clicked), audio);
    g_signal_connect(schema_load, "clicked", G_CALLBACK(player_rotation_load_clicked), audio);
    gtk_widget_set_sensitive(audio->rotation_load, view->ctx->data != NULL);
    g_signal_connect(audio->rotation_load, "clicked", G_CALLBACK(player_rotation_load_clicked), audio);
    g_signal_connect(audio->weekly_load, "clicked", G_CALLBACK(player_weekly_load_clicked), audio);
    gtk_widget_show_all(items); pack(rotation_row, automation, FALSE);
    audio->rotation_status = radios_ui_label("Otomasyon yüklenmedi", "lp-current-meta");
    control_name = g_strdup_printf("deck-%u-rotation-status", deck); gtk_widget_set_name(audio->rotation_status, control_name); g_free(control_name);
    gtk_label_set_single_line_mode(GTK_LABEL(audio->rotation_status), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(audio->rotation_status), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(audio->rotation_status), 24);
    pack(rotation_row, audio->rotation_status, TRUE);
    audio->weekly_repeat = gtk_check_button_new_with_label("Tekrarla");
    control_name = g_strdup_printf("deck-%u-weekly-repeat", deck); gtk_widget_set_name(audio->weekly_repeat, control_name); g_free(control_name);
    gtk_widget_set_tooltip_text(audio->weekly_repeat, "Programın bütün hafta aralığını tekrarlar. Kapatınca mevcut döngü tamamlanır.");
    gtk_widget_set_sensitive(audio->weekly_repeat, FALSE); gtk_widget_set_no_show_all(audio->weekly_repeat, TRUE);
    g_signal_connect(audio->weekly_repeat, "toggled", G_CALLBACK(player_weekly_repeat_changed), audio);
    pack(rotation_row, audio->weekly_repeat, FALSE); pack(card, rotation_row, FALSE);
    audio->weekly_next = radios_ui_label("Sıradaki program —", "lp-current-meta");
    control_name = g_strdup_printf("deck-%u-weekly-next", deck); gtk_widget_set_name(audio->weekly_next, control_name); g_free(control_name);
    gtk_label_set_line_wrap(GTK_LABEL(audio->weekly_next), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(audio->weekly_next), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(audio->weekly_next), 3);
    gtk_label_set_ellipsize(GTK_LABEL(audio->weekly_next), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(audio->weekly_next), 48);
    gtk_widget_set_no_show_all(audio->weekly_next, TRUE);
    pack(card, audio->weekly_next, FALSE);
    GtkWidget *transport = named_box(GTK_ORIENTATION_HORIZONTAL, 4, "lp-transport");
    const gchar *controls[] = {"▶", "Ⅱ", "■", "▸|"};
    const gchar *control_labels[] = {"Oynat", "Duraklat", "Durdur", "Sonraki"};
    for (guint i = 0; i < G_N_ELEMENTS(controls); ++i) {
        GtkWidget *control = button(transport, controls[i], NULL, view->ctx, NULL);
        gtk_widget_set_tooltip_text(control, control_labels[i]);
        atk_object_set_name(gtk_widget_get_accessible(control), control_labels[i]);
        gtk_widget_set_hexpand(control, TRUE);
        gtk_box_set_child_packing(GTK_BOX(transport), control, TRUE, TRUE, 0, GTK_PACK_START);
        const gchar *suffix[] = { "play", "pause", "stop", "next" };
        control_name = g_strdup_printf("deck-%u-%s", deck, suffix[i]); gtk_widget_set_name(control, control_name); g_free(control_name);
        if (i == 0) audio->play = control; else if (i == 1) audio->pause = control; else if (i == 2) audio->stop = control; else audio->next = control;
        g_signal_connect(control, "clicked", G_CALLBACK(player_audio_action), audio);
        gtk_widget_set_sensitive(control, FALSE);
    }
    pack(card, transport, FALSE);
    audio->waveform = radios_player_waveform_new(view->ctx, card, deck);
    pack(card, audio->waveform, FALSE);
    GtkWidget *times = named_box(GTK_ORIENTATION_HORIZONTAL, 8, "lp-times");
    audio->elapsed = radios_ui_label("Geçen  —:—", NULL); audio->duration = radios_ui_label("Süre  —:—", NULL);
    gtk_label_set_xalign(GTK_LABEL(audio->duration), 1);
    pack(times, audio->elapsed, TRUE); pack(times, audio->duration, TRUE);
    pack(card, times, FALSE);
    audio->seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
    gtk_scale_set_draw_value(GTK_SCALE(audio->seek), FALSE); gtk_widget_set_sensitive(audio->seek, FALSE);
    control_name = g_strdup_printf("deck-%u-seek", deck); gtk_widget_set_name(audio->seek, control_name); g_free(control_name);
    g_signal_connect(audio->seek, "change-value", G_CALLBACK(player_audio_seek), audio); pack(card, audio->seek, FALSE);
    g_signal_connect(audio->seek, "button-press-event", G_CALLBACK(player_seek_press), audio);
    g_signal_connect(audio->seek, "button-release-event", G_CALLBACK(player_seek_release), audio);
    g_signal_connect(audio->seek, "grab-broken-event", G_CALLBACK(player_seek_cancel), audio);
    g_signal_connect(audio->seek, "unmap", G_CALLBACK(player_seek_unmapped), audio);
    GtkWidget *level = named_box(GTK_ORIENTATION_HORIZONTAL, 7, "lp-level");
    pack(level, radios_ui_label("VOLUME", "lp-volume-line-control"), FALSE);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -60, 6, 0.5);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_range_set_value(GTK_RANGE(scale), 0);
    add_class(scale, "lp-volume-line");
    control_name = g_strdup_printf("deck-%u-volume", deck); gtk_widget_set_name(scale, control_name); g_free(control_name);
    GtkWidget *value = radios_ui_label("0.0 dB", "lp-volume-value");
    g_signal_connect(scale, "value-changed", G_CALLBACK(volume_change), value);
    audio->gain = scale;
    g_signal_connect(scale, "value-changed", G_CALLBACK(player_audio_gain), audio);
    gtk_widget_set_sensitive(scale, view->ctx->audio != NULL);
    pack(level, scale, TRUE);
    pack(level, value, FALSE);
    GtkWidget *cue = button(level, "CUE", "lp-cue", view->ctx, NULL);
    control_name = g_strdup_printf("deck-%u-cue", deck); gtk_widget_set_name(cue, control_name); g_free(control_name);
    audio->cue = cue; g_signal_connect(cue, "clicked", G_CALLBACK(player_audio_cue), audio);
    gtk_widget_set_sensitive(cue, FALSE);
    pack(card, level, FALSE);
    GtkWidget *sync_row = named_box(GTK_ORIENTATION_HORIZONTAL, 6, "lp-sync-row");
    audio->sync = gtk_toggle_button_new_with_label("SYNC");
    audio->sync_target = gtk_combo_box_text_new(); player_combo_compact(audio->sync_target);
    audio->bpm = radios_ui_label("BPM —", NULL);
    gtk_widget_set_has_tooltip(audio->bpm, TRUE);
    g_signal_connect(audio->bpm, "query-tooltip", G_CALLBACK(player_loudness_tooltip), audio);
    gtk_label_set_line_wrap(GTK_LABEL(audio->bpm), FALSE);
    gtk_label_set_single_line_mode(GTK_LABEL(audio->bpm), TRUE);
    audio->autodj_target = gtk_combo_box_text_new();
    player_combo_compact(audio->autodj_target);
    if (view->ctx->players) {
        audio->autodj_mode = gtk_combo_box_text_new();
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(audio->autodj_mode), "0", "Manuel");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(audio->autodj_mode), "1", "Yarı AutoDJ");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(audio->autodj_mode), "2", "AutoDJ geçiş");
        gtk_combo_box_set_active(GTK_COMBO_BOX(audio->autodj_mode), 0); player_combo_compact(audio->autodj_mode);
        control_name = g_strdup_printf("deck-%u-autodj-mode", deck); gtk_widget_set_name(audio->autodj_mode, control_name); g_free(control_name);
        g_signal_connect(audio->autodj_mode, "changed", G_CALLBACK(player_mode_changed), audio);
    }
    GtkWidget *autodj_row = named_box(GTK_ORIENTATION_HORIZONTAL, 6, "lp-autodj-row");
    GtkWidget *autodj_selection = named_box(GTK_ORIENTATION_HORIZONTAL, 6, "lp-autodj-selection");
    g_object_set_data(G_OBJECT(card), "live-card-autodj-row", autodj_row);
    pack(autodj_selection, radios_ui_label("AutoDJ", NULL), FALSE);
    audio->autodj_start = gtk_button_new_with_label("Başlat");
    add_class(audio->autodj_start, "lp-autodj-start");
    gtk_widget_set_hexpand(audio->autodj_start, FALSE);
    GtkWidget *start_caption = gtk_bin_get_child(GTK_BIN(audio->autodj_start));
    gtk_label_set_single_line_mode(GTK_LABEL(start_caption), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(start_caption), FALSE);
    GtkWidget *automation_controls[] = {audio->sync, audio->sync_target, audio->bpm, audio->autodj_target, audio->autodj_start};
    const gchar *automation_names[] = {"sync", "sync-target", "bpm", "autodj-target", "autodj-start"};
    for (guint index = 0; index < G_N_ELEMENTS(automation_controls); ++index) {
        control_name = g_strdup_printf("deck-%u-%s", deck, automation_names[index]);
        gtk_widget_set_name(automation_controls[index], control_name); g_free(control_name);
        gtk_widget_set_sensitive(automation_controls[index], view->ctx->audio != NULL);
    }
    gtk_widget_set_tooltip_text(audio->sync, "İki oynatıcı da Play durumundayken seçtiğiniz oynatıcıyla BPM ve beat fazını eşleştir (±%16). Ton korunur.");
    gtk_widget_set_tooltip_text(audio->sync_target, "Kümesizler içinde çalan bir oynatıcı seçin. SYNC yalnız iki oynatıcı da Play durumundayken kullanılabilir.");
    gtk_widget_set_tooltip_text(audio->autodj_target, "AutoDJ'nin bu oynatıcıdan geçeceği oynatıcı. Bir oynatıcı seçmek AutoDJ geçişini açar; Otomatik devam aynı gruptaki sıradaki oynatıcıyı kullanır. Playlist yükleme ve SYNC bu seçimden bağımsızdır.");
    gtk_widget_set_sensitive(audio->autodj_start, FALSE);
    gtk_widget_set_tooltip_text(audio->autodj_start, "AutoDJ kapalı. Bu oynatıcıya önce rotasyon yükleyin.");
    g_signal_connect(audio->sync, "toggled", G_CALLBACK(player_sync_changed), audio);
    g_signal_connect(audio->sync_target, "changed", G_CALLBACK(player_sync_target_changed), audio);
    g_signal_connect(audio->autodj_target, "changed", G_CALLBACK(player_options_changed), audio);
    g_signal_connect(audio->autodj_start, "clicked", G_CALLBACK(player_autodj_clicked), audio);
    audio->grid_edit = gtk_button_new_with_label("BPM ✎");
    control_name = g_strdup_printf("deck-%u-bpm-edit", deck); gtk_widget_set_name(audio->grid_edit, control_name); g_free(control_name);
    gtk_widget_set_tooltip_text(audio->grid_edit, "BPM'i elle gir, dalga biçimindeki ızgarada dörtlük başlarını seç.");
    gtk_widget_set_sensitive(audio->grid_edit, FALSE);
    g_signal_connect(audio->grid_edit, "clicked", G_CALLBACK(player_grid_clicked), audio);
    pack(sync_row, audio->bpm, TRUE); pack(sync_row, audio->grid_edit, FALSE);
    GtkWidget *sync_target_row = named_box(GTK_ORIENTATION_HORIZONTAL, 6, "lp-sync-target-row");
    pack(sync_target_row, audio->sync, FALSE); pack(sync_target_row, audio->sync_target, TRUE);
    if (audio->autodj_mode) pack(autodj_selection, audio->autodj_mode, TRUE); else pack(autodj_selection, audio->autodj_target, TRUE);
    pack(autodj_row, autodj_selection, TRUE);
    pack(autodj_row, audio->autodj_start, FALSE);
    pack(card, sync_row, FALSE);
    audio->independent_controls = named_box(GTK_ORIENTATION_VERTICAL, 5, "lp-independent-controls");
    gtk_widget_set_name(audio->independent_controls, "lp-independent-controls");
    pack(audio->independent_controls, sync_target_row, FALSE); pack(audio->independent_controls, autodj_row, FALSE);
    audio->autodj_state = radios_ui_label("Manuel · AutoDJ kapalı", "lp-current-meta");
    control_name = g_strdup_printf("deck-%u-autodj-state", deck); gtk_widget_set_name(audio->autodj_state, control_name); g_free(control_name);
    gtk_label_set_single_line_mode(GTK_LABEL(audio->autodj_state), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(audio->autodj_state), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(audio->autodj_state), 28);
    pack(audio->independent_controls, audio->autodj_state, FALSE);
    gtk_widget_show_all(audio->independent_controls);
    gtk_widget_set_no_show_all(audio->independent_controls, TRUE);
    pack(card, audio->independent_controls, FALSE);
    const RadioSPlayerDefinition *control_definition = player_definition(audio);
    gtk_widget_set_visible(audio->independent_controls, !view->ctx->players || (control_definition && !*control_definition->cluster_id));
    audio->main = gtk_check_button_new_with_label("Ana çıkışa gönder");
    g_object_set_data(G_OBJECT(card), "radios-deck-main-widget", audio->main);
    gtk_widget_set_sensitive(audio->main, view->ctx->audio != NULL);
    g_signal_connect(audio->main, "toggled", G_CALLBACK(player_audio_main), audio); pack(card, audio->main, FALSE);
    gtk_widget_set_no_show_all(audio->main, TRUE); gtk_widget_hide(audio->main);
    GtkWidget *output_row = named_box(GTK_ORIENTATION_HORIZONTAL, 6, "lp-output-row");
    pack(output_row, radios_ui_label("Çıkış kanalı", NULL), FALSE);
    audio->output_channel = gtk_combo_box_text_new();
    player_combo_compact(audio->output_channel);
    control_name = g_strdup_printf("deck-%u-output-channel", deck); gtk_widget_set_name(audio->output_channel, control_name); g_free(control_name);
    g_signal_connect(audio->output_channel, "changed", G_CALLBACK(player_output_changed), audio);
    pack(output_row, audio->output_channel, TRUE); pack(card, output_row, FALSE);
    if (audio->autodj_mode) {
        /* "AutoDJ oynatıcısı": the single place that picks the player AutoDJ
         * continues on. It stays under the output channel in every mode;
         * choosing a player switches the mode to AutoDJ geçiş. */
        GtkWidget *target_row = named_box(GTK_ORIENTATION_HORIZONTAL, 6, "lp-autodj-target-row");
        audio->autodj_target_row = target_row;
        GtkWidget *target_caption = radios_ui_label("AutoDJ oynatıcısı", NULL);
        gtk_label_set_line_wrap(GTK_LABEL(target_caption), FALSE); gtk_label_set_single_line_mode(GTK_LABEL(target_caption), TRUE);
        pack(target_row, target_caption, FALSE); pack(target_row, audio->autodj_target, TRUE); pack(card, target_row, FALSE);
        gtk_widget_show_all(target_row); gtk_widget_set_no_show_all(target_row, TRUE);
        /* Manuel keeps the square card's former geometry: the selector is
         * disabled there anyway and appears once Yarı AutoDJ or AutoDJ geçiş is chosen. */
        gtk_widget_set_visible(target_row, control_definition && !*control_definition->cluster_id &&
            control_definition->autodj_mode != RADIOS_AUTODJ_OFF);
    }
    player_output_refresh(audio);
    audio->inputs = gtk_expander_new("Girdiler (0)");
    add_class(audio->inputs, "lp-inputs");
    control_name = g_strdup_printf("deck-%u-inputs", deck); gtk_widget_set_name(audio->inputs, control_name); g_free(control_name);
    GtkWidget *input_body = named_box(GTK_ORIENTATION_VERTICAL, 5, "lp-input-body");
    audio->inputs_list = named_box(GTK_ORIENTATION_VERTICAL, 2, "lp-input-list"); pack(input_body, audio->inputs_list, FALSE);
    audio->clear = button(input_body, "Sırayı temizle", "lp-clear", view->ctx, NULL);
    control_name = g_strdup_printf("deck-%u-clear", deck); gtk_widget_set_name(audio->clear, control_name); g_free(control_name);
    g_signal_connect(audio->clear, "clicked", G_CALLBACK(player_clear), audio);
    gtk_container_add(GTK_CONTAINER(audio->inputs), input_body);
    pack(card, audio->inputs, FALSE);
    gtk_widget_set_no_show_all(audio->inputs, TRUE); gtk_widget_hide(audio->inputs);
    guint target_count = 0; const GtkTargetEntry *targets = radios_drag_targets(&target_count);
    gtk_drag_dest_set(card, GTK_DEST_DEFAULT_ALL, targets, target_count, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(card, "drag-data-received", G_CALLBACK(player_drop), audio);
    g_signal_connect(card, "drag-motion", G_CALLBACK(player_drag_motion), audio);
    g_signal_connect(card, "drag-leave", G_CALLBACK(player_drag_leave), audio);
    gtk_flow_box_insert(GTK_FLOW_BOX(view->players), card, -1);
    gtk_widget_show_all(card);
    player_inputs_render(audio);
    player_options_refresh(audio);
    if (view->ctx->audio) audio->timer = g_timeout_add(100, player_audio_tick, audio);
    player_count_update(view);
}

static void players_filter(BroadcastView *view)
{
    if (!view->players || !view->live_scope_set) return;
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(view->ctx->players);
    if (!topology) return;
    guint shown = 0;
    GList *children = gtk_container_get_children(GTK_CONTAINER(view->players));
    for (GList *item = children; item; item = item->next) {
        GtkWidget *card = gtk_bin_get_child(GTK_BIN(item->data));
        PlayerAudio *player = card ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
        const RadioSPlayerDefinition *definition = player ? radios_player_topology_player_const(topology, player->deck) : NULL;
        gboolean visible = definition && (view->live_scope.kind == RADIOS_AUTOMATION_SCOPE_PLAYER ? player->deck == view->live_scope.deck :
            view->live_scope.kind == RADIOS_AUTOMATION_SCOPE_CLUSTER ? g_str_equal(definition->cluster_id, view->live_scope.cluster_id) : !*definition->cluster_id);
        gtk_widget_set_no_show_all(item->data, TRUE);
        gtk_widget_set_visible(item->data, visible); shown += visible != FALSE;
    }
    g_list_free(children);
    if (view->players_empty) gtk_widget_set_visible(view->players_empty, shown == 0);
    const RadioSPlayerCluster *cluster = view->live_scope.kind == RADIOS_AUTOMATION_SCOPE_CLUSTER ?
        radios_player_topology_cluster_const(topology, view->live_scope.cluster_id) : NULL;
    const RadioSPlayerDefinition *player = view->live_scope.kind == RADIOS_AUTOMATION_SCOPE_PLAYER ?
        radios_player_topology_player_const(topology, view->live_scope.deck) : NULL;
    gchar *title = cluster ? g_strdup(cluster->name) : player && *player->name ? g_strdup(player->name) :
        player ? g_strdup_printf("Oynatıcı %u", view->live_scope.deck + 1) : g_strdup("Kümesizler");
    if (view->player_selection_title) gtk_label_set_text(GTK_LABEL(view->player_selection_title), title);
    g_free(title);
}
static gboolean players_reconcile(gpointer data)
{
    BroadcastView *view = data;
    if (view->disposed) return G_SOURCE_REMOVE;
    if (!view->ctx->players || !view->players || !radios_player_workspace_ready(view->ctx->players)) return G_SOURCE_CONTINUE;
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(view->ctx->players);
    JsonObject *object = radios_player_topology_to_json(topology);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, object);
    gchar *key = json_to_string(node, FALSE); json_node_free(node);
    if (!g_strcmp0(key, view->player_topology_key)) { g_free(key); return G_SOURCE_CONTINUE; }
    g_free(view->player_topology_key); view->player_topology_key = key; view->loading = TRUE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(view->players));
    for (GList *item = children; item; item = item->next) {
        GtkWidget *card = gtk_bin_get_child(GTK_BIN(item->data));
        PlayerAudio *player = card ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
        if (!player) continue;
        const RadioSPlayerDefinition *definition = radios_player_topology_player_const(topology, player->deck);
        if (!definition) {
            RadioSAudioUiSnapshot audio = {0}; radios_audio_ui_get_snapshot(view->ctx, player->deck, &audio);
            if (audio.state != RADIOS_AUDIO_UI_PLAYING && audio.state != RADIOS_AUDIO_UI_PAUSED && audio.state != RADIOS_AUDIO_UI_LOADING && !player_managed_context(player))
                gtk_widget_destroy(item->data);
            continue;
        }
        GtkWidget *caption = g_object_get_data(G_OBJECT(card), "radios-player-caption-widget");
        gchar *name = *definition->name ? g_strdup(definition->name) : g_strdup_printf("Oynatıcı %u", player->deck + 1);
        if (caption) gtk_label_set_text(GTK_LABEL(caption), name);
        g_object_set_data_full(G_OBJECT(card), "radios-player-name", name, g_free);
        player_group_metadata(card, topology, definition);
        gtk_widget_set_visible(player->independent_controls, !*definition->cluster_id);
        player->choices_signature = G_MAXUINT64;
    }
    g_list_free(children);
    for (guint deck = 0; deck < 32; ++deck) {
        if (!radios_player_topology_player_const(topology, deck)) continue;
        gchar *owner_key = g_strdup_printf("radios-deck-owner-%u", deck);
        gboolean present = g_object_get_data(G_OBJECT(view->ctx->parent), owner_key) != NULL; g_free(owner_key);
        if (!present) player_card_add(view, deck);
    }
    player_count_update(view); view->loading = FALSE;
    players_filter(view); return G_SOURCE_CONTINUE;
}
static void player_scope_selected(const RadioSAutomationScope *scope, gpointer data)
{
    BroadcastView *view = data;
    if (!scope || view->disposed) return;
    view->live_scope = *scope; view->live_scope_set = TRUE;
    players_reconcile(view);
    players_filter(view);
    if (!view->scope_mirroring && view->scope_panel) {
        view->scope_mirroring = TRUE;
        radios_automation_workspace_ui_select(view->scope_panel, scope);
        view->scope_mirroring = FALSE;
    }
}
static void queue_scope_selected(const RadioSAutomationScope *scope, gpointer data)
{
    BroadcastView *view = data;
    if (!scope || view->disposed || view->scope_mirroring) return;
    view->live_scope = *scope; view->live_scope_set = TRUE;
    view->scope_mirroring = TRUE;
    if (view->player_tree) { radios_player_tree_ui_refresh(view->player_tree); radios_player_tree_ui_select(view->player_tree, scope); }
    view->scope_mirroring = FALSE;
    players_filter(view);
    if (view->right_stack) gtk_stack_set_visible_child_name(GTK_STACK(view->right_stack), "players");
}
static void scope_edit_requested(const RadioSAutomationScope *scope, gpointer data)
{
    BroadcastView *view = data;
    if (!scope || scope->kind != RADIOS_AUTOMATION_SCOPE_PLAYER || view->disposed) return;
    queue_scope_selected(scope, view);
    gchar *id = g_strdup_printf("%u", scope->deck + 1);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->queue_player), id); g_free(id);
    PlayerQueue *queue = &view->queues[scope->deck + 1];
    if (queue->document) radios_data_document_reload(queue->document);
    if (view->queue_stack) gtk_stack_set_visible_child_name(GTK_STACK(view->queue_stack), "editor");
}
static GtkWidget *
players_view(BroadcastView *view)
{
    GtkWidget *rack = named_box(GTK_ORIENTATION_VERTICAL, 10, "live-player-rack");
    GdkScreen *screen = gtk_widget_get_screen(rack);
    if (!g_object_get_data(G_OBJECT(screen), "radios-square-player-theme")) {
        GtkCssProvider *theme = gtk_css_provider_new();
        gtk_css_provider_load_from_data(theme,
            ".lp-square-card button { min-height: 22px; min-width: 0; padding: 2px 5px; }"
            ".lp-square-card button.lp-autodj-start { min-width: 64px; min-height: 26px; padding: 3px 8px; }"
            ".lp-square-card scale { min-height: 16px; padding: 2px 0; }"
            ".lp-square-card scale slider { min-height: 12px; min-width: 12px; }"
            ".lp-square-card .lp-current { padding: 4px; }", -1, NULL);
        gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(theme), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        g_object_set_data_full(G_OBJECT(screen), "radios-square-player-theme", theme, g_object_unref);
    }
    add_class(rack, "broadcast-right-pane");
    gtk_widget_set_name(rack, "broadcast-players-pane");
    GtkWidget *toolbar = named_box(GTK_ORIENTATION_HORIZONTAL, 10, "live-player-toolbar");
    view->player_count = radios_ui_label("0 oynatıcı", NULL);
    pack(toolbar, view->player_count, TRUE);
    if (!view->ctx->players) {
        GtkWidget *add = button(toolbar, "+ Oynatıcı ekle", "ld-btn--primary", view->ctx, NULL);
        g_signal_connect(add, "clicked", G_CALLBACK(player_add), view);
    }
    pack(rack, toolbar, FALSE);
    if (view->ctx->players) {
        view->live_scope = (RadioSAutomationScope){.kind = RADIOS_AUTOMATION_SCOPE_UNCLUSTERED}; view->live_scope_set = TRUE;
        view->player_tree = radios_player_tree_ui_new(view->ctx, TRUE, FALSE, player_scope_selected, view);
        gtk_widget_set_name(view->player_tree, "live-player-definitions");
        GtkWidget *management = gtk_expander_new("Oynatıcıları düzenle · Kümeli / Kümesizler");
        gtk_widget_set_name(management, "live-player-management");
        gtk_expander_set_expanded(GTK_EXPANDER(management), FALSE);
        gtk_widget_set_tooltip_text(management, "Küme veya bağımsız oynatıcı ekleyin; mevcut oynatıcıları taşıyın ve düzenleyin.");
        gtk_container_add(GTK_CONTAINER(management), view->player_tree);
        pack(rack, management, FALSE);
        view->player_selection_title = radios_ui_label("Kümesizler", "heading");
        gtk_widget_set_name(view->player_selection_title, "live-player-selection-title");
        pack(rack, view->player_selection_title, FALSE);
        view->players_empty = radios_ui_label("Bu kümede oynatıcı yok.", "task-help");
        gtk_widget_set_name(view->players_empty, "live-player-selection-empty");
        gtk_label_set_line_wrap(GTK_LABEL(view->players_empty), TRUE);
        gtk_widget_set_no_show_all(view->players_empty, TRUE); pack(rack, view->players_empty, FALSE);
    }
    view->players = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(view->players), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(view->players), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(view->players), 2);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(view->players), TRUE);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(view->players), 9);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(view->players), 9);
    g_signal_connect_object(view->players, "size-allocate", G_CALLBACK(live_players_size), view->page, 0);
    add_class(view->players, "live-player-list");
    gtk_widget_set_valign(view->players, GTK_ALIGN_START);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_widget_set_size_request(scroll, -1, view->ctx->players ? 180 : 380);
    gtk_container_add(GTK_CONTAINER(scroll), view->players);
    pack(rack, scroll, TRUE);
    if (view->ctx->players) {
        view->topology_timer = g_timeout_add(300, players_reconcile, view);
        players_reconcile(view);
    } else for (guint i = 0; i < 6; ++i) player_add(NULL, view);
    return rack;
}

GtkWidget *
radios_broadcast_new(RadioSViewContext *ctx)
{
    BroadcastView *view = g_new0(BroadcastView, 1);
    view->event_overrides = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_object_unref);
    g_weak_ref_init(&view->local_event_dialog, NULL);
    view->ctx = ctx;
    view->lifecycle_context = *ctx;
    for (guint slot = 0; slot < G_N_ELEMENTS(view->queues); ++slot) {
        PlayerQueue *queue = &view->queues[slot]; queue->view = view; queue->slot = slot; queue->context = *ctx;
        if (slot) { queue->context.playout = NULL; queue->context.broadcast = NULL; queue->context.advertisement = NULL; }
    }
    view->queues[0].transition = 1; view->queues[0].overlap = 3;
    ProgrammeView view_defaults = PROGRAMME_VIEW_DEFAULTS; view->view_settings = view_defaults;
    view->policy = json_node_new(JSON_NODE_OBJECT); JsonObject *default_policy = json_object_new();
    json_object_set_int_member(default_policy, "schema", 1); json_object_set_int_member(default_policy, "mode", 1);
    json_object_set_int_member(default_policy, "overlap_ms", 3000); json_object_set_boolean_member(default_policy, "beat_sync", FALSE);
    json_node_take_object(view->policy, default_policy);
    GtkWidget *page = named_box(GTK_ORIENTATION_HORIZONTAL, 12, "broadcast-workspace");
    view->page = page;
    view->programme_config = json_node_new(JSON_NODE_OBJECT); json_node_take_object(view->programme_config, json_object_new());
    JsonObject *config = json_node_get_object(view->programme_config);
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d");
    json_object_set_string_member(config, "title", "Program taslağı"); json_object_set_string_member(config, "date", date);
    json_object_set_string_member(config, "note", ""); json_object_set_array_member(config, "events", json_array_new());
    g_free(date); g_date_time_unref(now);
    g_object_set_data(G_OBJECT(ctx->parent), "radios-broadcast-context", view);
    add_class(page, "live-desk");
    gtk_widget_set_name(page, "live-task-view");
    gtk_box_set_homogeneous(GTK_BOX(page), TRUE);
    g_object_set_data_full(G_OBJECT(page), "radios-broadcast-view", view, broadcast_free);
    g_signal_connect(page, "size-allocate", G_CALLBACK(live_page_size), page);
    g_signal_connect_object(ctx->parent, "size-allocate", G_CALLBACK(live_page_size), page, 0);
    g_signal_connect(page, "destroy", G_CALLBACK(programme_destroyed), view);
    GtkWidget *left = named_box(GTK_ORIENTATION_VERTICAL, 10, NULL);
    GtkWidget *editor = flow_queue(view);
    GtkWidget *navigation = named_box(GTK_ORIENTATION_HORIZONTAL, 8, "broadcast-sequence-navigation");
    GtkWidget *local_event = button(navigation, "Yerel Event oluştur", "ld-btn", ctx, NULL);
    gtk_widget_set_name(local_event, "programme-local-event");
    gtk_widget_set_tooltip_text(local_event, "Playlist ve şemalarla bağımsız bir Event hazırla");
    g_signal_connect(local_event, "clicked", G_CALLBACK(local_event_open), view);
    gtk_box_set_child_packing(GTK_BOX(navigation), local_event, FALSE, FALSE, 0, GTK_PACK_END);
    pack(left, navigation, FALSE);
    if (ctx->players) {
        GtkWidget *tabs = stack_tabs(left, &view->queue_stack, "broadcast-sequence-tabs");
        /* Share the tab row with the action while keeping page content below. */
        g_object_ref(tabs);
        gtk_container_remove(GTK_CONTAINER(left), tabs);
        pack(navigation, tabs, FALSE);
        g_object_unref(tabs);
        gtk_widget_set_name(view->queue_stack, "broadcast-sequence-stack");
        view->scope_panel = radios_automation_workspace_ui_new(ctx, queue_scope_selected, view);
        radios_automation_workspace_ui_set_edit_hook(view->scope_panel, scope_edit_requested, view);
        gtk_stack_add_titled(GTK_STACK(view->queue_stack), editor, "editor", "Liste düzenle");
        gtk_stack_add_titled(GTK_STACK(view->queue_stack), view->scope_panel, "scopes", "Otomasyon · Yayın sırası");
        gtk_stack_set_visible_child_name(GTK_STACK(view->queue_stack), "scopes");
    } else pack(left, editor, TRUE);
    pack(page, left, TRUE);
    GtkWidget *right = named_box(GTK_ORIENTATION_VERTICAL, 0, "broadcast-right");
    GtkWidget *stack;
    stack_tabs(right, &stack, "broadcast-right-tabs");
    view->right_stack = stack;
    gtk_stack_add_titled(GTK_STACK(stack), catalog_view(view), "library", "Parça · Albüm · Program");
    gtk_stack_add_titled(GTK_STACK(stack), players_view(view), "players", "Canlı oynatıcılar");
    pack(page, right, TRUE);
    view->programme_document = radios_data_document_new(ctx, page, "programme", "Mevcut program", programme_apply, &view->queues[0]);
    view->queues[0].document = view->programme_document;
    view->general_automation = radios_player_weekly_new_general(ctx, page);
    g_object_set_data(G_OBJECT(page), "radios-general-programme-driver", view->general_automation);
    if (view->general_automation) radios_player_weekly_general_set_queue(view->general_automation, general_queue_apply, view);
    view->policy_document = radios_data_document_new(ctx, page, "playout_policy", "main", policy_apply, view);
    if (view->policy_document) radios_data_document_set_status_callback(view->policy_document, policy_status_changed, view);
    view->queue_timer = g_timeout_add(100, queue_documents_tick, view);
    radios_data_register_refresh(ctx, page, broadcast_refresh);
    if (ctx->playout) view->playout_timer = g_timeout_add(100, programme_playout_tick, view);
    programme_view_load(view); programme_view_apply(view);
    return page;
}

static void channel_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; (void)owner; BroadcastView *view = data;
    if (view->ctx->players) { players_reconcile(view); return; }
    gint count = (gint)radios_json_int(config, "deck_count", 6); view->loading = TRUE;
    GList *cards = gtk_container_get_children(GTK_CONTAINER(view->players)); gint current = (gint)g_list_length(cards);
    while (current > count) { GtkWidget *last = g_list_last(cards)->data; cards = g_list_remove(cards, last); gtk_widget_destroy(last); --current; }
    g_list_free(cards);
    while (current++ < count) player_add(NULL, view);
    player_count_update(view); view->loading = FALSE;
}
static void broadcast_refresh(RadioSViewContext *ctx, GtkWidget *owner)
{
    BroadcastView *view = g_object_get_data(G_OBJECT(owner), "radios-broadcast-view");
    radios_player_weekly_general_refresh(view->general_automation);
    if (!ctx->players && !view->channel_document) view->channel_document = radios_data_document_new(ctx, owner, "channel", "Yayın oynatıcıları", channel_apply, view);
    catalog_search(NULL, view);
}

static void broadcast_header_copy(gchar *destination, const gchar *source, gsize capacity)
{
    g_strlcpy(destination, source ? source : "", capacity);
    const gchar *invalid_utf8 = NULL;
    if (!g_utf8_validate(destination, -1, &invalid_utf8)) destination[invalid_utf8 - destination] = '\0';
}
static gboolean broadcast_accepted_deck_metadata(RadioSViewContext *ctx, guint deck,
    RadioSPlayoutDeckMetadata *metadata)
{
    RadioSViewContext *scoped = radios_player_workspace_running_context(ctx->players, deck);
    if (scoped) ctx = scoped;
    RadioSAudioRunSnapshot before = {0}, after = {0};
    if (!radios_playout_ui_navigation_snapshot(ctx, &before) || !before.owned[deck]) return FALSE;
    /* TRUE with a cleared result is authoritative for a managed but retired
     * deck. Do not let the caller substitute an old filename/manual binding. */
    if (before.state != RADIOS_AUDIO_RUN_PLAYING && before.state != RADIOS_AUDIO_RUN_OVERLAP) return TRUE;
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx);
    if (!engine) return TRUE;
    RadioSAudioSnapshot *audio = g_new0(RadioSAudioSnapshot, 1);
    radios_audio_engine_snapshot(engine, audio);
    const gchar *entry = before.deck_current == (gint)deck ? before.entry_id :
        before.state == RADIOS_AUDIO_RUN_OVERLAP && before.deck_next == (gint)deck ? before.next_entry_id : NULL;
    gboolean current = entry && entry[0] && !g_strcmp0(entry, audio->decks[deck].binding.entry_id);
    gboolean okay = current && radios_playout_ui_deck_metadata(ctx, deck, metadata) &&
        radios_playout_ui_navigation_snapshot(ctx, &after) &&
        after.epoch == before.epoch && after.state == before.state &&
        after.index == before.index && after.next_index == before.next_index &&
        after.deck_current == before.deck_current && after.deck_next == before.deck_next && after.owned[deck] &&
        !g_strcmp0(after.entry_id, before.entry_id) && !g_strcmp0(after.next_entry_id, before.next_entry_id);
    if (!okay) memset(metadata, 0, sizeof *metadata);
    g_free(audio); radios_audio_engine_unref(engine);
    return TRUE;
}
gboolean radios_broadcast_player_deck_metadata(RadioSViewContext *ctx, guint deck,
    RadioSPlayoutDeckMetadata *metadata)
{
    if (!metadata) return FALSE;
    memset(metadata, 0, sizeof *metadata);
    if (!ctx || deck >= RADIOS_AUDIO_NORMAL_DECKS) return FALSE;
    BroadcastView *view = ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context") : NULL;
    if (view && !view->disposed) {
        PlayerQueue *queue = &view->queues[deck + 1];
        if (queue->initialized && broadcast_accepted_deck_metadata(&queue->context, deck, metadata)) return TRUE;
    }
    return broadcast_accepted_deck_metadata(ctx, deck, metadata);
}
static void broadcast_header_programme(RadioSBroadcastHeaderSnapshot *out, const RadioSPlayoutHeaderSnapshot *in)
{
    out->programme_active = in->programme_active;
    broadcast_header_copy(out->programme_title, in->programme_title, sizeof out->programme_title);
    broadcast_header_copy(out->playlist_title, in->playlist_title, sizeof out->playlist_title);
    out->programme_end_utc_us = in->programme_end_utc_us; out->playlist_end_utc_us = in->playlist_end_utc_us;
    out->programme_remaining_us = in->programme_remaining_us; out->playlist_remaining_us = in->playlist_remaining_us;
    out->track_remaining_us = in->track_remaining_us;
    out->advert_timing_known = in->advert_timing_known; out->advert_remaining_us = in->advert_remaining_us;
}
static void broadcast_header_card_playlist(PlayerAudio *player, RadioSBroadcastHeaderSnapshot *out)
{
    if (!player || player->destroyed || !player->queue_active || !player->queue || player->current < 0) return;
    JsonObject *current = player_input(player, player->current);
    const gchar *name = radios_json_string(current, "playlist_name", "");
    if (!*name) name = g_object_get_data(G_OBJECT(player->card), "radios-player-playlist-name");
    if (!name || !*name) return;
    const gchar *playlist_id = radios_json_string(current, "playlist_id", "");
    broadcast_header_copy(out->playlist_title, name, sizeof out->playlist_title);
    if (!out->duration_ms || out->position_ms > out->duration_ms) return;
    gdouble ratio = isfinite(out->tempo_ratio) && out->tempo_ratio > 0 ? out->tempo_ratio : 1;
    gint64 remaining = (gint64)ceil((out->duration_ms - out->position_ms) * 1000.0 / ratio);
    out->track_remaining_us = remaining;
    for (guint i = (guint)player->current + 1; i < json_array_get_length(player->queue); ++i) {
        JsonObject *input = json_array_get_object_element(player->queue, i);
        if ((*playlist_id && g_strcmp0(playlist_id, radios_json_string(input, "playlist_id", ""))) ||
            (!*playlist_id && *radios_json_string(input, "playlist_name", "") &&
             g_strcmp0(name, radios_json_string(input, "playlist_name", "")))) break;
        gint64 duration = radios_json_int(input, "duration_ms", 0);
        if (duration <= 0 || duration > 86400000) return;
        remaining += duration * 1000;
    }
    out->playlist_remaining_us = remaining;
    if (!out->paused) out->playlist_end_utc_us = g_get_real_time() + remaining;
}
static gboolean broadcast_header_scope_schedule(RadioSViewContext *ctx, RadioSViewContext *source,
    RadioSBroadcastHeaderSnapshot *out)
{
    if (!ctx || !ctx->players) return FALSE;
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(ctx);
    for (guint i = 0; execution && i < RADIOS_AUDIO_NORMAL_DECKS; ++i) {
        if (source && radios_player_workspace_source_context(ctx->players, i) != source) continue;
        RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = i};
        RadioSPlayerScopeExecutionSnapshot schedule = {0};
        if (!radios_player_scope_execution_snapshot(execution, &scope, &schedule) || schedule.source != i ||
            !schedule.schema_active || schedule.state == RADIOS_SCOPE_EXECUTION_IDLE ||
            schedule.state == RADIOS_SCOPE_EXECUTION_ERROR || !schedule.event_name[0]) continue;
        out->programme_active = TRUE;
        broadcast_header_copy(out->programme_title, schedule.event_name, sizeof out->programme_title);
        out->programme_end_utc_us = schedule.ends_at_utc_us;
        out->programme_remaining_us = MAX((gint64)0, schedule.ends_at_utc_us - g_get_real_time());
        if (!out->current) out->waiting = TRUE;
        return TRUE;
    }
    return FALSE;
}
static void broadcast_header_schedule(BroadcastView *view, RadioSViewContext *source, RadioSBroadcastHeaderSnapshot *out)
{
    if (view && broadcast_header_scope_schedule(view->ctx, source, out)) return;
    if (!view || source != view->ctx || !out->programme_active) return;
    RadioSPlayerWeeklySnapshot schedule = {0};
    gboolean owned = radios_player_weekly_header_snapshot(view->general_automation, &schedule);
    for (guint deck = 0; !owned && deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
        GtkWidget *card = g_object_get_data(G_OBJECT(view->ctx->parent), key); g_free(key);
        PlayerAudio *player = card ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
        if (player && !player->destroyed) owned = radios_player_weekly_header_snapshot(player->weekly, &schedule);
    }
    if (owned) {
        out->programme_end_utc_us = schedule.end_utc_us;
        out->programme_remaining_us = MAX((gint64)0, schedule.end_utc_us - g_get_real_time());
    }
}
/* A-4: the flow item selected in the broadcast queue and whether it is the
 * sounding one, so the header counter can show its duration instead. */
static void broadcast_header_selection(BroadcastView *view, RadioSBroadcastHeaderSnapshot *out)
{
    if (!view || view->disposed || !view->selected_event || !view->programme_config) return;
    flow_index_ensure(view);
    FlowLeaf *leaf = g_hash_table_lookup(view->flow_leaves, view->selected_event);
    FlowBlock *block = leaf ? leaf->block : g_hash_table_lookup(view->flow_blocks, view->selected_event);
    if (!block) return;
    out->selected = TRUE; out->selected_content = leaf != NULL;
    out->selected_duration_ms = leaf ? leaf->duration_ms : block->duration_ms;
    broadcast_header_copy(out->selected_title, leaf ? leaf->title : block->title, sizeof out->selected_title);
    RadioSPlayoutSnapshot run = {0};
    if (radios_playout_ui_get_snapshot(programme_context(view), &run) && run.entry_id[0] &&
        (run.state == RADIOS_PLAYOUT_PLAYING || run.state == RADIOS_PLAYOUT_OVERLAP || run.state == RADIOS_PLAYOUT_PAUSED)) {
        FlowLeaf *current = g_hash_table_lookup(view->flow_leaves, run.entry_id);
        out->selected_playing = leaf ? g_str_equal(run.entry_id, view->selected_event) : current && current->block == block;
    }
    if (!leaf && !out->selected_playing) out->selected_playing = event_run_active(view, block->id);
}
/* Prompt/2: aggregate live sources, including private player programme queues. */
gboolean radios_broadcast_header_snapshot(RadioSViewContext *ctx, RadioSBroadcastHeaderSnapshot *out)
{
    if (!out) return FALSE;
    memset(out, 0, sizeof(*out)); out->tempo_ratio = 1.0; out->selected_duration_ms = -1;
    BroadcastView *view = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-broadcast-context") : NULL;
    if (!ctx) return FALSE;
    if (view && view->disposed) return TRUE;
    RadioSPlayoutHeaderSnapshot programme = {0}; radios_playout_ui_header_snapshot(ctx, &programme);
    broadcast_header_programme(out, &programme);
    RadioSPlayoutSnapshot initial = {0};
    if (radios_playout_ui_get_snapshot(ctx, &initial))
        out->waiting = initial.state == RADIOS_PLAYOUT_WAITING || initial.state == RADIOS_PLAYOUT_MANUAL_WAIT;
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx);
    RadioSAudioSnapshot raw = {0};
    if (engine) { radios_audio_engine_snapshot(engine, &raw); radios_audio_engine_unref(engine); }
    gboolean main_ready = engine && raw.buses[RADIOS_AUDIO_BUS_MAIN].ready && raw.buses[RADIOS_AUDIO_BUS_MAIN].gate;
    gint selected = -1; gboolean selected_managed = FALSE;
    RadioSViewContext *selected_source = ctx;
    for (guint deck = 0; main_ready && deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) {
        const RadioSAudioDeckSnapshot *real = &raw.decks[deck];
        gboolean mapped = FALSE;
        for (guint channel = 0; channel < G_N_ELEMENTS(raw.mixer.channels); ++channel)
            mapped |= raw.mixer.channels[channel].source_deck == (gint)deck;
        gboolean paused = real->state == RADIOS_AUDIO_PAUSED;
        if (!mapped || !real->main_routed || (real->state != RADIOS_AUDIO_PLAYING && !paused)) continue;
        RadioSAudioUiSnapshot audio = {0};
        if (!radios_audio_ui_get_snapshot(ctx, deck, &audio) || audio.generation != real->generation ||
            audio.state != (RadioSAudioUiState)real->state) continue;
        RadioSViewContext *source = radios_player_workspace_running_context(ctx->players, deck);
        RadioSPlayoutDeckMetadata metadata = {0};
        gboolean managed = source != NULL;
        if (managed) radios_playout_ui_deck_metadata(source, deck, &metadata);
        else {
            source = ctx;
            PlayerQueue *queue = view ? &view->queues[deck + 1] : NULL;
            managed = queue && queue->initialized && radios_playout_ui_deck_metadata(&queue->context, deck, &metadata);
            if (managed) source = &queue->context;
            else managed = radios_playout_ui_deck_metadata(ctx, deck, &metadata);
        }
        RadioSPlayoutSnapshot run = {0};
        gboolean have_run = managed && radios_playout_ui_get_snapshot(source, &run);
        gboolean current_plan = have_run && run.deck == (gint)deck;
        /* A paused runner freezes its owned PCM while deck.state stays PLAYING. */
        paused |= have_run && run.state == RADIOS_PLAYOUT_PAUSED;
        if (!paused && real->output_bus != RADIOS_AUDIO_BUS_MAIN) continue;
        if (!paused) ++out->active_sources;
        if (selected >= 0 && ((!out->paused && paused) || (out->paused == paused &&
            (!current_plan || selected_managed)))) continue;
        selected = (gint)deck; selected_managed = current_plan; selected_source = source;
        out->current = TRUE; out->paused = paused;
        out->local_deck = TRUE; out->deck = deck;
        broadcast_header_copy(out->entry_id, real->binding.entry_id, sizeof out->entry_id);
        gchar *owner_key = g_strdup_printf("radios-deck-owner-%u", deck);
        out->card = ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), owner_key) : NULL; g_free(owner_key);
        out->waiting = have_run && (run.state == RADIOS_PLAYOUT_WAITING || run.state == RADIOS_PLAYOUT_MANUAL_WAIT);
        broadcast_header_copy(out->title, managed && metadata.title[0] ? metadata.title : audio.title, sizeof out->title);
        broadcast_header_copy(out->artist, managed && metadata.artist[0] ? metadata.artist : audio.artist, sizeof out->artist);
        out->position_ms = audio.position_ms;
        out->duration_ms = managed && metadata.duration_ms ? metadata.duration_ms : audio.duration_ms;
        out->tempo_ratio = isfinite(audio.tempo_ratio) && audio.tempo_ratio > 0 ? audio.tempo_ratio : 1.0;
        if (current_plan && (run.state == RADIOS_PLAYOUT_PLAYING || run.state == RADIOS_PLAYOUT_OVERLAP ||
            run.state == RADIOS_PLAYOUT_PAUSED) && run.duration_ms) {
            out->duration_ms = run.duration_ms; out->position_ms = run.position_ms;
        }
        /* Actual loaded PCM bounds manual and retiring sources as well. */
        guint64 actual_end_ms = real->cue_out_frame ? real->cue_out_frame / 48 : audio.cue_out_ms;
        if (actual_end_ms) out->duration_ms = actual_end_ms;
        RadioSPlayoutHeaderSnapshot started = {0}; radios_playout_ui_header_snapshot(source, &started);
        broadcast_header_programme(out, &started);
        if (!current_plan) out->track_remaining_us = 0;
        if (!managed) {
            gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
            GtkWidget *card = ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), key) : NULL; g_free(key);
            PlayerAudio *player = card ? g_object_get_data(G_OBJECT(card), "radios-player-audio") : NULL;
            if (player && !player->destroyed && player_input_loaded(player, &audio))
                broadcast_header_card_playlist(player, out);
        }
    }
    /* App-owned scope runners keep their accepted queue when cards are hidden
     * or another editor is selected. Observe actual owners in stable order. */
    if (ctx->players && !out->current && !out->programme_active) {
        for (guint i = 0; i < RADIOS_AUDIO_NORMAL_DECKS; ++i) {
            RadioSViewContext *source = radios_player_workspace_source_context(ctx->players, i);
            RadioSPlayoutHeaderSnapshot waiting = {0};
            if (!source || !radios_playout_ui_header_snapshot(source, &waiting) || !waiting.programme_active) continue;
            broadcast_header_programme(out, &waiting); selected_source = source;
            RadioSPlayoutSnapshot run = {0};
            if (radios_playout_ui_get_snapshot(source, &run))
                out->waiting = run.state == RADIOS_PLAYOUT_WAITING || run.state == RADIOS_PLAYOUT_MANUAL_WAIT;
            break;
        }
        if (!out->programme_active) broadcast_header_scope_schedule(ctx, NULL, out);
    }
    /* Accepted player programmes remain current in manual/media waits,
     * even while no deck supplies an audible track. Prefer the viewed queue. */
    if (view && !out->current && !out->programme_active) {
        for (guint pass = 0; pass < G_N_ELEMENTS(view->queues); ++pass) {
            guint slot = pass == 0 ? view->queue_slot : pass;
            if (!slot || slot >= G_N_ELEMENTS(view->queues) ||
                (pass && slot == view->queue_slot) || !view->queues[slot].initialized) continue;
            RadioSPlayoutHeaderSnapshot waiting = {0};
            if (!radios_playout_ui_header_snapshot(&view->queues[slot].context, &waiting) ||
                !waiting.programme_active) continue;
            broadcast_header_programme(out, &waiting); selected_source = &view->queues[slot].context;
            RadioSPlayoutSnapshot wait_run = {0};
            if (radios_playout_ui_get_snapshot(selected_source, &wait_run))
                out->waiting = wait_run.state == RADIOS_PLAYOUT_WAITING || wait_run.state == RADIOS_PLAYOUT_MANUAL_WAIT;
            break;
        }
    }
    broadcast_header_schedule(view, selected_source, out);
    broadcast_header_selection(view, out);
    return TRUE;
}
