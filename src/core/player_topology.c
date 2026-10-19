#include "radios/player_topology.h"
#include <string.h>

static gboolean invalid(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, message); return FALSE; }
static gboolean uuid(const gchar *value)
{
    if (!value || (*value && !g_uuid_string_is_valid(value))) return FALSE;
    for (const gchar *cursor = value; *cursor; ++cursor)
        if (g_ascii_isupper(*cursor)) return FALSE;
    return TRUE;
}
static gboolean bounded_text(const gchar *value, gsize size, guint characters)
{
    const gchar *end = memchr(value, 0, size);
    return end && g_utf8_validate(value, end - value, NULL) &&
        g_utf8_strlen(value, end - value) <= (glong)characters;
}
static void defaults(RadioSPlayerDefinition *player)
{ memset(player, 0, sizeof *player); player->enabled = TRUE; player->autodj_target = -1; }
void radios_player_topology_default(RadioSPlayerTopology *topology, guint count)
{
    g_return_if_fail(topology);
    memset(topology, 0, sizeof *topology);
    count = CLAMP(count, 1, RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS);
    for (guint deck = 0; deck < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++deck) {
        topology->players[deck].autodj_target = -1;
        if (deck < count) defaults(&topology->players[deck]);
    }
}
guint radios_player_topology_count(const RadioSPlayerTopology *topology)
{
    guint count = 0;
    for (guint i = 0; topology && i < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++i)
        count += topology->players[i].enabled != FALSE;
    return count;
}
const RadioSPlayerDefinition *radios_player_topology_player_const(const RadioSPlayerTopology *topology, guint deck)
{ return topology && deck < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS && topology->players[deck].enabled ? &topology->players[deck] : NULL; }
RadioSPlayerDefinition *radios_player_topology_player(RadioSPlayerTopology *topology, guint deck)
{ return (RadioSPlayerDefinition *)radios_player_topology_player_const(topology, deck); }
const RadioSPlayerCluster *radios_player_topology_cluster_const(const RadioSPlayerTopology *topology, const gchar *id)
{
    if (!topology || !id || !*id || topology->cluster_count > RADIOS_PLAYER_TOPOLOGY_MAX_CLUSTERS) return NULL;
    for (guint i = 0; i < topology->cluster_count; ++i)
        if (g_str_equal(topology->clusters[i].id, id)) return &topology->clusters[i];
    return NULL;
}
RadioSPlayerCluster *radios_player_topology_cluster(RadioSPlayerTopology *topology, const gchar *id)
{ return (RadioSPlayerCluster *)radios_player_topology_cluster_const(topology, id); }
gboolean radios_player_topology_same_group(const RadioSPlayerTopology *topology, guint first, guint second)
{
    const RadioSPlayerDefinition *a = radios_player_topology_player_const(topology, first);
    const RadioSPlayerDefinition *b = radios_player_topology_player_const(topology, second);
    return a && b && g_str_equal(a->cluster_id, b->cluster_id);
}
guint radios_player_topology_members(const RadioSPlayerTopology *topology, const gchar *cluster_id, guint *decks, guint capacity)
{
    const gchar *id = cluster_id ? cluster_id : ""; guint count = 0;
    for (guint i = 0; topology && i < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++i)
        if (topology->players[i].enabled && g_str_equal(topology->players[i].cluster_id, id)) {
            if (decks && count < capacity) decks[count] = i;
            ++count;
        }
    return count;
}
static gboolean target_valid(const RadioSPlayerTopology *topology, guint deck, const RadioSPlayerDefinition *player)
{
    return player->autodj_target == -1 ||
        (player->autodj_mode == RADIOS_AUTODJ_TRANSITIONS && player->autodj_target >= 0 &&
         (guint)player->autodj_target != deck &&
         radios_player_topology_same_group(topology, deck, (guint)player->autodj_target));
}
gboolean radios_player_topology_validate(const RadioSPlayerTopology *topology, GError **error)
{
    if (!topology || topology->cluster_count > RADIOS_PLAYER_TOPOLOGY_MAX_CLUSTERS ||
        !radios_player_topology_count(topology)) return invalid(error, "En az bir, en çok 32 oynatıcı tanımlanmalı.");
    if (!bounded_text(topology->master_name, sizeof topology->master_name, 64)) return invalid(error, "MASTER adı geçersiz.");
    for (guint i = 0; i < topology->cluster_count; ++i) {
        const RadioSPlayerCluster *cluster = &topology->clusters[i];
        if (!memchr(cluster->id, 0, sizeof cluster->id) || !cluster->id[0] || !uuid(cluster->id) ||
            !bounded_text(cluster->name, sizeof cluster->name, 80) || !cluster->name[0] ||
            !memchr(cluster->automation_id, 0, sizeof cluster->automation_id) || !uuid(cluster->automation_id))
            return invalid(error, "Küme kimliği, adı veya otomasyon şeması geçersiz.");
        gchar *trimmed = g_strdup(cluster->name); g_strstrip(trimmed);
        gboolean trimmed_valid = *trimmed && g_str_equal(trimmed, cluster->name); g_free(trimmed);
        if (!trimmed_valid) return invalid(error, "Küme adı boş olamaz ve başında veya sonunda boşluk içeremez.");
        for (guint j = 0; j < i; ++j)
            if (g_str_equal(topology->clusters[j].id, cluster->id) || g_str_equal(topology->clusters[j].name, cluster->name))
                return invalid(error, "Küme kimlikleri ve adları farklı olmalı.");
    }
    for (guint deck = 0; deck < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++deck) {
        const RadioSPlayerDefinition *player = &topology->players[deck];
        if (!player->enabled) continue;
        if (!bounded_text(player->name, sizeof player->name, 64) ||
            !memchr(player->cluster_id, 0, sizeof player->cluster_id) || !uuid(player->cluster_id) ||
            (*player->cluster_id && !radios_player_topology_cluster_const(topology, player->cluster_id)) ||
            !memchr(player->automation_id, 0, sizeof player->automation_id) || !uuid(player->automation_id))
            return invalid(error, "Oynatıcı adı, kümesi veya otomasyon şeması geçersiz.");
        if (player->autodj_mode < RADIOS_AUTODJ_OFF || player->autodj_mode > RADIOS_AUTODJ_TRANSITIONS)
            return invalid(error, "AutoDJ çalışma düzeyi geçersiz.");
    }
    /* Validate every string before comparing a target against another player. */
    for (guint deck = 0; deck < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++deck)
        if (topology->players[deck].enabled && !target_valid(topology, deck, &topology->players[deck]))
            return invalid(error, "AutoDJ hedefi aynı kümede ya da Kümesizler içinde farklı bir oynatıcı olmalı.");
    return TRUE;
}
static void repair_targets(RadioSPlayerTopology *topology)
{
    for (guint deck = 0; deck < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++deck)
        if (topology->players[deck].enabled && !target_valid(topology, deck, &topology->players[deck]))
            topology->players[deck].autodj_target = -1;
}
gboolean radios_player_topology_add_player(RadioSPlayerTopology *topology, const gchar *cluster_id, guint *deck, GError **error)
{
    if (!topology || (* (cluster_id ? cluster_id : "") && !radios_player_topology_cluster_const(topology, cluster_id)))
        return invalid(error, "Oynatıcı için geçerli bir küme seçilmeli.");
    for (guint i = 0; i < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++i) if (!topology->players[i].enabled) {
        defaults(&topology->players[i]); g_strlcpy(topology->players[i].cluster_id, cluster_id ? cluster_id : "", sizeof topology->players[i].cluster_id);
        if (deck) *deck = i;
        return TRUE;
    }
    return invalid(error, "En çok 32 oynatıcı tanımlanabilir.");
}
gboolean radios_player_topology_remove_player(RadioSPlayerTopology *topology, guint deck, GError **error)
{
    if (!radios_player_topology_player(topology, deck)) return invalid(error, "Oynatıcı bulunamadı.");
    if (radios_player_topology_count(topology) <= 1) return invalid(error, "En az bir oynatıcı kalmalı.");
    memset(&topology->players[deck], 0, sizeof topology->players[deck]); topology->players[deck].autodj_target = -1;
    repair_targets(topology); return TRUE;
}
gboolean radios_player_topology_add_cluster(RadioSPlayerTopology *topology, const gchar *name, gchar id[37], GError **error)
{
    if (!topology || !name || !g_utf8_validate(name, -1, NULL) || strlen(name) >= sizeof topology->clusters[0].name ||
        g_utf8_strlen(name, -1) > 80 || topology->cluster_count >= RADIOS_PLAYER_TOPOLOGY_MAX_CLUSTERS)
        return invalid(error, "Küme adı en çok 80 karakter olmalı; en çok 32 küme tanımlanabilir.");
    RadioSPlayerTopology next = *topology;
    RadioSPlayerCluster *cluster = &next.clusters[next.cluster_count++]; memset(cluster, 0, sizeof *cluster);
    gchar *new_id = g_uuid_string_random(); g_strlcpy(cluster->id, new_id, sizeof cluster->id); g_free(new_id);
    g_strlcpy(cluster->name, name, sizeof cluster->name);
    if (!radios_player_topology_validate(&next, error)) return FALSE;
    *topology = next; if (id) g_strlcpy(id, cluster->id, 37); return TRUE;
}
gboolean radios_player_topology_remove_cluster(RadioSPlayerTopology *topology, const gchar *id, GError **error)
{
    RadioSPlayerCluster *cluster = radios_player_topology_cluster(topology, id);
    if (!cluster) return invalid(error, "Küme bulunamadı.");
    guint index = (guint)(cluster - topology->clusters);
    for (guint deck = 0; deck < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++deck)
        if (g_str_equal(topology->players[deck].cluster_id, id)) topology->players[deck].cluster_id[0] = 0;
    memmove(&topology->clusters[index], &topology->clusters[index + 1], (topology->cluster_count - index - 1) * sizeof *cluster);
    memset(&topology->clusters[--topology->cluster_count], 0, sizeof *cluster); repair_targets(topology); return TRUE;
}
gboolean radios_player_topology_set_cluster(RadioSPlayerTopology *topology, guint deck, const gchar *cluster_id, GError **error)
{
    RadioSPlayerDefinition *player = radios_player_topology_player(topology, deck);
    const gchar *id = cluster_id ? cluster_id : "";
    if (!player || (*id && !radios_player_topology_cluster_const(topology, id))) return invalid(error, "Oynatıcı veya küme bulunamadı.");
    g_strlcpy(player->cluster_id, id, sizeof player->cluster_id); repair_targets(topology); return TRUE;
}
gboolean radios_player_topology_set_automation(RadioSPlayerTopology *topology, guint deck, const gchar *automation_id, GError **error)
{
    RadioSPlayerDefinition *player = radios_player_topology_player(topology, deck);
    const gchar *id = automation_id ? automation_id : "";
    if (!player || !uuid(id)) return invalid(error, "Oynatıcı veya otomasyon şeması kimliği geçersiz.");
    g_strlcpy(player->automation_id, id, sizeof player->automation_id); return TRUE;
}
gboolean radios_player_topology_set_cluster_automation(RadioSPlayerTopology *topology, const gchar *cluster_id, const gchar *automation_id, GError **error)
{
    RadioSPlayerCluster *cluster = radios_player_topology_cluster(topology, cluster_id);
    const gchar *id = automation_id ? automation_id : "";
    if (!cluster || !uuid(id)) return invalid(error, "Küme veya otomasyon şeması kimliği geçersiz.");
    g_strlcpy(cluster->automation_id, id, sizeof cluster->automation_id); return TRUE;
}
const gchar *radios_player_topology_automation(const RadioSPlayerTopology *topology, guint deck)
{
    const RadioSPlayerDefinition *player = radios_player_topology_player_const(topology, deck);
    if (!player) return "";
    if (*player->automation_id) return player->automation_id;
    const RadioSPlayerCluster *cluster = radios_player_topology_cluster_const(topology, player->cluster_id);
    return cluster ? cluster->automation_id : "";
}
gboolean radios_player_topology_set_autodj(RadioSPlayerTopology *topology, guint deck, RadioSAutoDJMode mode, gint target, GError **error)
{
    RadioSPlayerDefinition *player = radios_player_topology_player(topology, deck);
    if (!player || mode < RADIOS_AUTODJ_OFF || mode > RADIOS_AUTODJ_TRANSITIONS || target < -1 || target >= (gint)RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS)
        return invalid(error, "AutoDJ oynatıcısı, çalışma düzeyi veya hedefi geçersiz.");
    RadioSPlayerDefinition next = *player; next.autodj_mode = mode; next.autodj_target = target;
    if (!target_valid(topology, deck, &next)) return invalid(error, "AutoDJ hedefi aynı grupta farklı bir oynatıcı olmalı.");
    *player = next; return TRUE;
}
gint radios_player_topology_next_target(const RadioSPlayerTopology *topology, guint source)
{
    const RadioSPlayerDefinition *player = radios_player_topology_player_const(topology, source);
    if (!player || player->autodj_mode != RADIOS_AUTODJ_TRANSITIONS) return -1;
    if (player->autodj_target >= 0)
        return target_valid(topology, source, player) ? player->autodj_target : -1;
    for (guint offset = 1; offset < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++offset) {
        guint deck = (source + offset) % RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS;
        if (radios_player_topology_same_group(topology, source, deck)) return (gint)deck;
    }
    return -1;
}

static gboolean allowed(JsonObject *object, const gchar *const *keys)
{
    GList *members = json_object_get_members(object); gboolean result = TRUE;
    for (GList *item = members; item && result; item = item->next) {
        gboolean found = FALSE;
        for (guint i = 0; keys[i]; ++i) found |= g_str_equal(item->data, keys[i]);
        result = found;
    }
    g_list_free(members); return result;
}
static gboolean read_integer(JsonObject *object, const gchar *key, gint64 low, gint64 high, gint64 fallback, gint64 *out)
{
    JsonNode *node = json_object_get_member(object, key);
    if (!node) { *out = fallback; return TRUE; }
    if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) return FALSE;
    *out = json_node_get_int(node); return *out >= low && *out <= high;
}
static gboolean read_string(JsonObject *object, const gchar *key, gchar *out, gsize capacity, const gchar *fallback)
{
    JsonNode *node = json_object_get_member(object, key);
    const gchar *value = fallback;
    if (node) {
        if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) return FALSE;
        value = json_node_get_string(node);
    }
    if (!value || strlen(value) >= capacity) return FALSE;
    g_strlcpy(out, value, capacity); return TRUE;
}
static gboolean parse(RadioSPlayerTopology *topology, JsonObject *configuration, gboolean legacy, GError **error)
{
    static const gchar *const keys[] = {"schema_version", "deck_count", "slots", "names", "clusters", "players", NULL};
    static const gchar *const old_keys[] = {"deck_count", "slots", "names", NULL};
    gint64 count = 0, version = 0;
    if (!topology || !configuration || !allowed(configuration, legacy ? old_keys : keys) ||
        !json_object_has_member(configuration, "deck_count") ||
        !read_integer(configuration, "deck_count", 1, 32, 0, &count) ||
        (!legacy && (!read_integer(configuration, "schema_version", 1, 1, 0, &version) || version != 1)))
        return invalid(error, "Oynatıcı yapılandırmasının alanları veya sürümü geçersiz.");
    RadioSPlayerTopology next; radios_player_topology_default(&next, (guint)count);
    JsonNode *slots = json_object_get_member(configuration, "slots");
    if (slots) {
        if (!JSON_NODE_HOLDS_ARRAY(slots) || json_array_get_length(json_node_get_array(slots)) != (guint)count)
            return invalid(error, "Oynatıcı yuvaları sayıyla eşleşmeli.");
        memset(next.players, 0, sizeof next.players);
        for (guint i = 0; i < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++i) next.players[i].autodj_target = -1;
        JsonArray *array = json_node_get_array(slots);
        for (guint i = 0; i < (guint)count; ++i) {
            JsonNode *node = json_array_get_element(array, i);
            if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64 || json_node_get_int(node) < 0 || json_node_get_int(node) > 31)
                return invalid(error, "Oynatıcı yuvası 0..31 olmalı.");
            guint deck = (guint)json_node_get_int(node);
            if (next.players[deck].enabled) return invalid(error, "Oynatıcı yuvaları farklı olmalı.");
            defaults(&next.players[deck]);
        }
    }
    JsonNode *names = json_object_get_member(configuration, "names");
    if (names) {
        if (!JSON_NODE_HOLDS_OBJECT(names)) return invalid(error, "Oynatıcı adları bir nesne olmalı.");
        JsonObject *object = json_node_get_object(names); GList *members = json_object_get_members(object);
        gboolean names_valid = TRUE;
        for (GList *item = members; item && names_valid; item = item->next) {
            const gchar *key = item->data; gchar *end = NULL; guint64 number = g_ascii_strtoull(key, &end, 10);
            gchar canonical[4]; g_snprintf(canonical, sizeof canonical, "%u", (guint)number);
            names_valid = *key && !*end && number <= 32 && g_str_equal(key, canonical);
            if (names_valid) names_valid = read_string(object, key, number ? next.players[number - 1].name : next.master_name, 257, "");
        }
        g_list_free(members);
        if (!names_valid) return invalid(error, "Oynatıcı adı veya numarası geçersiz.");
    }
    JsonNode *clusters = json_object_get_member(configuration, "clusters");
    if (clusters) {
        if (!JSON_NODE_HOLDS_ARRAY(clusters) || json_array_get_length(json_node_get_array(clusters)) > 32)
            return invalid(error, "Kümeler en çok 32 eleman içeren bir dizi olmalı.");
        JsonArray *array = json_node_get_array(clusters); next.cluster_count = json_array_get_length(array);
        static const gchar *const cluster_keys[] = {"id", "name", "automation_id", NULL};
        for (guint i = 0; i < next.cluster_count; ++i) {
            JsonNode *node = json_array_get_element(array, i);
            if (!JSON_NODE_HOLDS_OBJECT(node)) return invalid(error, "Küme bir nesne olmalı.");
            JsonObject *object = json_node_get_object(node); RadioSPlayerCluster *cluster = &next.clusters[i];
            if (!allowed(object, cluster_keys) || !read_string(object, "id", cluster->id, sizeof cluster->id, "") ||
                !read_string(object, "name", cluster->name, sizeof cluster->name, "") ||
                !read_string(object, "automation_id", cluster->automation_id, sizeof cluster->automation_id, ""))
                return invalid(error, "Küme alanları geçersiz.");
        }
    }
    JsonNode *players = json_object_get_member(configuration, "players");
    if (players) {
        if (!JSON_NODE_HOLDS_OBJECT(players)) return invalid(error, "Oynatıcı ayarları bir nesne olmalı.");
        JsonObject *object = json_node_get_object(players); GList *members = json_object_get_members(object);
        gboolean players_valid = TRUE;
        static const gchar *const player_keys[] = {"cluster_id", "automation_id", "autodj_mode", "autodj_target", NULL};
        for (GList *item = members; item && players_valid; item = item->next) {
            const gchar *key = item->data; gchar *end = NULL; guint64 number = g_ascii_strtoull(key, &end, 10);
            gchar canonical[4]; g_snprintf(canonical, sizeof canonical, "%u", (guint)number);
            JsonNode *node = json_object_get_member(object, key);
            players_valid = *key && !*end && number >= 1 && number <= 32 && g_str_equal(key, canonical) && next.players[number - 1].enabled && JSON_NODE_HOLDS_OBJECT(node);
            if (!players_valid) break;
            JsonObject *settings = json_node_get_object(node); RadioSPlayerDefinition *player = &next.players[number - 1];
            gint64 mode = 0, target = 0;
            players_valid = allowed(settings, player_keys) &&
                read_string(settings, "cluster_id", player->cluster_id, sizeof player->cluster_id, "") &&
                read_string(settings, "automation_id", player->automation_id, sizeof player->automation_id, "") &&
                read_integer(settings, "autodj_mode", 0, 2, 0, &mode) && read_integer(settings, "autodj_target", 0, 32, 0, &target);
            player->autodj_mode = (RadioSAutoDJMode)mode; player->autodj_target = (gint)target - 1;
        }
        g_list_free(members);
        if (!players_valid) return invalid(error, "Oynatıcı ayarları, numarası veya AutoDJ düzeyi geçersiz.");
    }
    if (!radios_player_topology_validate(&next, error)) return FALSE;
    *topology = next; return TRUE;
}
gboolean radios_player_topology_from_json(RadioSPlayerTopology *topology, JsonObject *configuration, GError **error)
{ return parse(topology, configuration, FALSE, error); }
gboolean radios_player_topology_from_legacy(RadioSPlayerTopology *topology, JsonObject *configuration, GError **error)
{ return parse(topology, configuration, TRUE, error); }
JsonObject *radios_player_topology_to_json(const RadioSPlayerTopology *topology)
{
    g_return_val_if_fail(radios_player_topology_validate(topology, NULL), NULL);
    JsonObject *object = json_object_new(), *names = json_object_new(), *players = json_object_new();
    JsonArray *slots = json_array_new(), *clusters = json_array_new();
    json_object_set_int_member(object, "schema_version", 1);
    json_object_set_int_member(object, "deck_count", radios_player_topology_count(topology));
    if (*topology->master_name) json_object_set_string_member(names, "0", topology->master_name);
    for (guint deck = 0; deck < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++deck) if (topology->players[deck].enabled) {
        const RadioSPlayerDefinition *player = &topology->players[deck]; gchar key[4]; g_snprintf(key, sizeof key, "%u", deck + 1);
        json_array_add_int_element(slots, deck);
        json_object_set_string_member(names, key, player->name);
        JsonObject *settings = json_object_new();
        json_object_set_string_member(settings, "cluster_id", player->cluster_id);
        json_object_set_string_member(settings, "automation_id", player->automation_id);
        json_object_set_int_member(settings, "autodj_mode", player->autodj_mode);
        json_object_set_int_member(settings, "autodj_target", player->autodj_target + 1);
        json_object_set_object_member(players, key, settings);
    }
    for (guint i = 0; i < topology->cluster_count; ++i) {
        const RadioSPlayerCluster *cluster = &topology->clusters[i]; JsonObject *entry = json_object_new();
        json_object_set_string_member(entry, "id", cluster->id); json_object_set_string_member(entry, "name", cluster->name);
        json_object_set_string_member(entry, "automation_id", cluster->automation_id); json_array_add_object_element(clusters, entry);
    }
    json_object_set_array_member(object, "slots", slots); json_object_set_object_member(object, "names", names);
    json_object_set_array_member(object, "clusters", clusters); json_object_set_object_member(object, "players", players);
    return object;
}
