#include "radios/automation_scope.h"
#include "radios/automation_calendar_plan.h"
#include "radios/weekly_programme_plan.h"
#include "radios/programme_model.h"
#include <string.h>

static const gchar *const kinds[] = {"music", "advertisement", "jingle", "sweeper"};

static gboolean invalid(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message); return FALSE; }
static JsonObject *object(JsonObject *parent, const gchar *key)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array(JsonObject *parent, const gchar *key)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static const gchar *text(JsonObject *parent, const gchar *key)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : "";
}
static gint64 number(JsonObject *parent, const gchar *key)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 ? json_node_get_int(node) : 0;
}
static void binding_fingerprint(const RadioSPlayerTopology *topology, RadioSAutomationBinding *binding)
{
    GString *state = g_string_new(binding->automation_id);
    for (guint i = 0; i < binding->count; ++i) {
        guint deck = binding->decks[i]; const RadioSPlayerDefinition *player = &topology->players[deck];
        g_string_append_printf(state, "|%u:%s:%s:%d:%d", deck, player->cluster_id,
            radios_player_topology_automation(topology, deck), (gint)player->autodj_mode, player->autodj_target);
    }
    gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, state->str, (gssize)state->len);
    g_strlcpy(binding->topology_fingerprint, digest, sizeof binding->topology_fingerprint);
    g_free(digest); g_string_free(state, TRUE);
}

/* Unclustered enabled players not already in the binding's pool. */
static void binding_auxiliary(const RadioSPlayerTopology *topology, RadioSAutomationBinding *binding)
{
    guint members[RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS];
    guint count = radios_player_topology_members(topology, NULL, members, G_N_ELEMENTS(members));
    binding->auxiliary = 0;
    for (guint i = 0; i < count; ++i) if (members[i] < 32) binding->auxiliary |= (guint32)1 << members[i];
    for (guint i = 0; i < binding->count; ++i) if (binding->decks[i] < 32) binding->auxiliary &= ~((guint32)1 << binding->decks[i]);
}

gboolean radios_automation_scope_resolve(const RadioSPlayerTopology *topology,
    const RadioSAutomationScope *scope, RadioSAutomationBinding *binding, GError **error)
{
    if (!topology || !scope || !binding) return invalid(error, "Otomasyon kapsamı veya oynatıcı tanımı eksik.");
    *binding = (RadioSAutomationBinding){.source = G_MAXUINT, .autodj_target = -1};
    if (!radios_player_topology_validate(topology, error)) return FALSE;
    binding->scope = *scope;
    if (scope->kind == RADIOS_AUTOMATION_SCOPE_PLAYER) {
        const RadioSPlayerDefinition *player = radios_player_topology_player_const(topology, scope->deck);
        if (!player) return invalid(error, "Otomasyon oynatıcısı artık tanımlı değil.");
        binding->source = binding->decks[0] = scope->deck; binding->count = 1;
        binding->autodj_mode = player->autodj_mode;
        binding->autodj_target = radios_player_topology_next_target(topology, scope->deck);
        if (binding->autodj_target >= 0) binding->decks[binding->count++] = (guint)binding->autodj_target;
        g_strlcpy(binding->automation_id, radios_player_topology_automation(topology, scope->deck), sizeof binding->automation_id);
        binding->inherited = !*player->automation_id && *player->cluster_id && *binding->automation_id;
        binding_auxiliary(topology, binding);
        binding_fingerprint(topology, binding);
        return TRUE;
    }
    const gchar *cluster_id = NULL;
    if (scope->kind == RADIOS_AUTOMATION_SCOPE_CLUSTER) {
        if (!memchr(scope->cluster_id, '\0', sizeof scope->cluster_id)) return invalid(error, "Küme kimliği geçersiz.");
        const RadioSPlayerCluster *cluster = radios_player_topology_cluster_const(topology, scope->cluster_id);
        if (!cluster) return invalid(error, "Otomasyon kümesi artık tanımlı değil.");
        cluster_id = cluster->id;
        g_strlcpy(binding->automation_id, cluster->automation_id, sizeof binding->automation_id);
    } else if (scope->kind != RADIOS_AUTOMATION_SCOPE_UNCLUSTERED)
        return invalid(error, "Otomasyon kapsamının türü geçersiz.");
    binding->count = radios_player_topology_members(topology, cluster_id, binding->decks, G_N_ELEMENTS(binding->decks));
    if (binding->count) {
        binding->source = binding->decks[0];
        binding->autodj_mode = topology->players[binding->source].autodj_mode;
        binding->autodj_target = radios_player_topology_next_target(topology, binding->source);
        /* A group's continuation choice determines the first transition while
         * retaining every member for explicit saved event distributions. */
        for (guint i = 1; binding->autodj_target >= 0 && i < binding->count; ++i) {
            if (binding->decks[i] != (guint)binding->autodj_target) continue;
            guint target = binding->decks[i];
            memmove(&binding->decks[2], &binding->decks[1], (i - 1) * sizeof binding->decks[0]);
            binding->decks[1] = target; break;
        }
        /* A cluster plays together. Its members' individual flags are not
         * editable while clustered (a card left on Yarı AutoDJ before it was
         * moved in must not shrink the whole group to one player). */
        if (scope->kind == RADIOS_AUTOMATION_SCOPE_CLUSTER)
            binding->autodj_mode = binding->count > 1 ? RADIOS_AUTODJ_TRANSITIONS : RADIOS_AUTODJ_SEMI;
    }
    binding_auxiliary(topology, binding);
    binding_fingerprint(topology, binding);
    return TRUE;
}

gboolean radios_automation_scope_resolve_routed(const RadioSPlayerTopology *topology,
    const RadioSAutomationScope *scope, const guint *decks, guint count,
    RadioSAutomationBinding *binding, GError **error)
{
    if (!scope || scope->kind != RADIOS_AUTOMATION_SCOPE_PLAYER || !decks || !count ||
        count > RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS || !binding)
        return invalid(error, "Rotasyonun oynatıcı kapsamı geçersiz.");
    RadioSAutomationBinding captured;
    if (!radios_automation_scope_resolve(topology, scope, &captured, error)) return FALSE;
    guint32 seen = 0; gboolean has_source = FALSE;
    for (guint i = 0; i < count; ++i) {
        guint deck = decks[i];
        /* Unclustered auxiliary players (cart-style) count as the scope's own. */
        if (deck >= RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS || (seen & ((guint32)1 << deck)) ||
            (!radios_player_topology_same_group(topology, captured.source, deck) && !(captured.auxiliary & ((guint32)1 << deck))))
            return invalid(error, "Rotasyonun oynatıcıları aynı kapsamda, tanımlı ve farklı olmalı.");
        seen |= (guint32)1 << deck; has_source |= deck == captured.source;
    }
    if (!has_source) return invalid(error, "Rotasyon kaynak oynatıcısı kayıtlı dağılımın dışında.");
    captured.routed_pool = TRUE; captured.count = 1; captured.decks[0] = captured.source;
    for (guint i = 0; i < count; ++i)
        if (decks[i] != captured.source) captured.decks[captured.count++] = decks[i];
    binding_auxiliary(topology, &captured);
    binding_fingerprint(topology, &captured); *binding = captured; return TRUE;
}

gboolean radios_automation_scope_binding_current(const RadioSPlayerTopology *topology,
    const RadioSAutomationBinding *binding, GError **error)
{
    RadioSAutomationBinding current;
    if (!binding) return invalid(error, "Otomasyon kapsamı eksik.");
    if (binding->routed_pool) {
        if (!radios_automation_scope_resolve_routed(topology, &binding->scope,
                binding->decks, binding->count, &current, error)) return FALSE;
    } else if (!radios_automation_scope_resolve(topology, &binding->scope, &current, error)) return FALSE;
    gboolean same = current.source == binding->source && current.count == binding->count &&
        current.inherited == binding->inherited && current.autodj_mode == binding->autodj_mode &&
        current.autodj_target == binding->autodj_target && current.auxiliary == binding->auxiliary &&
        g_str_equal(current.automation_id, binding->automation_id) &&
        g_str_equal(current.topology_fingerprint, binding->topology_fingerprint);
    for (guint i = 0; same && i < current.count; ++i) same = current.decks[i] == binding->decks[i];
    return same || invalid(error, "Otomasyon ataması veya oynatıcı kapsamı değişti; yeniden hazırlayın.");
}

static gboolean binding_pool_valid(const RadioSAutomationBinding *binding, guint32 *mask, GError **error)
{
    if (!binding || !binding->count || binding->count > RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS)
        return invalid(error, "Otomasyon kapsamında çalıştırılacak oynatıcı yok.");
    *mask = 0;
    gboolean has_source = FALSE;
    for (guint i = 0; i < binding->count; ++i) {
        guint deck = binding->decks[i];
        if (deck >= RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS || (*mask & ((guint32)1 << deck)))
            return invalid(error, "Otomasyon oynatıcı havuzu geçersiz veya yineleniyor.");
        *mask |= (guint32)1 << deck; has_source |= deck == binding->source;
    }
    return has_source || invalid(error, "Otomasyon kaynak oynatıcısı kapsam dışında.");
}
static JsonArray *scope_pool(const RadioSAutomationBinding *binding)
{
    JsonArray *pool = json_array_new();
    for (guint j = 0; j < binding->count; ++j) json_array_add_int_element(pool, binding->decks[j] + 1);
    return pool;
}
/* Saved per-kind pools keep their order inside the scope. Advertisement,
 * jingle and sweeper pools also keep the binding's unclustered auxiliary
 * players, so a music cluster can hand its spots to separate cart players.
 * Other players outside the scope are dropped and reported through *missing
 * (bit = slot); a kind left without players falls back to the complete scope
 * pool. Malformed input
 * (missing kinds, non-integers, out-of-range or repeated numbers) is rejected. */
static JsonObject *routed_distribution(JsonObject *routing, const RadioSAutomationBinding *binding,
    guint32 scope, guint32 *missing, GError **error)
{
    if (!routing || json_object_get_size(routing) != G_N_ELEMENTS(kinds)) { invalid(error, "Otomasyon kanal dağılımı geçersiz."); return NULL; }
    JsonObject *result = json_object_new();
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        JsonArray *pool = array(routing, kinds[i]); guint32 seen = 0;
        if (!pool || !json_array_get_length(pool) || json_array_get_length(pool) > RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS) {
            json_object_unref(result); invalid(error, "Otomasyon medya türünün oynatıcı havuzu boş veya geçersiz."); return NULL;
        }
        JsonArray *kept = json_array_new(); json_object_set_array_member(result, kinds[i], kept);
        const guint32 allowed = i == 0 ? scope : scope | binding->auxiliary; /* kinds[0] is music */
        for (guint j = 0; j < json_array_get_length(pool); ++j) {
            JsonNode *node = json_array_get_element(pool, j);
            if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) {
                json_object_unref(result); invalid(error, "Otomasyon oynatıcı kimliği tam sayı olmalı."); return NULL;
            }
            gint64 slot = json_node_get_int(node);
            if (slot < 1 || slot > 32 || (seen & ((guint32)1 << (slot - 1)))) {
                json_object_unref(result); invalid(error, "Otomasyon kanal dağılımı yineleniyor veya 1..32 dışında."); return NULL;
            }
            seen |= (guint32)1 << (slot - 1);
            if (allowed & ((guint32)1 << (slot - 1))) json_array_add_int_element(kept, slot);
            else *missing |= (guint32)1 << (slot - 1);
        }
        if (!json_array_get_length(kept)) json_object_set_array_member(result, kinds[i], scope_pool(binding));
    }
    return result;
}
static gchar *missing_notice(const RadioSAutomationBinding *binding, guint32 missing)
{
    GString *text = g_string_new("Şemadaki ");
    for (guint slot = 0, listed = 0; slot < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++slot) {
        if (!(missing & ((guint32)1 << slot))) continue;
        g_string_append_printf(text, "%sOynatıcı %u", listed++ ? ", " : "", slot + 1);
    }
    g_string_append(text, binding->scope.kind == RADIOS_AUTOMATION_SCOPE_CLUSTER ?
        " bu kümede yok; kümenin oynatıcıları kullanılıyor." :
        binding->scope.kind == RADIOS_AUTOMATION_SCOPE_UNCLUSTERED ?
        " Kümesizler içinde yok; Kümesizler oynatıcıları kullanılıyor." :
        " bu oynatıcının kapsamında yok; kapsamın oynatıcıları kullanılıyor.");
    return g_string_free(text, FALSE);
}
static JsonObject *default_distribution(const RadioSAutomationBinding *binding)
{
    JsonObject *routing = json_object_new();
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) json_object_set_array_member(routing, kinds[i], scope_pool(binding));
    return routing;
}
JsonNode *radios_automation_scope_route_event_notice(const RadioSAutomationBinding *binding,
    JsonObject *resolved_event, gchar **notice, GError **error)
{
    guint32 mask = 0, missing = 0;
    if (notice) *notice = NULL;
    if (!binding_pool_valid(binding, &mask, error)) return NULL;
    if (!resolved_event || !array(resolved_event, "rotations")) { invalid(error, "Çözümlenmiş otomasyon eventi eksik."); return NULL; }
    JsonObject *event = radios_programme_clone_object(resolved_event);
    JsonArray *rotations = array(event, "rotations");
    for (guint i = 0; i < json_array_get_length(rotations); ++i) {
        JsonNode *node = json_array_get_element(rotations, i);
        JsonObject *source = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        if (!source || json_object_has_member(source, "rotation_id")) {
            json_object_unref(event); invalid(error, "Otomasyon eventi önce kayıtlı rotasyonlarla çözümlenmeli."); return NULL;
        }
        JsonObject *routing = object(source, "autodj");
        JsonObject *settings = object(source, "calendar_settings");
        if (!routing && json_object_has_member(source, "autodj")) {
            json_object_unref(event); invalid(error, "Otomasyon kanal dağılımı geçersiz."); return NULL;
        }
        if (!routing && settings && json_object_has_member(settings, "autodj_players")) {
            routing = object(settings, "autodj_players");
            if (!routing) { json_object_unref(event); invalid(error, "Otomasyon medya türü dağılımı geçersiz."); return NULL; }
        }
        if (routing) {
            JsonObject *routed = routed_distribution(routing, binding, mask, &missing, error);
            if (!routed) { json_object_unref(event); return NULL; }
            json_object_set_object_member(source, "autodj", routed);
        } else json_object_set_object_member(source, "autodj", default_distribution(binding));
    }
    JsonNode *validated = radios_automation_calendar_prepare_config(event, NULL, error);
    if (!validated) { json_object_unref(event); return NULL; }
    json_node_free(validated);
    if (notice && missing) *notice = missing_notice(binding, missing);
    JsonNode *result = json_node_new(JSON_NODE_OBJECT); json_node_take_object(result, event); return result;
}
JsonNode *radios_automation_scope_route_event(const RadioSAutomationBinding *binding,
    JsonObject *resolved_event, GError **error)
{ return radios_automation_scope_route_event_notice(binding, resolved_event, NULL, error); }

gboolean radios_automation_scope_record_valid(JsonObject *record, const gchar *station_id,
    const gchar *document_id, gint64 revision, GError **error)
{
    if (!record || !station_id || !g_uuid_string_is_valid(station_id) || !document_id ||
        !g_uuid_string_is_valid(document_id) || revision < 0 || !g_str_equal(text(record, "_id"), document_id) ||
        !g_str_equal(text(record, "station_id"), station_id) || !g_str_equal(text(record, "kind"), "weekly_programme") ||
        !g_str_equal(text(record, "status"), "active") || !*text(record, "name") || number(record, "_rev") < 1 ||
        (revision && number(record, "_rev") != revision) || !object(record, "config"))
        return invalid(error, "Otomasyon şeması kimliği, istasyonu, durumu veya sürümü değişti.");
    GError *failure = NULL;
    RadioSWeeklyProgrammeOccurrence *probe = radios_weekly_programme_next(object(record, "config"), g_get_real_time(), &failure);
    radios_weekly_programme_occurrence_free(probe);
    if (failure) { g_propagate_error(error, failure); return FALSE; }
    return TRUE;
}
