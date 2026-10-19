#include "radios/routing_graph.h"
#include <math.h>
#include <string.h>

static const gchar *const kind_names[] = {"mixer", "input", "microphone", "virtual", "main",
                                          "output", "recorder", "stream", "meter"};
static const gchar *const kind_titles[] = {"Mikser girişi", "Ses girişi", "Mikrofon", "Sanal ses arayüzü",
                                           "MAIN yayını", "Ses çıkışı", "Kaydedici", "İnternet yayını", "Seviye ölçer"};
static const gchar *const protocol_names[] = {"icecast", "shoutcast1", "shoutcast2"};
static const gchar *const metadata_source_names[] = {"manual", "player", "automation"};
static const guint bitrates[] = {64, 96, 128, 160, 192, 256, 320};

static gboolean invalid(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, message); return FALSE; }
const gchar *radios_routing_graph_kind_name(RadioSRoutingGraphKind kind)
{ return kind >= RG_MIXER && kind < RG_KIND_COUNT ? kind_names[kind] : NULL; }
const gchar *radios_routing_graph_kind_title(RadioSRoutingGraphKind kind)
{ return kind >= RG_MIXER && kind < RG_KIND_COUNT ? kind_titles[kind] : ""; }
gboolean radios_routing_graph_kind_from_name(const gchar *name, RadioSRoutingGraphKind *kind)
{
    for (guint i = 0; name && i < RG_KIND_COUNT; ++i)
        if (g_str_equal(name, kind_names[i])) { if (kind) *kind = (RadioSRoutingGraphKind)i; return TRUE; }
    return FALSE;
}
const gchar *radios_routing_stream_protocol_name(RadioSRoutingStreamProtocol protocol)
{ return protocol >= RG_STREAM_ICECAST && protocol < RG_STREAM_PROTOCOL_COUNT ? protocol_names[protocol] : NULL; }
const gchar *radios_routing_metadata_source_name(RadioSRoutingMetadataSource source)
{ return source >= RG_METADATA_MANUAL && source < RG_METADATA_SOURCE_COUNT ? metadata_source_names[source] : NULL; }
gboolean radios_routing_graph_has_input(RadioSRoutingGraphKind kind)
{ return kind != RG_INPUT && kind != RG_MICROPHONE && kind < RG_KIND_COUNT; }
gboolean radios_routing_graph_has_output(RadioSRoutingGraphKind kind)
{ return kind != RG_OUTPUT && kind != RG_RECORDER && kind != RG_STREAM && kind < RG_KIND_COUNT; }
const gchar *radios_routing_graph_node_title(const RadioSRoutingGraphNode *node)
{ return !node ? "" : node->name[0] ? node->name : radios_routing_graph_kind_title(node->kind); }

const RadioSRoutingGraphNode *radios_routing_graph_find_const(const RadioSRoutingGraph *graph, guint id)
{
    if (!graph || graph->node_count > RADIOS_ROUTING_GRAPH_MAX_NODES) return NULL;
    for (guint i = 0; i < graph->node_count; ++i) if (graph->nodes[i].id == id) return &graph->nodes[i];
    return NULL;
}
RadioSRoutingGraphNode *radios_routing_graph_find(RadioSRoutingGraph *graph, guint id)
{ return (RadioSRoutingGraphNode *)radios_routing_graph_find_const(graph, id); }
static gint index_of(const RadioSRoutingGraph *graph, guint id)
{
    for (guint i = 0; i < graph->node_count; ++i) if (graph->nodes[i].id == id) return (gint)i;
    return -1;
}
gboolean radios_routing_graph_has_edge(const RadioSRoutingGraph *graph, guint from, guint to)
{
    for (guint i = 0; graph && i < graph->edge_count; ++i)
        if (graph->edges[i].from == from && graph->edges[i].to == to) return TRUE;
    return FALSE;
}
static gboolean position(gdouble x, gdouble y)
{ return isfinite(x) && isfinite(y) && x >= 0 && y >= 0 && x <= RADIOS_ROUTING_GRAPH_MAX_X && y <= RADIOS_ROUTING_GRAPH_MAX_Y; }
/* Bounded UTF-8 without control characters; `size` includes the terminator. */
static gboolean text(const gchar *value, gsize size)
{
    const gchar *end = memchr(value, '\0', size);
    if (!end || !g_utf8_validate(value, end - value, NULL)) return FALSE;
    for (const guchar *c = (const guchar *)value; *c; ++c) if (*c < 32 || *c == 127) return FALSE;
    return TRUE;
}
static gboolean bitrate(guint value)
{
    for (guint i = 0; i < G_N_ELEMENTS(bitrates); ++i) if (bitrates[i] == value) return TRUE;
    return FALSE;
}
void radios_routing_graph_node_defaults(RadioSRoutingGraphNode *node, RadioSRoutingGraphKind kind)
{
    g_return_if_fail(node); memset(node, 0, sizeof *node);
    node->kind = kind; node->channel = 1; node->split_minutes = 0;
    g_strlcpy(node->prefix, "kayit", sizeof node->prefix);
    node->protocol = RG_STREAM_ICECAST; node->port = 8000; node->stream_id = 1; node->bitrate_kbps = 128;
    node->metadata_source = RG_METADATA_MANUAL; node->metadata_player = 0;
    g_strlcpy(node->metadata_format, "{artist} - {title}", sizeof node->metadata_format);
    g_strlcpy(node->mount, "/canli", sizeof node->mount); g_strlcpy(node->username, "source", sizeof node->username);
    if (kind == RG_MICROPHONE) { g_strlcpy(node->device, RADIOS_ROUTING_TALK_MICROPHONE, sizeof node->device); node->push_to_talk = TRUE; }
}
static gboolean node_valid(const RadioSRoutingGraphNode *n, GError **error)
{
    if (!radios_routing_graph_kind_name(n->kind) || n->id < 1 || n->id > RADIOS_ROUTING_GRAPH_MAX_ID || !position(n->x, n->y))
        return invalid(error, "Blok kimliği, türü veya konumu geçersiz.");
    if (!text(n->name, sizeof n->name)) return invalid(error, "Blok adı en çok 160 bayt, denetim karakteri içermeyen metin olmalı.");
    if (!isfinite(n->gain_db) || n->gain_db < -60 || n->gain_db > 12) return invalid(error, "Blok kazancı -60..+12 dB olmalı.");
    switch (n->kind) {
    case RG_MIXER:
        if (n->channel < 1 || n->channel > 32) return invalid(error, "Mikser kanalı 1..32 olmalı.");
        break;
    case RG_INPUT: case RG_MICROPHONE: case RG_OUTPUT:
        if (!text(n->device, sizeof n->device)) return invalid(error, "Aygıt adı geçersiz.");
        if (n->kind != RG_MICROPHONE && !g_strcmp0(n->device, RADIOS_ROUTING_TALK_MICROPHONE))
            return invalid(error, "Konuşma mikrofonu yalnız Mikrofon bloğunda seçilebilir.");
        break;
    case RG_RECORDER:
        if (!text(n->folder, sizeof n->folder) || (n->folder[0] && !g_path_is_absolute(n->folder)))
            return invalid(error, "Kayıt klasörü boş ya da tam yol olmalı.");
        if (!text(n->prefix, sizeof n->prefix) || !n->prefix[0] || n->prefix[0] == '.' ||
            strchr(n->prefix, '/') || strchr(n->prefix, '\\'))
            return invalid(error, "Kayıt dosya adı öneki boş olamaz, '/' içeremez ve '.' ile başlayamaz.");
        if (n->split_minutes > 1440) return invalid(error, "Otomatik bölme 0..1440 dakika olmalı.");
        break;
    case RG_STREAM:
        if (!radios_routing_stream_protocol_name(n->protocol)) return invalid(error, "Yayın sunucusu türü geçersiz.");
        if (!text(n->host, sizeof n->host) || strpbrk(n->host, " /\\?#@")) return invalid(error, "Sunucu adresi yalnız ana makine adı ya da IP olmalı.");
        if (n->port < 1 || n->port > 65535) return invalid(error, "Sunucu portu 1..65535 olmalı.");
        if (!text(n->mount, sizeof n->mount) || (n->mount[0] && (n->mount[0] != '/' || strpbrk(n->mount, " ?#\\"))))
            return invalid(error, "Bağlama noktası '/' ile başlamalı ve boşluk, '?' ya da '#' içermemeli.");
        if (!text(n->username, sizeof n->username) || strchr(n->username, ':')) return invalid(error, "Kullanıcı adı ':' içeremez.");
        if (n->stream_id < 1 || n->stream_id > 999) return invalid(error, "SHOUTcast akış numarası 1..999 olmalı.");
        if (!bitrate(n->bitrate_kbps)) return invalid(error, "MP3 bit hızı 64, 96, 128, 160, 192, 256 ya da 320 kbps olmalı.");
        if (!text(n->station, sizeof n->station) || !text(n->genre, sizeof n->genre) ||
            !text(n->description, sizeof n->description) || !text(n->website, sizeof n->website))
            return invalid(error, "Yayın bilgileri geçersiz.");
        if (!text(n->password, sizeof n->password)) return invalid(error, "Yayın şifresi en çok 1024 bayt, denetim karakteri içermeyen metin olmalı.");
        if (!n->password_saved && n->password[0]) return invalid(error, "Yayın şifresi yalnız “Şifreyi kaydet” işaretliyken blokta saklanır.");
        if (!radios_routing_metadata_source_name(n->metadata_source) || n->metadata_player > 31 ||
            !text(n->metadata_format, sizeof n->metadata_format) || !text(n->metadata_manual, sizeof n->metadata_manual))
            return invalid(error, "Şarkı bilgisi kaynağı, player veya metin ayarları geçersiz.");
        break;
    default: break;
    }
    return TRUE;
}
static void virtual_names(const RadioSRoutingGraphNode *node, gchar sink[64], gchar monitor[80], gchar microphone[80])
{
    g_snprintf(sink, 64, "radios_virtual_%u", node->id);
    g_snprintf(monitor, 80, "%s.monitor", sink);
    g_snprintf(microphone, 80, "%s_mic", sink);
}
/* TRUE when a's input reaches b's output outside the drawn cables: through
 * the mixer, or a device that b records from. */
static gboolean implicit(const RadioSRoutingGraphNode *a, const RadioSRoutingGraphNode *b)
{
    if (a == b) return FALSE;
    if ((a->kind == RG_MIXER || a->kind == RG_MAIN) && b->kind == RG_MAIN) return TRUE;
    if (a->kind == RG_MIXER && b->kind == RG_MIXER && a->channel == b->channel) return TRUE;
    gchar sink[64], monitor[80], microphone[80];
    if (a->kind == RG_VIRTUAL) virtual_names(a, sink, monitor, microphone);
    else if (a->kind == RG_OUTPUT && a->device[0]) {
        g_strlcpy(sink, a->device, sizeof sink); g_snprintf(monitor, sizeof monitor, "%s.monitor", a->device); microphone[0] = 0;
        if (b->kind == RG_VIRTUAL) {
            gchar target[64], unused[80], unused_mic[80]; virtual_names(b, target, unused, unused_mic);
            if (g_str_equal(a->device, target)) return TRUE;
        }
    } else return FALSE;
    if (b->kind != RG_INPUT && b->kind != RG_MICROPHONE) return FALSE;
    return g_str_equal(b->device, monitor) || (a->kind == RG_VIRTUAL && g_str_equal(b->device, microphone));
}
/* Each block has an input vertex (2i) and an output vertex (2i+1). Cables
 * join an output to an input; pass-through blocks, the mixer, MAIN and
 * virtual interfaces carry their input to their own output; implicit links
 * carry an input to another block's output. A loop is a cycle here. */
static gboolean passes_through(RadioSRoutingGraphKind kind)
{ return kind == RG_METER || kind == RG_MIXER || kind == RG_MAIN || kind == RG_VIRTUAL; }
static gboolean linked(const RadioSRoutingGraph *graph, guint from, guint to)
{
    const RadioSRoutingGraphNode *a = &graph->nodes[from / 2], *b = &graph->nodes[to / 2];
    gboolean from_output = from % 2, to_output = to % 2;
    if (from_output && !to_output) return radios_routing_graph_has_edge(graph, a->id, b->id);
    if (!from_output && to_output) return a == b ? passes_through(a->kind) : implicit(a, b);
    return FALSE;
}
static gboolean cyclic(const RadioSRoutingGraph *graph, guint vertex, guint8 *state)
{
    state[vertex] = 1;
    for (guint next = 0; next < graph->node_count * 2; ++next) {
        if (!linked(graph, vertex, next)) continue;
        if (state[next] == 1 || (state[next] == 0 && cyclic(graph, next, state))) return TRUE;
    }
    state[vertex] = 2; return FALSE;
}
gboolean radios_routing_graph_validate(const RadioSRoutingGraph *graph, GError **error)
{
    if (!graph || graph->node_count > RADIOS_ROUTING_GRAPH_MAX_NODES || graph->edge_count > RADIOS_ROUTING_GRAPH_MAX_EDGES)
        return invalid(error, "Yönlendirmenin blok veya bağlantı sınırı aşıldı.");
    guint counts[RG_KIND_COUNT] = {0};
    for (guint i = 0; i < graph->node_count; ++i) {
        const RadioSRoutingGraphNode *n = &graph->nodes[i];
        if (!node_valid(n, error)) return FALSE;
        for (guint j = 0; j < i; ++j) if (graph->nodes[j].id == n->id) return invalid(error, "Blok kimlikleri benzersiz olmalı.");
        ++counts[n->kind];
    }
    if (counts[RG_INPUT] > RADIOS_ROUTING_GRAPH_MAX_DEVICES || counts[RG_MICROPHONE] > RADIOS_ROUTING_GRAPH_MAX_DEVICES ||
        counts[RG_OUTPUT] > RADIOS_ROUTING_GRAPH_MAX_DEVICES || counts[RG_VIRTUAL] > RADIOS_ROUTING_GRAPH_MAX_DEVICES)
        return invalid(error, "Her aygıt bloğu türünden en çok 8 tane kullanılabilir.");
    if (counts[RG_RECORDER] > RADIOS_ROUTING_GRAPH_MAX_RECORDERS) return invalid(error, "En çok 8 kaydedici kullanılabilir.");
    if (counts[RG_STREAM] > RADIOS_ROUTING_GRAPH_MAX_STREAMS) return invalid(error, "En çok 4 internet yayını kullanılabilir.");
    for (guint i = 0; i < graph->edge_count; ++i) {
        const RadioSRoutingGraphEdge *e = &graph->edges[i];
        const RadioSRoutingGraphNode *from = radios_routing_graph_find_const(graph, e->from), *to = radios_routing_graph_find_const(graph, e->to);
        if (!from || !to) return invalid(error, "Bağlantının kaynak veya hedef bloğu bulunamadı.");
        if (from == to) return invalid(error, "Bir blok kendine bağlanamaz.");
        if (!radios_routing_graph_has_output(from->kind)) return invalid(error, "Bu bloğun ses çıkışı yok.");
        if (!radios_routing_graph_has_input(to->kind)) return invalid(error, "Hedef bloğun ses girişi yok.");
        for (guint j = 0; j < i; ++j)
            if (graph->edges[j].from == e->from && graph->edges[j].to == e->to) return invalid(error, "Aynı bağlantı iki kez eklenemez.");
    }
    guint8 state[RADIOS_ROUTING_GRAPH_MAX_NODES * 2] = {0};
    for (guint i = 0; i < graph->node_count * 2; ++i)
        if (!state[i] && cyclic(graph, i, state))
            return invalid(error, "Bu bağlantı geri besleme döngüsü oluşturur: ses kendi girişine geri döner.");
    return TRUE;
}
gboolean radios_routing_graph_order(const RadioSRoutingGraph *graph, guint order[RADIOS_ROUTING_GRAPH_MAX_NODES])
{
    if (!radios_routing_graph_validate(graph, NULL)) return FALSE;
    guint pending[RADIOS_ROUTING_GRAPH_MAX_NODES] = {0}, count = 0;
    for (guint e = 0; e < graph->edge_count; ++e) ++pending[index_of(graph, graph->edges[e].to)];
    gboolean done[RADIOS_ROUTING_GRAPH_MAX_NODES] = {0};
    while (count < graph->node_count) {
        guint before = count;
        for (guint i = 0; i < graph->node_count; ++i) {
            if (done[i] || pending[i]) continue;
            done[i] = TRUE; order[count++] = graph->nodes[i].id;
            for (guint e = 0; e < graph->edge_count; ++e)
                if (graph->edges[e].from == graph->nodes[i].id) --pending[index_of(graph, graph->edges[e].to)];
        }
        if (count == before) return FALSE;
    }
    return TRUE;
}
guint radios_routing_graph_upstream(const RadioSRoutingGraph *graph, guint id, guint *sources, guint capacity)
{
    if (!graph || index_of(graph, id) < 0) return 0;
    gboolean seen[RADIOS_ROUTING_GRAPH_MAX_NODES] = {0}; guint stack[RADIOS_ROUTING_GRAPH_MAX_NODES], depth = 0, found = 0;
    stack[depth++] = (guint)index_of(graph, id); seen[stack[0]] = TRUE;
    while (depth) {
        guint node = stack[--depth]; gboolean fed = FALSE;
        for (guint e = 0; e < graph->edge_count; ++e) {
            if (graph->edges[e].to != graph->nodes[node].id) continue;
            gint from = index_of(graph, graph->edges[e].from); fed = TRUE;
            if (from >= 0 && !seen[from] && depth < RADIOS_ROUTING_GRAPH_MAX_NODES) { seen[from] = TRUE; stack[depth++] = (guint)from; }
        }
        RadioSRoutingGraphKind kind = graph->nodes[node].kind;
        gboolean source = !radios_routing_graph_has_input(kind) || kind == RG_MIXER || kind == RG_MAIN || kind == RG_VIRTUAL;
        if ((source || !fed) && radios_routing_graph_has_output(kind) && found < capacity) sources[found++] = graph->nodes[node].id;
    }
    return found;
}
static guint next_id(const RadioSRoutingGraph *graph)
{
    for (guint id = 1; id <= RADIOS_ROUTING_GRAPH_MAX_ID; ++id) if (index_of(graph, id) < 0) return id;
    return 0;
}
static RadioSRoutingGraphNode *append(RadioSRoutingGraph *graph, RadioSRoutingGraphKind kind, gdouble x, gdouble y)
{
    RadioSRoutingGraphNode *node = &graph->nodes[graph->node_count];
    radios_routing_graph_node_defaults(node, kind); node->id = next_id(graph); node->x = x; node->y = y;
    ++graph->node_count; return node;
}
void radios_routing_graph_default(RadioSRoutingGraph *graph)
{
    g_return_if_fail(graph); memset(graph, 0, sizeof *graph);
    append(graph, RG_MAIN, 40, 60); append(graph, RG_METER, 300, 60);
    append(graph, RG_RECORDER, 560, 20); append(graph, RG_STREAM, 560, 190);
    graph->edges[graph->edge_count++] = (RadioSRoutingGraphEdge){1, 2};
    graph->edges[graph->edge_count++] = (RadioSRoutingGraphEdge){2, 3};
    graph->edges[graph->edge_count++] = (RadioSRoutingGraphEdge){2, 4};
}
/* Every mutation validates a heap candidate; the graph is unchanged on error. */
static gboolean commit(RadioSRoutingGraph *graph, RadioSRoutingGraph *candidate, GError **error)
{
    gboolean valid = radios_routing_graph_validate(candidate, error);
    if (valid) *graph = *candidate;
    g_free(candidate); return valid;
}
static RadioSRoutingGraph *candidate_of(const RadioSRoutingGraph *graph, GError **error)
{
    if (!radios_routing_graph_validate(graph, error)) return NULL;
    return g_memdup2(graph, sizeof *graph);
}
gboolean radios_routing_graph_add(RadioSRoutingGraph *graph, RadioSRoutingGraphKind kind, gdouble x, gdouble y, guint *id, GError **error)
{
    if (!radios_routing_graph_kind_name(kind) || !position(x, y)) return invalid(error, "Blok türü veya konumu geçersiz.");
    RadioSRoutingGraph *candidate = candidate_of(graph, error); if (!candidate) return FALSE;
    if (candidate->node_count == RADIOS_ROUTING_GRAPH_MAX_NODES) { g_free(candidate); return invalid(error, "Blok sınırına (48) ulaşıldı."); }
    guint added = append(candidate, kind, x, y)->id;
    if (!commit(graph, candidate, error)) return FALSE;
    if (id) *id = added;
    return TRUE;
}
gboolean radios_routing_graph_update(RadioSRoutingGraph *graph, const RadioSRoutingGraphNode *node, GError **error)
{
    if (!node) return invalid(error, "Blok bulunamadı.");
    RadioSRoutingGraph *candidate = candidate_of(graph, error); if (!candidate) return FALSE;
    RadioSRoutingGraphNode *target = radios_routing_graph_find(candidate, node->id);
    if (!target || target->kind != node->kind) { g_free(candidate); return invalid(error, "Güncellenecek blok bulunamadı."); }
    gdouble x = target->x, y = target->y; *target = *node; target->x = x; target->y = y;
    return commit(graph, candidate, error);
}
gboolean radios_routing_graph_rename(RadioSRoutingGraph *graph, guint id, const gchar *name, GError **error)
{
    const RadioSRoutingGraphNode *node = radios_routing_graph_find_const(graph, id);
    if (!node || !name || strlen(name) >= RADIOS_ROUTING_GRAPH_NAME_SIZE) return invalid(error, "Blok adı en çok 160 bayt olmalı.");
    RadioSRoutingGraphNode copy = *node; gchar *trimmed = g_strstrip(g_strdup(name));
    g_strlcpy(copy.name, trimmed, sizeof copy.name); g_free(trimmed);
    return radios_routing_graph_update(graph, &copy, error);
}
gboolean radios_routing_graph_remove(RadioSRoutingGraph *graph, guint id, GError **error)
{
    RadioSRoutingGraph *candidate = candidate_of(graph, error); if (!candidate) return FALSE;
    gint found = index_of(candidate, id);
    if (found < 0) { g_free(candidate); return invalid(error, "Silinecek blok bulunamadı."); }
    for (guint i = 0; i < candidate->edge_count;) {
        if (candidate->edges[i].from != id && candidate->edges[i].to != id) { ++i; continue; }
        memmove(&candidate->edges[i], &candidate->edges[i + 1], (--candidate->edge_count - i) * sizeof candidate->edges[0]);
        memset(&candidate->edges[candidate->edge_count], 0, sizeof candidate->edges[0]);
    }
    memmove(&candidate->nodes[found], &candidate->nodes[found + 1], (--candidate->node_count - (guint)found) * sizeof candidate->nodes[0]);
    memset(&candidate->nodes[candidate->node_count], 0, sizeof candidate->nodes[0]);
    return commit(graph, candidate, error);
}
gboolean radios_routing_graph_move(RadioSRoutingGraph *graph, guint id, gdouble x, gdouble y, GError **error)
{
    RadioSRoutingGraphNode *node = radios_routing_graph_find(graph, id);
    if (!node || !position(x, y)) return invalid(error, "Taşınacak blok veya konumu geçersiz.");
    node->x = x; node->y = y; return TRUE;
}
gboolean radios_routing_graph_connect(RadioSRoutingGraph *graph, guint from, guint to, GError **error)
{
    RadioSRoutingGraph *candidate = candidate_of(graph, error); if (!candidate) return FALSE;
    if (candidate->edge_count == RADIOS_ROUTING_GRAPH_MAX_EDGES) { g_free(candidate); return invalid(error, "Bağlantı sınırına (128) ulaşıldı."); }
    candidate->edges[candidate->edge_count++] = (RadioSRoutingGraphEdge){from, to};
    return commit(graph, candidate, error);
}
gboolean radios_routing_graph_disconnect(RadioSRoutingGraph *graph, guint from, guint to, GError **error)
{
    for (guint i = 0; graph && i < graph->edge_count; ++i) {
        if (graph->edges[i].from != from || graph->edges[i].to != to) continue;
        memmove(&graph->edges[i], &graph->edges[i + 1], (--graph->edge_count - i) * sizeof graph->edges[0]);
        memset(&graph->edges[graph->edge_count], 0, sizeof graph->edges[0]); return TRUE;
    }
    return invalid(error, "Silinecek bağlantı bulunamadı.");
}

/* JSON */
static gboolean exact_keys(JsonObject *object, const gchar *const *keys, guint count)
{
    if (!object || json_object_get_size(object) != count) return FALSE;
    for (guint i = 0; i < count; ++i) if (!json_object_has_member(object, keys[i])) return FALSE;
    return TRUE;
}
/* Required settings stay exact; optional fields preserve older documents. */
static gboolean keys_with_optional(JsonObject *object, const gchar *const *required, guint required_count,
    const gchar *const *optional, guint optional_count)
{
    if (!object) return FALSE;
    for (guint i = 0; i < required_count; ++i) if (!json_object_has_member(object, required[i])) return FALSE;
    guint known_count = required_count;
    for (guint i = 0; i < optional_count; ++i) if (json_object_has_member(object, optional[i])) ++known_count;
    return json_object_get_size(object) == known_count;
}
static gboolean int_member(JsonObject *object, const gchar *name, gint64 low, gint64 high, guint *value)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) return FALSE;
    gint64 n = json_node_get_int(node); if (n < low || n > high) return FALSE;
    *value = (guint)n; return TRUE;
}
static gboolean number_member(JsonObject *object, const gchar *name, gdouble *value)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        (json_node_get_value_type(node) != G_TYPE_INT64 && json_node_get_value_type(node) != G_TYPE_DOUBLE)) return FALSE;
    *value = json_node_get_double(node); return isfinite(*value);
}
static gboolean bool_member(JsonObject *object, const gchar *name, gboolean *value)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN) return FALSE;
    *value = json_node_get_boolean(node); return TRUE;
}
static gboolean string_member(JsonObject *object, const gchar *name, gchar *value, gsize size)
{
    JsonNode *node = json_object_get_member(object, name);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) return FALSE;
    const gchar *s = json_node_get_string(node);
    return s && strlen(s) < size && g_strlcpy(value, s, size) < size;
}
static gboolean settings_from_json(RadioSRoutingGraphNode *n, JsonObject *s)
{
    static const gchar *const mixer[] = {"channel"}, *const device[] = {"device"}, *const microphone[] = {"device", "push_to_talk"},
        *const virtual_keys[] = {"as_microphone"}, *const recorder[] = {"folder", "prefix", "split_minutes"},
        *const stream[] = {"protocol", "host", "port", "mount", "stream_id", "username", "bitrate_kbps", "public",
                           "station", "genre", "description", "website"},
        *const stream_optional[] = {"metadata_source", "metadata_player", "metadata_format", "metadata_manual",
                                    "password_saved", "password"};
    switch (n->kind) {
    case RG_MIXER: return exact_keys(s, mixer, 1) && int_member(s, "channel", 1, 32, &n->channel);
    case RG_INPUT: case RG_OUTPUT: return exact_keys(s, device, 1) && string_member(s, "device", n->device, sizeof n->device);
    case RG_MICROPHONE: return exact_keys(s, microphone, 2) && string_member(s, "device", n->device, sizeof n->device) &&
        bool_member(s, "push_to_talk", &n->push_to_talk);
    case RG_VIRTUAL: return exact_keys(s, virtual_keys, 1) && bool_member(s, "as_microphone", &n->as_microphone);
    case RG_RECORDER: return exact_keys(s, recorder, 3) && string_member(s, "folder", n->folder, sizeof n->folder) &&
        string_member(s, "prefix", n->prefix, sizeof n->prefix) && int_member(s, "split_minutes", 0, 1440, &n->split_minutes);
    case RG_STREAM: {
        gchar protocol[16] = "";
        if (!keys_with_optional(s, stream, G_N_ELEMENTS(stream), stream_optional, G_N_ELEMENTS(stream_optional)) ||
            !string_member(s, "protocol", protocol, sizeof protocol)) return FALSE;
        if (json_object_has_member(s, "metadata_source")) {
            gchar source[16] = ""; gboolean source_known = FALSE;
            if (!string_member(s, "metadata_source", source, sizeof source)) return FALSE;
            for (guint i = 0; i < RG_METADATA_SOURCE_COUNT; ++i)
                if (g_str_equal(source, metadata_source_names[i])) { n->metadata_source = (RadioSRoutingMetadataSource)i; source_known = TRUE; }
            if (!source_known) return FALSE;
        }
        if ((json_object_has_member(s, "metadata_player") && !int_member(s, "metadata_player", 0, 31, &n->metadata_player)) ||
            (json_object_has_member(s, "metadata_format") && !string_member(s, "metadata_format", n->metadata_format, sizeof n->metadata_format)) ||
            (json_object_has_member(s, "metadata_manual") && !string_member(s, "metadata_manual", n->metadata_manual, sizeof n->metadata_manual))) return FALSE;
        /* A stored password is only ever an explicit choice: the flag must be
         * present and TRUE, and the password must accompany the flag. */
        if (json_object_has_member(s, "password_saved") && !bool_member(s, "password_saved", &n->password_saved)) return FALSE;
        if (json_object_has_member(s, "password") != (n->password_saved != FALSE)) return FALSE;
        if (n->password_saved && !string_member(s, "password", n->password, sizeof n->password)) return FALSE;
        gboolean known = FALSE;
        for (guint i = 0; i < RG_STREAM_PROTOCOL_COUNT; ++i)
            if (g_str_equal(protocol, protocol_names[i])) { n->protocol = (RadioSRoutingStreamProtocol)i; known = TRUE; }
        return known && string_member(s, "host", n->host, sizeof n->host) && int_member(s, "port", 1, 65535, &n->port) &&
            string_member(s, "mount", n->mount, sizeof n->mount) && int_member(s, "stream_id", 1, 999, &n->stream_id) &&
            string_member(s, "username", n->username, sizeof n->username) && int_member(s, "bitrate_kbps", 64, 320, &n->bitrate_kbps) &&
            bool_member(s, "public", &n->public_listing) && string_member(s, "station", n->station, sizeof n->station) &&
            string_member(s, "genre", n->genre, sizeof n->genre) && string_member(s, "description", n->description, sizeof n->description) &&
            string_member(s, "website", n->website, sizeof n->website);
    }
    default: return json_object_get_size(s) == 0;
    }
}
gboolean radios_routing_graph_from_json(RadioSRoutingGraph *graph, JsonObject *object, GError **error)
{
    static const gchar *const graph_keys[] = {"nodes", "edges"},
        *const node_keys[] = {"id", "kind", "name", "x", "y", "gain_db", "settings"}, *const edge_keys[] = {"from", "to"};
    if (!graph || !exact_keys(object, graph_keys, 2)) return invalid(error, "Yönlendirme yalnız nodes ve edges alanlarını taşımalıdır.");
    JsonNode *nodes_node = json_object_get_member(object, "nodes"), *edges_node = json_object_get_member(object, "edges");
    if (!JSON_NODE_HOLDS_ARRAY(nodes_node) || !JSON_NODE_HOLDS_ARRAY(edges_node)) return invalid(error, "Bloklar ve bağlantılar liste olmalıdır.");
    JsonArray *nodes = json_node_get_array(nodes_node), *edges = json_node_get_array(edges_node);
    if (json_array_get_length(nodes) > RADIOS_ROUTING_GRAPH_MAX_NODES || json_array_get_length(edges) > RADIOS_ROUTING_GRAPH_MAX_EDGES)
        return invalid(error, "Yönlendirme listeleri sınırı aştı.");
    RadioSRoutingGraph *candidate = g_new0(RadioSRoutingGraph, 1);
    candidate->node_count = json_array_get_length(nodes); candidate->edge_count = json_array_get_length(edges);
    for (guint i = 0; i < candidate->node_count; ++i) {
        JsonNode *entry = json_array_get_element(nodes, i);
        JsonObject *node = JSON_NODE_HOLDS_OBJECT(entry) ? json_node_get_object(entry) : NULL;
        RadioSRoutingGraphNode *out = &candidate->nodes[i]; gchar kind[24] = "";
        JsonNode *settings = node ? json_object_get_member(node, "settings") : NULL;
        if (!exact_keys(node, node_keys, G_N_ELEMENTS(node_keys)) || !string_member(node, "kind", kind, sizeof kind) ||
            !radios_routing_graph_kind_from_name(kind, &out->kind) || !JSON_NODE_HOLDS_OBJECT(settings)) {
            g_free(candidate); return invalid(error, "Blok alanları geçersiz.");
        }
        radios_routing_graph_node_defaults(out, out->kind);
        if (!int_member(node, "id", 1, RADIOS_ROUTING_GRAPH_MAX_ID, &out->id) || !string_member(node, "name", out->name, sizeof out->name) ||
            !number_member(node, "x", &out->x) || !number_member(node, "y", &out->y) || !number_member(node, "gain_db", &out->gain_db) ||
            !settings_from_json(out, json_node_get_object(settings))) {
            g_free(candidate); return invalid(error, "Blok ayarları geçersiz.");
        }
    }
    for (guint i = 0; i < candidate->edge_count; ++i) {
        JsonNode *entry = json_array_get_element(edges, i);
        JsonObject *edge = JSON_NODE_HOLDS_OBJECT(entry) ? json_node_get_object(entry) : NULL;
        if (!exact_keys(edge, edge_keys, 2) || !int_member(edge, "from", 1, RADIOS_ROUTING_GRAPH_MAX_ID, &candidate->edges[i].from) ||
            !int_member(edge, "to", 1, RADIOS_ROUTING_GRAPH_MAX_ID, &candidate->edges[i].to)) {
            g_free(candidate); return invalid(error, "Bağlantı alanları geçersiz.");
        }
    }
    return commit(graph, candidate, error);
}
static JsonObject *settings_to_json(const RadioSRoutingGraphNode *n)
{
    JsonObject *s = json_object_new();
    switch (n->kind) {
    case RG_MIXER: json_object_set_int_member(s, "channel", n->channel); break;
    case RG_INPUT: case RG_OUTPUT: json_object_set_string_member(s, "device", n->device); break;
    case RG_MICROPHONE:
        json_object_set_string_member(s, "device", n->device); json_object_set_boolean_member(s, "push_to_talk", n->push_to_talk); break;
    case RG_VIRTUAL: json_object_set_boolean_member(s, "as_microphone", n->as_microphone); break;
    case RG_RECORDER:
        json_object_set_string_member(s, "folder", n->folder); json_object_set_string_member(s, "prefix", n->prefix);
        json_object_set_int_member(s, "split_minutes", n->split_minutes); break;
    case RG_STREAM:
        json_object_set_string_member(s, "protocol", protocol_names[n->protocol]);
        json_object_set_string_member(s, "host", n->host); json_object_set_int_member(s, "port", n->port);
        json_object_set_string_member(s, "mount", n->mount); json_object_set_int_member(s, "stream_id", n->stream_id);
        json_object_set_string_member(s, "username", n->username); json_object_set_int_member(s, "bitrate_kbps", n->bitrate_kbps);
        json_object_set_boolean_member(s, "public", n->public_listing);
        json_object_set_string_member(s, "station", n->station); json_object_set_string_member(s, "genre", n->genre);
        json_object_set_string_member(s, "description", n->description); json_object_set_string_member(s, "website", n->website);
        json_object_set_string_member(s, "metadata_source", metadata_source_names[n->metadata_source]);
        json_object_set_int_member(s, "metadata_player", n->metadata_player);
        json_object_set_string_member(s, "metadata_format", n->metadata_format);
        json_object_set_string_member(s, "metadata_manual", n->metadata_manual);
        if (n->password_saved) {
            json_object_set_boolean_member(s, "password_saved", TRUE);
            json_object_set_string_member(s, "password", n->password);
        }
        break;
    default: break;
    }
    return s;
}
JsonObject *radios_routing_graph_to_json(const RadioSRoutingGraph *graph)
{
    if (!radios_routing_graph_validate(graph, NULL)) return NULL;
    JsonObject *object = json_object_new(); JsonArray *nodes = json_array_new(), *edges = json_array_new();
    for (guint i = 0; i < graph->node_count; ++i) {
        const RadioSRoutingGraphNode *n = &graph->nodes[i]; JsonObject *node = json_object_new();
        json_object_set_int_member(node, "id", n->id); json_object_set_string_member(node, "kind", kind_names[n->kind]);
        json_object_set_string_member(node, "name", n->name);
        json_object_set_double_member(node, "x", n->x); json_object_set_double_member(node, "y", n->y);
        json_object_set_double_member(node, "gain_db", n->gain_db);
        json_object_set_object_member(node, "settings", settings_to_json(n));
        json_array_add_object_element(nodes, node);
    }
    for (guint i = 0; i < graph->edge_count; ++i) {
        JsonObject *edge = json_object_new(); json_object_set_int_member(edge, "from", graph->edges[i].from);
        json_object_set_int_member(edge, "to", graph->edges[i].to); json_array_add_object_element(edges, edge);
    }
    json_object_set_array_member(object, "nodes", nodes); json_object_set_array_member(object, "edges", edges); return object;
}
static const gchar *legacy_string(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : "";
}
static gdouble legacy_number(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    gdouble value = node && JSON_NODE_HOLDS_VALUE(node) &&
        (json_node_get_value_type(node) == G_TYPE_DOUBLE || json_node_get_value_type(node) == G_TYPE_INT64) ? json_node_get_double(node) : 0;
    return isfinite(value) ? CLAMP(value, -60, 12) : 0;
}
void radios_routing_graph_from_legacy(RadioSRoutingGraph *graph, JsonObject *config)
{
    g_return_if_fail(graph);
    /* Schema 1 ids were kind + 1: 1 application, 2 microphone, 3 compressor,
     * 4 output, 5 MAIN, 6 recorder. Application and compressor are gone; a
     * compressed microphone path becomes a direct microphone cable. */
    static const RadioSRoutingGraphKind kinds[] = {RG_KIND_COUNT, RG_MICROPHONE, RG_KIND_COUNT, RG_OUTPUT, RG_MAIN, RG_RECORDER};
    RadioSRoutingGraph *result = g_new0(RadioSRoutingGraph, 1);
    JsonNode *graph_node = config ? json_object_get_member(config, "graph") : NULL;
    JsonObject *old = graph_node && JSON_NODE_HOLDS_OBJECT(graph_node) ? json_node_get_object(graph_node) : NULL;
    JsonArray *nodes = old && json_object_has_member(old, "nodes") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(old, "nodes")) ?
        json_object_get_array_member(old, "nodes") : NULL;
    JsonArray *edges = old && json_object_has_member(old, "edges") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(old, "edges")) ?
        json_object_get_array_member(old, "edges") : NULL;
    gboolean present[7] = {0};
    for (guint i = 0; nodes && i < json_array_get_length(nodes); ++i) {
        JsonNode *entry = json_array_get_element(nodes, i);
        JsonObject *node = JSON_NODE_HOLDS_OBJECT(entry) ? json_node_get_object(entry) : NULL;
        guint id = 0; gdouble x = 0, y = 0;
        if (!node || !int_member(node, "id", 1, 6, &id) || kinds[id - 1] == RG_KIND_COUNT || present[id]) continue;
        number_member(node, "x", &x); number_member(node, "y", &y);
        RadioSRoutingGraphNode *n = &result->nodes[result->node_count++];
        radios_routing_graph_node_defaults(n, kinds[id - 1]); n->id = id;
        n->x = isfinite(x) ? CLAMP(x, 0, RADIOS_ROUTING_GRAPH_MAX_X) : 0; n->y = isfinite(y) ? CLAMP(y, 0, RADIOS_ROUTING_GRAPH_MAX_Y) : 0;
        if (n->kind == RG_MICROPHONE) {
            g_strlcpy(n->device, legacy_string(config, "microphone_source"), sizeof n->device);
            n->push_to_talk = FALSE; n->gain_db = legacy_number(config, "microphone_gain_db");
        } else if (n->kind == RG_OUTPUT) g_strlcpy(n->device, legacy_string(config, "output_sink"), sizeof n->device);
        present[id] = TRUE;
    }
    if (!nodes) { radios_routing_graph_default(result); *graph = *result; g_free(result); return; }
    gboolean compressed_in = FALSE, compressed_out = FALSE;
    for (guint i = 0; edges && i < json_array_get_length(edges); ++i) {
        JsonNode *entry = json_array_get_element(edges, i);
        JsonObject *edge = JSON_NODE_HOLDS_OBJECT(entry) ? json_node_get_object(entry) : NULL;
        guint from = 0, to = 0;
        if (!edge || !int_member(edge, "from", 1, 6, &from) || !int_member(edge, "to", 1, 6, &to)) continue;
        if (from == 2 && to == 3) compressed_in = TRUE;
        if (from == 3 && to == 6) compressed_out = TRUE;
        if (from == 2 && to == 6 && present[2] && present[6] && !radios_routing_graph_has_edge(result, 2, 6))
            result->edges[result->edge_count++] = (RadioSRoutingGraphEdge){2, 6};
    }
    if (compressed_in && compressed_out && present[2] && present[6] && !radios_routing_graph_has_edge(result, 2, 6))
        result->edges[result->edge_count++] = (RadioSRoutingGraphEdge){2, 6};
    if (!radios_routing_graph_validate(result, NULL)) radios_routing_graph_default(result);
    *graph = *result; g_free(result);
}
