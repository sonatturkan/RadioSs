#include "radios/player_workspace.h"
#include "radios/player_scope_execution.h"
#include "radios/audio_ui.h"
#include "radios/programme_model.h"
#include <string.h>

typedef struct {
    RadioSDocument *document;
    JsonObject *programme;
} QueueDraft;
struct RadioSPlayerWorkspace {
    RadioSViewContext *ctx;
    GWeakRef owner;
    GtkWidget *document_owner;
    gulong owner_handler;
    RadioSDocument *document, *legacy;
    RadioSPlayerTopology topology;
    RadioSViewContext runners[32];
    QueueDraft queues[32], unclustered;
    GHashTable *cluster_queues;
    gboolean closed, edited, definition_seen;
};

static gboolean invalid(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message); return FALSE; }
static void notify(RadioSPlayerWorkspace *w, GError *error)
{
    if (error && w->ctx->notify) w->ctx->notify(w->ctx->notify_data, error->message);
    g_clear_error(&error);
}
static void definition_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; (void)owner; RadioSPlayerWorkspace *w = data;
    if (w->closed) return;
    w->definition_seen = TRUE;
    RadioSPlayerTopology next; GError *error = NULL;
    if (radios_player_topology_from_json(&next, config, &error)) w->topology = next;
    else notify(w, error);
}
static void legacy_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; (void)owner; RadioSPlayerWorkspace *w = data;
    if (w->closed || w->edited || w->definition_seen || *radios_data_document_id(w->document)) return;
    RadioSPlayerTopology next; GError *error = NULL;
    if (radios_player_topology_from_legacy(&next, config, &error)) w->topology = next;
    else notify(w, error);
}
static void owner_destroyed(GtkWidget *owner, gpointer data)
{
    (void)owner; RadioSPlayerWorkspace *w = data; w->owner_handler = 0;
    radios_player_workspace_close(w);
}
static void draft_free(gpointer data)
{ QueueDraft *draft = data; g_clear_pointer(&draft->programme, json_object_unref); g_free(draft); }
RadioSPlayerWorkspace *radios_player_workspace_new(RadioSViewContext *ctx, GtkWidget *owner)
{
    if (!ctx || !GTK_IS_WIDGET(owner)) return NULL;
    RadioSPlayerWorkspace *w = g_new0(RadioSPlayerWorkspace, 1); w->ctx = ctx;
    g_weak_ref_init(&w->owner, owner);
    w->owner_handler = g_signal_connect(owner, "destroy", G_CALLBACK(owner_destroyed), w);
    /* Document callback lifetimes are tied to this service, independently of
     * the app window and of whichever player widget is currently displayed. */
    w->document_owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref_sink(w->document_owner);
    radios_player_topology_default(&w->topology, 6);
    w->cluster_queues = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, draft_free);
    w->document = radios_data_document_new(ctx, w->document_owner, "live_players", "main", definition_loaded, w);
    w->legacy = radios_data_document_new(ctx, w->document_owner, "channel", "Yayın oynatıcıları", legacy_loaded, w);
    return w;
}
void radios_player_workspace_close(RadioSPlayerWorkspace *w)
{
    if (!w || w->closed) return;
    w->closed = TRUE;
    radios_data_document_preserve_pending(w->document);
    for (guint i = 0; i < 32; ++i) {
        radios_data_document_preserve_pending(w->queues[i].document);
        radios_playout_ui_close(w->runners[i].playout);
    }
    radios_data_document_preserve_pending(w->unclustered.document);
    GHashTableIter iter; gpointer value;
    g_hash_table_iter_init(&iter, w->cluster_queues);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        radios_data_document_preserve_pending(((QueueDraft *)value)->document);
    gtk_widget_destroy(w->document_owner); g_object_unref(w->document_owner);
    w->document_owner = NULL;
}
void radios_player_workspace_free(RadioSPlayerWorkspace *w)
{
    if (!w) return;
    radios_player_workspace_close(w);
    GtkWidget *owner = g_weak_ref_get(&w->owner);
    if (owner && w->owner_handler && g_signal_handler_is_connected(owner, w->owner_handler))
        g_signal_handler_disconnect(owner, w->owner_handler);
    g_clear_object(&owner); g_weak_ref_clear(&w->owner);
    for (guint i = 0; i < 32; ++i) {
        radios_playout_ui_unref(w->runners[i].playout);
        g_clear_pointer(&w->queues[i].programme, json_object_unref);
    }
    g_clear_pointer(&w->unclustered.programme, json_object_unref);
    g_hash_table_unref(w->cluster_queues); g_free(w);
}
gboolean radios_player_workspace_ready(const RadioSPlayerWorkspace *w)
{ return w && !w->closed && radios_data_document_ready(w->document) && radios_data_document_ready(w->legacy); }
void radios_player_workspace_reload(RadioSPlayerWorkspace *w)
{
    if (!w || w->closed) return;
    /* Legacy import is only a first-load fallback. A reload prompt may be
     * cancelled, so it must never make an edited live definition migratable. */
    radios_data_document_reload(w->document); radios_data_document_reload(w->legacy);
}
const RadioSPlayerTopology *radios_player_workspace_topology(const RadioSPlayerWorkspace *w)
{ return w && !w->closed ? &w->topology : NULL; }
gboolean radios_player_workspace_contains(const RadioSPlayerWorkspace *w, guint deck)
{ return radios_player_workspace_ready(w) && radios_player_topology_player_const(&w->topology, deck) != NULL; }
gboolean radios_player_workspace_replace(RadioSPlayerWorkspace *w,
    const RadioSPlayerTopology *topology, GError **error)
{
    if (!radios_player_workspace_ready(w)) return invalid(error, "Oynatıcı tanımları henüz yüklenmedi.");
    if (!radios_player_topology_validate(topology, error)) return FALSE;
    if (!radios_player_scope_execution_validate_topology(radios_player_scope_execution_get(w->ctx), w, topology, error)) return FALSE;
    /* Names and future policies may change, but active decks keep their group
     * and identity until the operator stops their current run. */
    for (guint deck = 0; deck < 32; ++deck) {
        const RadioSPlayerDefinition *old = &w->topology.players[deck], *next = &topology->players[deck];
        if (old->enabled == next->enabled && !g_strcmp0(old->cluster_id, next->cluster_id)) continue;
        RadioSAudioUiSnapshot audio = {0}; RadioSPlayoutSnapshot run = {0};
        radios_audio_ui_get_snapshot(w->ctx, deck, &audio); radios_player_workspace_snapshot(w, deck, &run);
        gboolean scoped_owned = FALSE;
        for (guint source = 0; source < 32 && !scoped_owned; ++source)
            if (w->runners[source].playout) scoped_owned = radios_playout_ui_owns_deck(&w->runners[source], deck);
        if (audio.state == RADIOS_AUDIO_UI_PLAYING || audio.state == RADIOS_AUDIO_UI_PAUSED ||
            (w->ctx->playout && radios_playout_ui_owns_deck(w->ctx, deck)) || scoped_owned ||
            run.state == RADIOS_PLAYOUT_PREPARING || run.state == RADIOS_PLAYOUT_PLAYING ||
            run.state == RADIOS_PLAYOUT_OVERLAP || run.state == RADIOS_PLAYOUT_PAUSED)
            return invalid(error, "Çalışan oynatıcının kümesini veya kimliğini değiştirmeden önce sırasını durdurun.");
    }
    JsonObject *config = radios_player_topology_to_json(topology);
    if (!config) return invalid(error, "Oynatıcı tanımları kaydedilemedi.");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, config);
    w->topology = *topology; w->edited = TRUE;
    radios_data_document_dirty(w->document, TRUE); radios_data_document_autosave(w->document, node);
    json_node_free(node); return TRUE;
}
gboolean radios_player_workspace_assign(RadioSPlayerWorkspace *w,
    const RadioSAutomationScope *scope, const gchar *automation_id, GError **error)
{
    if (!w || !scope || w->closed) return invalid(error, "Otomasyon atamasının kapsamı eksik.");
    RadioSPlayerTopology next = w->topology; gboolean accepted;
    if (scope->kind == RADIOS_AUTOMATION_SCOPE_PLAYER)
        accepted = radios_player_topology_set_automation(&next, scope->deck, automation_id, error);
    else if (scope->kind == RADIOS_AUTOMATION_SCOPE_CLUSTER)
        accepted = radios_player_topology_set_cluster_automation(&next, scope->cluster_id, automation_id, error);
    else return invalid(error, "Kümesizler toplamına değil, bir oynatıcıya ayrı otomasyon atayın.");
    return accepted && radios_player_workspace_replace(w, &next, error);
}
static void queue_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; (void)owner; QueueDraft *draft = data;
    g_clear_pointer(&draft->programme, json_object_unref);
    draft->programme = radios_programme_clone_object(config);
}
static QueueDraft *queue_get(RadioSPlayerWorkspace *w, const RadioSAutomationScope *scope, GError **error)
{
    RadioSAutomationBinding binding;
    if (!w || w->closed || !radios_automation_scope_resolve(&w->topology, scope, &binding, error)) return NULL;
    QueueDraft *draft; gchar *name;
    if (scope->kind == RADIOS_AUTOMATION_SCOPE_UNCLUSTERED) {
        draft = &w->unclustered; name = g_strdup("Kümesizler yayın sırası");
    } else if (scope->kind == RADIOS_AUTOMATION_SCOPE_PLAYER) {
        draft = &w->queues[scope->deck]; name = g_strdup_printf("Oynatıcı %u yayın sırası", scope->deck + 1);
    } else {
        draft = g_hash_table_lookup(w->cluster_queues, scope->cluster_id);
        if (!draft) { draft = g_new0(QueueDraft, 1); g_hash_table_insert(w->cluster_queues, g_strdup(scope->cluster_id), draft); }
        name = g_strdup_printf("Küme %s yayın sırası", scope->cluster_id);
    }
    if (!draft->document) {
        draft->document = radios_data_document_new(w->ctx, w->document_owner, "programme", name, queue_loaded, draft);
    }
    g_free(name); return draft;
}
gboolean radios_player_workspace_get_programme(RadioSPlayerWorkspace *w,
    const RadioSAutomationScope *scope, JsonObject **programme, GError **error)
{
    if (programme) *programme = NULL;
    if (!programme) return invalid(error, "Yayın sırası çıktısı eksik.");
    QueueDraft *draft = queue_get(w, scope, error); if (!draft) return FALSE;
    if (!radios_data_document_ready(draft->document)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Yayın sırası kaydı henüz yüklenmedi."); return FALSE;
    }
    if (!draft->programme) return invalid(error, "Seçili kapsamın yayın sırası boş.");
    *programme = radios_programme_clone_object(draft->programme); return TRUE;
}
gboolean radios_player_workspace_programme_saved(RadioSPlayerWorkspace *w,
    const RadioSAutomationScope *scope, GError **error)
{
    QueueDraft *draft = queue_get(w, scope, error); if (!draft) return FALSE;
    if (radios_data_document_ready(draft->document) && radios_data_document_revision(draft->document) > 0) return TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Yayın sırası kaydının doğrulanması bekleniyor."); return FALSE;
}
gboolean radios_player_workspace_set_programme(RadioSPlayerWorkspace *w,
    const RadioSAutomationScope *scope, JsonObject *programme, GError **error)
{
    if (!programme) return invalid(error, "Yayın sırası eksik.");
    QueueDraft *draft = queue_get(w, scope, error); if (!draft) return FALSE;
    if (!radios_data_document_ready(draft->document)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Yayın sırası kaydı henüz yüklenmedi."); return FALSE;
    }
    JsonObject *copy = radios_programme_clone_object(programme);
    /* Destination belongs to the definitions/AutoDJ policy, never the loaded list. */
    json_object_remove_member(copy, "autodj_players"); json_object_remove_member(copy, "beat_sync");
    json_object_remove_member(copy, "queue_player"); json_object_remove_member(copy, "queue_transition");
    json_object_remove_member(copy, "queue_overlap");
    if (scope->kind == RADIOS_AUTOMATION_SCOPE_PLAYER) {
        json_object_set_int_member(copy, "queue_player", scope->deck + 1);
        json_object_set_int_member(copy, "queue_transition", 0); json_object_set_double_member(copy, "queue_overlap", 0);
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, copy);
    radios_data_document_dirty(draft->document, TRUE); radios_data_document_autosave(draft->document, node);
    g_clear_pointer(&draft->programme, json_object_unref); draft->programme = copy;
    json_node_free(node); return TRUE;
}
static JsonObject *player_projection(RadioSPlayerWorkspace *w, guint deck)
{
    JsonObject *row = json_object_new(); const RadioSPlayerDefinition *player = &w->topology.players[deck];
    json_object_set_int_member(row, "player", deck + 1); json_object_set_string_member(row, "name", player->name);
    json_object_set_string_member(row, "cluster_id", player->cluster_id);
    json_object_set_string_member(row, "automation_id", radios_player_topology_automation(&w->topology, deck));
    RadioSAudioUiSnapshot audio = {0}; radios_audio_ui_get_snapshot(w->ctx, deck, &audio);
    json_object_set_boolean_member(row, "playing", audio.state == RADIOS_AUDIO_UI_PLAYING);
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = deck};
    QueueDraft *draft = queue_get(w, &scope, NULL);
    json_object_set_boolean_member(row, "queue_ready", draft && radios_data_document_ready(draft->document));
    if (draft && draft->programme) json_object_set_object_member(row, "programme", radios_programme_clone_object(draft->programme));
    RadioSPlayoutSnapshot run = {.deck = -1, .next_deck = -1}; gint runtime_source = -1;
    for (guint source = 0; source < 32; ++source) {
        if (!w->runners[source].playout || !radios_playout_ui_owns_deck(&w->runners[source], deck)) continue;
        runtime_source = (gint)source; radios_player_workspace_snapshot(w, source, &run); break;
    }
    json_object_set_int_member(row, "runtime_source", runtime_source + 1);
    if (runtime_source >= 0) {
        JsonNode *order = radios_playout_ui_plan_snapshot(&w->runners[runtime_source]);
        if (order) json_object_set_member(row, "autodj_order", order);
    } else if (w->ctx->playout && radios_playout_ui_owns_deck(w->ctx, deck)) {
        /* General MAIN playback is a read-only owner here, never a scoped
         * execution lane. Keep runtime_source zero so scoped Stop cannot
         * acquire or cancel the global controller. */
        JsonNode *order = radios_playout_ui_plan_snapshot(w->ctx);
        if (order) json_object_set_member(row, "autodj_order", order);
    }
    json_object_set_int_member(row, "state", run.state); json_object_set_int_member(row, "index", run.index);
    json_object_set_string_member(row, "entry_id", run.entry_id); return row;
}
JsonNode *radios_player_workspace_projection(RadioSPlayerWorkspace *w,
    const RadioSAutomationScope *scope, GError **error)
{
    RadioSAutomationBinding binding;
    if (!w || w->closed || !radios_automation_scope_resolve(&w->topology, scope, &binding, error)) return NULL;
    JsonObject *result = json_object_new(); JsonArray *players = json_array_new();
    json_object_set_array_member(result, "players", players);
    json_object_set_int_member(result, "scope_kind", scope->kind);
    const RadioSPlayerCluster *cluster = scope->kind == RADIOS_AUTOMATION_SCOPE_CLUSTER ?
        radios_player_topology_cluster_const(&w->topology, scope->cluster_id) : NULL;
    const gchar *name = cluster ? cluster->name : scope->kind == RADIOS_AUTOMATION_SCOPE_UNCLUSTERED ?
        "Kümesizler" : w->topology.players[scope->deck].name;
    json_object_set_string_member(result, "name", name);
    json_object_set_string_member(result, "automation_id", binding.automation_id);
    /* The selected player exposes its own order; transition targets remain
     * part of execution without adding their independent queues to this view. */
    guint count = scope->kind == RADIOS_AUTOMATION_SCOPE_PLAYER ? 1 : binding.count;
    JsonArray *orders = json_array_new(); json_object_set_array_member(result, "autodj_orders", orders);
    for (guint i = 0; i < count; ++i) {
        JsonNode *order = radios_playout_ui_plan_snapshot(&w->runners[binding.decks[i]]);
        if (order) json_array_add_element(orders, order);
    }
    gboolean general_owned = FALSE;
    for (guint i = 0; i < count; ++i) {
        guint deck = binding.decks[i];
        json_array_add_object_element(players, player_projection(w, deck));
        general_owned |= w->ctx->playout && radios_playout_ui_owns_deck(w->ctx, deck);
    }
    if (general_owned) {
        JsonNode *order = radios_playout_ui_plan_snapshot(w->ctx);
        if (order) json_array_add_element(orders, order);
    }
    if (cluster || scope->kind == RADIOS_AUTOMATION_SCOPE_UNCLUSTERED) {
        QueueDraft *draft = queue_get(w, scope, NULL);
        if (draft && draft->programme) json_object_set_object_member(result, "programme", radios_programme_clone_object(draft->programme));
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, result); return node;
}
gboolean radios_player_workspace_start_prepared_pool(RadioSPlayerWorkspace *w,
    const RadioSAutomationBinding *binding, JsonObject *prepared,
    const guint *decks, guint count, RadioSAutoDJMode mode,
    const gchar *automation_id, gboolean assigned, GError **error)
{
    if (!radios_player_workspace_ready(w) || !binding || !binding->count || binding->source >= 32)
        return invalid(error, "Otomasyon kapsamı henüz hazır değil.");
    if (w->edited && radios_data_document_revision(w->document) <= 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING,
            "Oynatıcı tanımları kaydedilip doğrulanmadan AutoDJ başlatılamaz.");
        return FALSE;
    }
    if (!radios_automation_scope_binding_current(&w->topology, binding, error)) return FALSE;
    if (assigned && (!automation_id || !*automation_id || g_strcmp0(automation_id, binding->automation_id)))
        return invalid(error, "Ses eşitleme kaynağı doğrulanmış otomasyon atamasıyla uyuşmuyor.");
    if (mode != RADIOS_AUTODJ_SEMI && mode != RADIOS_AUTODJ_TRANSITIONS)
        return invalid(error, "Yarı AutoDJ veya oynatıcılar arası AutoDJ seçin.");
    if (!decks || !count || count > 32 || decks[0] != binding->source ||
        (mode == RADIOS_AUTODJ_SEMI && count != 1) || (mode == RADIOS_AUTODJ_TRANSITIONS && count < 2))
        return invalid(error, "AutoDJ çalıştırma havuzu geçersiz.");
    guint32 scope = 0, seen = 0;
    for (guint i = 0; i < binding->count; ++i) scope |= (guint32)1 << binding->decks[i];
    for (guint i = 0; i < count; ++i) {
        if (decks[i] >= 32 || !(scope & ((guint32)1 << decks[i])) || (seen & ((guint32)1 << decks[i])))
            return invalid(error, "AutoDJ çalıştırma havuzu kapsam dışında veya yineleniyor.");
        seen |= (guint32)1 << decks[i];
    }
    guint source = binding->source;
    if (!radios_player_workspace_prepare_scope(w, source, error)) return FALSE;
    GtkWidget *owner = g_weak_ref_get(&w->owner);
    if (!owner) return invalid(error, "Oynatıcı çalışma oturumu kapandı.");
    RadioSViewContext *ctx = &w->runners[source];
    gboolean accepted = radios_playout_ui_start_prepared_scope_with_loudness_origin(ctx, owner, prepared,
        decks, count, mode, automation_id, assigned, error);
    g_object_unref(owner); return accepted;
}
gboolean radios_player_workspace_start_prepared(RadioSPlayerWorkspace *w,
    const RadioSAutomationBinding *binding, JsonObject *prepared, RadioSAutoDJMode mode, GError **error)
{
    if (!binding) return invalid(error, "Otomasyon kapsamı eksik.");
    return radios_player_workspace_start_prepared_pool(w, binding, prepared, binding->decks,
        mode == RADIOS_AUTODJ_SEMI ? 1 : binding->count, mode,
        binding->automation_id, binding->automation_id[0] != 0, error);
}
gboolean radios_player_workspace_prepare_scope(RadioSPlayerWorkspace *w, guint source, GError **error)
{
    if (!radios_player_workspace_ready(w) || !radios_player_workspace_contains(w, source))
        return invalid(error, "AutoDJ kaynak oynatıcısı hazır değil.");
    if (w->edited && radios_data_document_revision(w->document) <= 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING,
            "Oynatıcı tanımları kaydedilip doğrulanmadan AutoDJ hazırlanamaz.");
        return FALSE;
    }
    RadioSViewContext *ctx = &w->runners[source];
    if (!ctx->playout) {
        *ctx = *w->ctx; ctx->broadcast = NULL; ctx->advertisement = NULL;
        ctx->playout = radios_playout_ui_new_scope(ctx, source, w->document_owner);
    }
    if (radios_playout_ui_ready(ctx)) return TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "AutoDJ kapsamının geçmişi henüz yüklenmedi.");
    return FALSE;
}
gboolean radios_player_workspace_action(RadioSPlayerWorkspace *w,
    guint source, RadioSPlayoutAction action, GError **error)
{
    if (!w || w->closed || source >= 32 || !w->runners[source].playout)
        return invalid(error, "Seçili kapsamda çalışan bir yayın sırası yok.");
    return radios_playout_ui_action(&w->runners[source], action, error);
}
gboolean radios_player_workspace_snapshot(RadioSPlayerWorkspace *w,
    guint source, RadioSPlayoutSnapshot *snapshot)
{
    if (snapshot) *snapshot = (RadioSPlayoutSnapshot){.deck = -1, .next_deck = -1};
    return w && !w->closed && source < 32 && snapshot && w->runners[source].playout &&
        radios_playout_ui_get_snapshot(&w->runners[source], snapshot);
}

RadioSViewContext *radios_player_workspace_source_context(RadioSPlayerWorkspace *w, guint source)
{
    return w && !w->closed && source < 32 && w->runners[source].playout ? &w->runners[source] : NULL;
}
RadioSViewContext *radios_player_workspace_running_context(RadioSPlayerWorkspace *w, guint deck)
{
    if (!w || w->closed || deck >= 32) return NULL;
    for (guint source = 0; source < 32; ++source)
        if (w->runners[source].playout && radios_playout_ui_owns_deck(&w->runners[source], deck)) return &w->runners[source];
    return NULL;
}
