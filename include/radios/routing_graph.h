#ifndef RADIOS_ROUTING_GRAPH_H
#define RADIOS_ROUTING_GRAPH_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Audio routing patchbay: freely placed blocks joined by cables. Every block
 * kind can be used several times; ids are stable within one configuration. */
#define RADIOS_ROUTING_GRAPH_MAX_NODES 48u
#define RADIOS_ROUTING_GRAPH_MAX_EDGES 128u
#define RADIOS_ROUTING_GRAPH_MAX_ID 65535u
#define RADIOS_ROUTING_GRAPH_MAX_X 3000.0
#define RADIOS_ROUTING_GRAPH_MAX_Y 2000.0
#define RADIOS_ROUTING_GRAPH_NAME_SIZE 161u
#define RADIOS_ROUTING_GRAPH_TEXT_SIZE 513u
/* Live Pulse streams and workers are bounded per kind. */
#define RADIOS_ROUTING_GRAPH_MAX_DEVICES 8u   /* inputs, microphones, outputs, virtual interfaces each */
#define RADIOS_ROUTING_GRAPH_MAX_RECORDERS 8u
#define RADIOS_ROUTING_GRAPH_MAX_STREAMS 4u
/* Microphone device that means the station's on-air talk microphone. */
#define RADIOS_ROUTING_TALK_MICROPHONE "@talk"

typedef enum {
    RG_MIXER,      /* "Mikser girişi": feeds a mixer channel; its output is that channel */
    RG_INPUT,      /* "Ses girişi": capture device, line input or output monitor */
    RG_MICROPHONE, /* "Mikrofon": a microphone or the on-air talk microphone */
    RG_VIRTUAL,    /* "Sanal ses arayüzü": RadioS-defined virtual device (in and out) */
    RG_MAIN,       /* "MAIN yayını": main mix; its input adds to MAIN */
    RG_OUTPUT,     /* "Ses çıkışı": playback device */
    RG_RECORDER,   /* "Kaydedici": WAV recorder */
    RG_STREAM,     /* "İnternet yayını": Icecast / SHOUTcast source */
    RG_METER,      /* "Seviye ölçer": pass-through level meter */
    RG_KIND_COUNT
} RadioSRoutingGraphKind;
typedef enum {
    RG_STREAM_ICECAST, RG_STREAM_SHOUTCAST1, RG_STREAM_SHOUTCAST2, RG_STREAM_PROTOCOL_COUNT
} RadioSRoutingStreamProtocol;
typedef enum {
    RG_METADATA_MANUAL, RG_METADATA_PLAYER, RG_METADATA_AUTOMATION, RG_METADATA_SOURCE_COUNT
} RadioSRoutingMetadataSource;

typedef struct {
    guint id;
    RadioSRoutingGraphKind kind;
    gdouble x, y;
    gchar name[RADIOS_ROUTING_GRAPH_NAME_SIZE]; /* operator's name; empty shows the kind title */
    gdouble gain_db;                            /* -60..+12, applied to the block's signal */
    guint channel;                              /* RG_MIXER: mixer channel 1..32 */
    gchar device[RADIOS_ROUTING_GRAPH_TEXT_SIZE]; /* INPUT, MICROPHONE, OUTPUT */
    gboolean push_to_talk;                      /* MICROPHONE: passes only while the on-air mic is open */
    gboolean as_microphone;                     /* VIRTUAL: also offer a virtual microphone to other apps */
    /* RG_RECORDER */
    gchar folder[RADIOS_ROUTING_GRAPH_TEXT_SIZE], prefix[81];
    guint split_minutes;                        /* 0 off, 1..1440 */
    /* RG_STREAM */
    RadioSRoutingStreamProtocol protocol;
    gchar host[256], mount[256], username[129];
    guint port, stream_id, bitrate_kbps;
    gchar station[RADIOS_ROUTING_GRAPH_NAME_SIZE], genre[RADIOS_ROUTING_GRAPH_NAME_SIZE];
    gchar description[257], website[257];
    gboolean public_listing;
    /* The stream password is stored with the block only when the operator
     * ticked "Şifreyi kaydet"; otherwise it stays empty and lives in session
     * memory. JSON carries both fields only while password_saved is TRUE. */
    gboolean password_saved;
    gchar password[1025];
    /* Song information follows this source without storing the live title. */
    RadioSRoutingMetadataSource metadata_source;
    guint metadata_player;                     /* zero-based player 0..31 */
    gchar metadata_format[256], metadata_manual[1025];
} RadioSRoutingGraphNode;
typedef struct { guint from, to; } RadioSRoutingGraphEdge;
typedef struct {
    RadioSRoutingGraphNode nodes[RADIOS_ROUTING_GRAPH_MAX_NODES];
    guint node_count;
    RadioSRoutingGraphEdge edges[RADIOS_ROUTING_GRAPH_MAX_EDGES];
    guint edge_count;
} RadioSRoutingGraph;

const gchar *radios_routing_graph_kind_name(RadioSRoutingGraphKind kind);
gboolean radios_routing_graph_kind_from_name(const gchar *name, RadioSRoutingGraphKind *kind);
/* Operator wording: "Kaydedici", "İnternet yayını", ... */
const gchar *radios_routing_graph_kind_title(RadioSRoutingGraphKind kind);
gboolean radios_routing_graph_has_input(RadioSRoutingGraphKind kind);
gboolean radios_routing_graph_has_output(RadioSRoutingGraphKind kind);
/* The block's display name: its given name, otherwise its kind title. */
const gchar *radios_routing_graph_node_title(const RadioSRoutingGraphNode *node);
const gchar *radios_routing_stream_protocol_name(RadioSRoutingStreamProtocol protocol);
const gchar *radios_routing_metadata_source_name(RadioSRoutingMetadataSource source);

/* Microphone -> recorder and talk microphone -> MAIN; nothing runs on load. */
void radios_routing_graph_default(RadioSRoutingGraph *graph);
/* Kind defaults for a new block (gain 0, channel 1, Icecast 8000 /canli 128). */
void radios_routing_graph_node_defaults(RadioSRoutingGraphNode *node, RadioSRoutingGraphKind kind);
RadioSRoutingGraphNode *radios_routing_graph_find(RadioSRoutingGraph *graph, guint id);
const RadioSRoutingGraphNode *radios_routing_graph_find_const(const RadioSRoutingGraph *graph, guint id);
/* Adds a block with kind defaults and a fresh id (written to *id). */
gboolean radios_routing_graph_add(RadioSRoutingGraph *graph,
    RadioSRoutingGraphKind kind, gdouble x, gdouble y, guint *id, GError **error);
/* Replaces a block's settings; id, kind and position are kept. */
gboolean radios_routing_graph_update(RadioSRoutingGraph *graph, const RadioSRoutingGraphNode *node, GError **error);
gboolean radios_routing_graph_rename(RadioSRoutingGraph *graph, guint id, const gchar *name, GError **error);
gboolean radios_routing_graph_remove(RadioSRoutingGraph *graph, guint id, GError **error);
gboolean radios_routing_graph_move(RadioSRoutingGraph *graph,
    guint id, gdouble x, gdouble y, GError **error);
gboolean radios_routing_graph_connect(RadioSRoutingGraph *graph,
    guint from, guint to, GError **error);
gboolean radios_routing_graph_disconnect(RadioSRoutingGraph *graph,
    guint from, guint to, GError **error);
gboolean radios_routing_graph_has_edge(const RadioSRoutingGraph *graph, guint from, guint to);
/* Rejects cycles and paths that would feed the mix back into itself: MAIN
 * to MAIN or a mixer channel, a channel to the same channel, and an input
 * that monitors the output it is routed to. */
gboolean radios_routing_graph_validate(const RadioSRoutingGraph *graph, GError **error);
/* Source blocks whose signal reaches `id` (itself for sources). */
guint radios_routing_graph_upstream(const RadioSRoutingGraph *graph, guint id,
    guint *sources, guint capacity);
/* Blocks in signal order: every block follows all of its inputs. */
gboolean radios_routing_graph_order(const RadioSRoutingGraph *graph, guint order[RADIOS_ROUTING_GRAPH_MAX_NODES]);

/* {nodes:[{id,kind,name,x,y,gain_db,settings{...}}],edges:[{from,to}]}.
 * Invalid input leaves the destination untouched. */
gboolean radios_routing_graph_from_json(RadioSRoutingGraph *graph,
    JsonObject *object, GError **error);
JsonObject *radios_routing_graph_to_json(const RadioSRoutingGraph *graph);
/* Schema 1 documents (application/compressor era): keeps the microphone,
 * output, MAIN and recorder blocks with their devices and cables. */
void radios_routing_graph_from_legacy(RadioSRoutingGraph *graph, JsonObject *config);

G_END_DECLS
#endif
