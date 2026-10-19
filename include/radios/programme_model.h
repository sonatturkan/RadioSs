#ifndef RADIOS_PROGRAMME_MODEL_H
#define RADIOS_PROGRAMME_MODEL_H

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* "Eventler" of the programme draft. config.events stays the flat playable
 * sequence that playout prepares; config.blocks lists the events (blocks) of
 * the broadcast flow, and every event names its block with block_id. A draft
 * saved before blocks groups consecutive music into one implicit event until
 * a different kind; its identity derives from the first content identity.
 * Declared blocks retain their boundaries, identities and operator metadata. */
typedef struct {
    JsonObject *block;  /* owned reference: id, kind, title, start_time, note */
    GPtrArray *leaves;  /* owned JsonObject references, in playback order */
} RadioSProgrammeBlock;

typedef struct {
    GPtrArray *blocks;  /* RadioSProgrammeBlock */
} RadioSProgrammeTree;

RadioSProgrammeTree *radios_programme_tree_new(JsonObject *config);
void radios_programme_tree_free(RadioSProgrammeTree *tree);
/* A deep copy of config carrying this tree's blocks and events. Timing that
 * names a removed event is dropped. */
JsonNode *radios_programme_tree_config(RadioSProgrammeTree *tree, JsonObject *config);

/* Item lookup. *leaf is -1 when id names a block. */
gboolean radios_programme_tree_find(RadioSProgrammeTree *tree, const gchar *id, guint *block, gint *leaf);
guint radios_programme_tree_leaf_count(RadioSProgrammeTree *tree);
/* One identity for a planned voice in both event and content views. Accepted
 * primary children retain entry_id; overlays derive a stable parent-bound id. */
gchar *radios_programme_hint_id(JsonObject *leaf, JsonObject *hint, guint ordinal);
guint radios_programme_leaf_content_count(JsonObject *leaf);
gboolean radios_programme_kind_attaches(const gchar *kind);
/* Index in config.events of a leaf, or of a block's first leaf; -1 if none. */
gint radios_programme_tree_leaf_index(RadioSProgrammeTree *tree, const gchar *id);
JsonObject *radios_programme_tree_leaf_at(RadioSProgrammeTree *tree, guint index, guint *block);
gint64 radios_programme_block_duration(const RadioSProgrammeBlock *block);

/* Edits. Inserted leaves are adopted (transfer full). Positions clamp. */
RadioSProgrammeBlock *radios_programme_tree_add_block(RadioSProgrammeTree *tree, guint position,
    const gchar *kind, const gchar *title);
void radios_programme_tree_insert(RadioSProgrammeTree *tree, guint block, guint position, JsonObject *leaf);
gboolean radios_programme_tree_remove(RadioSProgrammeTree *tree, const gchar *id);
/* A block moves to root position; a leaf moves into block at position. */
gboolean radios_programme_tree_move(RadioSProgrammeTree *tree, const gchar *id, guint block, guint position);
void radios_programme_tree_clear(RadioSProgrammeTree *tree, guint block);

/* Same id derivation as radios_programme_tree_new uses for legacy events. */
gchar *radios_programme_block_id_for(const gchar *event_id);
JsonNode *radios_programme_clone(JsonNode *node);
JsonObject *radios_programme_clone_object(JsonObject *object);
gboolean radios_programme_kind_valid(const gchar *kind);
const gchar *radios_programme_kind_label(const gchar *kind);  /* "Müzik" */
const gchar *radios_programme_event_title(const gchar *kind); /* "Müzik eventi" */

G_END_DECLS
#endif
