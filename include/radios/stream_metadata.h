#ifndef RADIOS_STREAM_METADATA_H
#define RADIOS_STREAM_METADATA_H

#include "radios/views.h"
#include "radios/routing_graph.h"

G_BEGIN_DECLS

#define RADIOS_STREAM_METADATA_TEXT_BYTES 1024u
#define RADIOS_STREAM_METADATA_FORMAT_BYTES 255u

typedef struct {
    gchar title[801], artist[801], text[1025];
    guint deck;             /* zero-based normal player, 0..31 */
    guint64 generation;    /* accepted engine source generation */
} RadioSStreamMetadataSnapshot;

/* Pure one-pass substitution of {artist} and {title}; other braces stay
 * literal. NULL/empty format uses "{artist} - {title}". The default emits
 * just the title when artist is absent. Whitespace/control characters are
 * flattened; a missing title or invalid UTF-8 returns FALSE with empty output.
 * Format is at most 255 bytes; output is truncated at a UTF-8 boundary to
 * 1024 bytes. Values containing placeholder text are never parsed again. */
gboolean radios_stream_metadata_format(const gchar *format, const gchar *artist,
    const gchar *title, gchar output[1025]);

/* GTK-thread read-only observer. PLAYER selects normal deck 0..31;
 * AUTOMATION follows the actually consumed global/General runner source,
 * never a draft, scheduled projection or preloaded next source. MANUAL is
 * handled by the caller and returns FALSE. A current MAIN path and playing
 * source are required; paused/finished/stale/CUE-only sources return FALSE.
 * Jingle/advertisement entries return FALSE; callers must skip dispatch and
 * leave the server's last song intact (never submit an empty title).
 * No command, provider request, draft edit, disk/network I/O or stream start
 * is issued. Every FALSE clears out; a source replacement during observation
 * is rejected. Online play-intent generations are distinct from engine ones. */
gboolean radios_stream_metadata_resolve(RadioSViewContext *ctx,
    RadioSRoutingMetadataSource source, guint deck, const gchar *format,
    RadioSStreamMetadataSnapshot *out);

G_END_DECLS
#endif
