#include "radios/programme_model.h"
#include <string.h>

/* "live": a live event step (microphone/DJ talk) of the broadcast flow; it
 * plays no media and ends by its own rule (operator, duration or schema). */
static const gchar *const kinds[] = {"music", "announcement", "advertisement", "jingle", "program",
    "sweeper", "news", "effect", "voice_track", "other", "live"};
static const gchar *const labels[] = {"Müzik", "Anons", "Reklam", "Jingle", "Program",
    "Sweeper", "Haber", "Efekt", "Ses kaydı", "Diğer", "Canlı"};
static const gchar *const event_titles[] = {"Müzik eventi", "Anons eventi", "Reklam eventi", "Jingle eventi",
    "Program eventi", "Sweeper eventi", "Haber eventi", "Efekt eventi", "Ses kaydı eventi", "Diğer event", "Live event"};

static gint kind_index(const gchar *kind)
{
    for (guint i = 0; kind && i < G_N_ELEMENTS(kinds); ++i) if (g_str_equal(kind, kinds[i])) return (gint)i;
    return -1;
}
gboolean radios_programme_kind_valid(const gchar *kind) { return kind_index(kind) >= 0; }
const gchar *radios_programme_kind_label(const gchar *kind) { gint i = kind_index(kind); return labels[MAX(i, 0)]; }
const gchar *radios_programme_event_title(const gchar *kind) { gint i = kind_index(kind); return event_titles[MAX(i, 0)]; }

static const gchar *text(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : "";
}
static gint64 number(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 ? json_node_get_int(node) : 0;
}
static JsonArray *array(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

/* json_node_copy() shares objects and arrays; drafts are edited as copies. */
JsonNode *radios_programme_clone(JsonNode *node)
{
    if (!node) return NULL;
    if (JSON_NODE_HOLDS_OBJECT(node)) {
        JsonNode *copy = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(copy, radios_programme_clone_object(json_node_get_object(node)));
        return copy;
    }
    if (JSON_NODE_HOLDS_ARRAY(node)) {
        JsonArray *source = json_node_get_array(node), *target = json_array_sized_new(json_array_get_length(source));
        for (guint i = 0; i < json_array_get_length(source); ++i)
            json_array_add_element(target, radios_programme_clone(json_array_get_element(source, i)));
        JsonNode *copy = json_node_new(JSON_NODE_ARRAY); json_node_take_array(copy, target); return copy;
    }
    return json_node_copy(node);
}
JsonObject *radios_programme_clone_object(JsonObject *object)
{
    JsonObject *copy = json_object_new();
    JsonObjectIter iter; const gchar *name; JsonNode *member;
    json_object_iter_init(&iter, object);
    while (json_object_iter_next(&iter, &name, &member)) json_object_set_member(copy, name, radios_programme_clone(member));
    return copy;
}

/* A name-based UUID, so a legacy event keeps one block identity across reloads. */
gchar *radios_programme_block_id_for(const gchar *event_id)
{
    gchar *seed = g_strconcat("radios-programme-block:", event_id ? event_id : "", NULL);
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA1); guint8 digest[20]; gsize length = sizeof digest;
    g_checksum_update(checksum, (const guchar *)seed, -1); g_checksum_get_digest(checksum, digest, &length);
    g_checksum_free(checksum); g_free(seed);
    digest[6] = (guint8)((digest[6] & 0x0f) | 0x50); digest[8] = (guint8)((digest[8] & 0x3f) | 0x80);
    return g_strdup_printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        digest[0], digest[1], digest[2], digest[3], digest[4], digest[5], digest[6], digest[7],
        digest[8], digest[9], digest[10], digest[11], digest[12], digest[13], digest[14], digest[15]);
}

static void block_free(gpointer data)
{
    RadioSProgrammeBlock *block = data;
    json_object_unref(block->block); g_ptr_array_unref(block->leaves); g_free(block);
}
static RadioSProgrammeBlock *block_new(JsonObject *object)
{
    RadioSProgrammeBlock *block = g_new0(RadioSProgrammeBlock, 1);
    block->block = object; block->leaves = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
    return block;
}
static JsonObject *block_object(const gchar *id, const gchar *kind, const gchar *title)
{
    JsonObject *object = json_object_new();
    json_object_set_string_member(object, "id", id);
    json_object_set_string_member(object, "kind", radios_programme_kind_valid(kind) ? kind : "music");
    json_object_set_string_member(object, "title", title && *title ? title : radios_programme_event_title(kind));
    json_object_set_string_member(object, "start_time", ""); json_object_set_string_member(object, "note", "");
    return object;
}

RadioSProgrammeTree *radios_programme_tree_new(JsonObject *config)
{
    RadioSProgrammeTree *tree = g_new0(RadioSProgrammeTree, 1);
    tree->blocks = g_ptr_array_new_with_free_func(block_free);
    GHashTable *index = g_hash_table_new(g_str_hash, g_str_equal);
    JsonArray *blocks = array(config, "blocks"), *events = array(config, "events");
    for (guint i = 0; blocks && i < json_array_get_length(blocks); ++i) {
        JsonNode *node = json_array_get_element(blocks, i);
        if (!JSON_NODE_HOLDS_OBJECT(node) || !*text(json_node_get_object(node), "id") ||
            g_hash_table_contains(index, text(json_node_get_object(node), "id"))) continue;
        RadioSProgrammeBlock *block = block_new(json_object_ref(json_node_get_object(node)));
        g_ptr_array_add(tree->blocks, block); g_hash_table_insert(index, (gpointer)text(block->block, "id"), block);
    }
    RadioSProgrammeBlock *implicit_music = NULL;
    for (guint i = 0; events && i < json_array_get_length(events); ++i) {
        JsonNode *node = json_array_get_element(events, i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) { implicit_music = NULL; continue; }
        JsonObject *event = json_node_get_object(node);
        const gchar *assigned = text(event, "block_id");
        RadioSProgrammeBlock *block = g_hash_table_lookup(index, assigned);
        gboolean implicit = !block && !*assigned;
        gboolean music = g_str_equal(text(event, "kind"), "music");
        /* An old flat music queue is one event until a different kind. A
         * declared block, including an operator-created empty/default block,
         * always retains its own boundary and metadata. */
        if (!implicit || !music) implicit_music = NULL;
        if (implicit && music && implicit_music) block = implicit_music;
        if (!block) {
            gchar *id = radios_programme_block_id_for(text(event, "id"));
            block = g_hash_table_lookup(index, id);
            if (!block) {
                block = block_new(block_object(id, text(event, "kind"), NULL));
                g_ptr_array_add(tree->blocks, block); g_hash_table_insert(index, (gpointer)text(block->block, "id"), block);
            }
            g_free(id);
        }
        g_ptr_array_add(block->leaves, json_object_ref(event));
        if (implicit && music) implicit_music = block;
    }
    g_hash_table_unref(index);
    return tree;
}

void radios_programme_tree_free(RadioSProgrammeTree *tree)
{
    if (!tree) return;
    g_ptr_array_unref(tree->blocks); g_free(tree);
}

JsonNode *radios_programme_tree_config(RadioSProgrammeTree *tree, JsonObject *config)
{
    JsonObject *next = radios_programme_clone_object(config);
    JsonArray *blocks = json_array_new(), *events = json_array_new();
    gboolean timed = FALSE; JsonNode *timing = json_object_get_member(next, "timing");
    const gchar *timed_id = timing && JSON_NODE_HOLDS_OBJECT(timing) ? text(json_node_get_object(timing), "event_id") : "";
    for (guint i = 0; i < tree->blocks->len; ++i) {
        RadioSProgrammeBlock *block = g_ptr_array_index(tree->blocks, i);
        json_array_add_object_element(blocks, radios_programme_clone_object(block->block));
        for (guint j = 0; j < block->leaves->len; ++j) {
            JsonObject *leaf = radios_programme_clone_object(g_ptr_array_index(block->leaves, j));
            /* Accepted-plan annotations are a view of this run, never a
             * persisted jingle rule or a future playback instruction. */
            json_object_remove_member(leaf, "planned_jingles");
            json_object_remove_member(leaf, "planned_jingle");
            json_object_set_string_member(leaf, "block_id", text(block->block, "id"));
            timed |= *timed_id && g_str_equal(timed_id, text(leaf, "id"));
            json_array_add_object_element(events, leaf);
        }
    }
    json_object_set_array_member(next, "blocks", blocks);
    json_object_set_array_member(next, "events", events);
    if (timing && !timed) json_object_remove_member(next, "timing");
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, next);
    return node;
}

gboolean radios_programme_tree_find(RadioSProgrammeTree *tree, const gchar *id, guint *block, gint *leaf)
{
    for (guint i = 0; id && *id && i < tree->blocks->len; ++i) {
        RadioSProgrammeBlock *candidate = g_ptr_array_index(tree->blocks, i);
        if (g_str_equal(id, text(candidate->block, "id"))) {
            if (block) *block = i;
            if (leaf) *leaf = -1;
            return TRUE;
        }
        for (guint j = 0; j < candidate->leaves->len; ++j)
            if (g_str_equal(id, text(g_ptr_array_index(candidate->leaves, j), "id"))) {
                if (block) *block = i;
                if (leaf) *leaf = (gint)j;
                return TRUE;
            }
    }
    return FALSE;
}

guint radios_programme_tree_leaf_count(RadioSProgrammeTree *tree)
{
    guint count = 0;
    for (guint i = 0; i < tree->blocks->len; ++i) count += ((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, i))->leaves->len;
    return count;
}

gboolean radios_programme_kind_attaches(const gchar *kind)
{ return kind && (g_str_equal(kind, "jingle") || g_str_equal(kind, "sweeper")); }
gchar *radios_programme_hint_id(JsonObject *leaf, JsonObject *hint, guint ordinal)
{
    const gchar *id = text(hint, "id");
    if (g_uuid_string_is_valid(id)) return g_strdup(id);
    if (!g_str_equal(text(hint, "placement"), "overlay") && *text(hint, "entry_id"))
        return g_strdup(text(hint, "entry_id"));
    gchar *seed = g_strdup_printf("%s:voice:%s:%s:%s:%u", text(leaf, "id"),
        text(hint, "entry_id"), text(hint, "placement"), text(hint, "title"), ordinal);
    gchar *result = radios_programme_block_id_for(seed); g_free(seed); return result;
}
guint radios_programme_leaf_content_count(JsonObject *leaf)
{
    guint count = 1;
    JsonArray *hints = array(leaf, "planned_jingles");
    for (guint i = 0; hints && i < json_array_get_length(hints); ++i) {
        JsonObject *hint = json_array_get_object_element(hints, i);
        if (g_str_equal(text(hint, "placement"), "overlay")) ++count;
    }
    return count;
}

gint radios_programme_tree_leaf_index(RadioSProgrammeTree *tree, const gchar *id)
{
    guint block = 0; gint leaf = -1, index = 0;
    if (!radios_programme_tree_find(tree, id, &block, &leaf)) return -1;
    for (guint i = 0; i < block; ++i) index += (gint)((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, i))->leaves->len;
    RadioSProgrammeBlock *owner = g_ptr_array_index(tree->blocks, block);
    if (leaf < 0) return owner->leaves->len ? index : -1;
    return index + leaf;
}

JsonObject *radios_programme_tree_leaf_at(RadioSProgrammeTree *tree, guint index, guint *block)
{
    for (guint i = 0; i < tree->blocks->len; ++i) {
        RadioSProgrammeBlock *candidate = g_ptr_array_index(tree->blocks, i);
        if (index < candidate->leaves->len) {
            if (block) *block = i;
            return g_ptr_array_index(candidate->leaves, index);
        }
        index -= candidate->leaves->len;
    }
    return NULL;
}

gint64 radios_programme_block_duration(const RadioSProgrammeBlock *block)
{
    gint64 total = 0;
    for (guint i = 0; i < block->leaves->len; ++i) total += MAX(0, number(g_ptr_array_index(block->leaves, i), "duration_ms"));
    JsonNode *live = json_object_get_member(block->block, "live");
    if (!total && live && JSON_NODE_HOLDS_OBJECT(live))
        total = MAX(0, number(json_node_get_object(live), "duration_ms"));
    return total;
}

RadioSProgrammeBlock *radios_programme_tree_add_block(RadioSProgrammeTree *tree, guint position,
    const gchar *kind, const gchar *title)
{
    gchar *id = g_uuid_string_random();
    RadioSProgrammeBlock *block = block_new(block_object(id, kind, title)); g_free(id);
    g_ptr_array_insert(tree->blocks, (gint)MIN(position, tree->blocks->len), block);
    return block;
}

void radios_programme_tree_insert(RadioSProgrammeTree *tree, guint block, guint position, JsonObject *leaf)
{
    g_return_if_fail(block < tree->blocks->len && leaf);
    RadioSProgrammeBlock *owner = g_ptr_array_index(tree->blocks, block);
    g_ptr_array_insert(owner->leaves, (gint)MIN(position, owner->leaves->len), leaf);
}

gboolean radios_programme_tree_remove(RadioSProgrammeTree *tree, const gchar *id)
{
    guint block = 0; gint leaf = -1;
    if (!radios_programme_tree_find(tree, id, &block, &leaf)) return FALSE;
    if (leaf < 0) g_ptr_array_remove_index(tree->blocks, block);
    else g_ptr_array_remove_index(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, block))->leaves, (guint)leaf);
    return TRUE;
}

gboolean radios_programme_tree_move(RadioSProgrammeTree *tree, const gchar *id, guint target, guint position)
{
    guint block = 0; gint leaf = -1;
    if (!radios_programme_tree_find(tree, id, &block, &leaf)) return FALSE;
    if (leaf < 0) {
        position = MIN(position, tree->blocks->len);
        if (position > block) --position;
        if (position == block) return FALSE;
        RadioSProgrammeBlock *moving = g_ptr_array_steal_index(tree->blocks, block);
        g_ptr_array_insert(tree->blocks, (gint)position, moving);
        return TRUE;
    }
    if (target >= tree->blocks->len) return FALSE;
    RadioSProgrammeBlock *source = g_ptr_array_index(tree->blocks, block), *destination = g_ptr_array_index(tree->blocks, target);
    position = MIN(position, destination->leaves->len);
    if (source == destination && position > (guint)leaf) --position;
    if (source == destination && position == (guint)leaf) return FALSE;
    JsonObject *moving = g_ptr_array_steal_index(source->leaves, (guint)leaf);
    g_ptr_array_insert(destination->leaves, (gint)position, moving);
    return TRUE;
}

void radios_programme_tree_clear(RadioSProgrammeTree *tree, guint block)
{
    if (block < tree->blocks->len) g_ptr_array_set_size(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, block))->leaves, 0);
}
