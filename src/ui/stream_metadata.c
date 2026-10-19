#include "radios/stream_metadata.h"
#include "radios/audio.h"
#include "radios/audio_runner.h"
#include "radios/audio_ui.h"
#include "radios/broadcast_header.h"
#include "radios/playout_ui.h"

#include <string.h>

#define DEFAULT_FORMAT "{artist} - {title}"

/* Human-readable single lines, including operator/source text with tabs or
 * newlines. Invalid UTF-8 is rejected rather than silently changing identity. */
static gchar *line_text(const gchar *value)
{
    if (!value) return g_strdup("");
    if (!g_utf8_validate(value, -1, NULL)) return NULL;
    GString *line = g_string_new(NULL);
    gboolean pending_space = FALSE;
    for (const gchar *cursor = value; *cursor; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        if (g_unichar_isspace(character) || g_unichar_iscntrl(character)) {
            pending_space = line->len > 0;
            continue;
        }
        if (pending_space) { g_string_append_c(line, ' '); pending_space = FALSE; }
        g_string_append_unichar(line, character);
    }
    return g_string_free(line, FALSE);
}

static void copy_text(gchar *destination, gsize capacity, const gchar *source)
{
    gsize length = MIN(strlen(source), capacity - 1);
    while (length && !g_utf8_validate(source, (gssize)length, NULL)) --length;
    memcpy(destination, source, length); destination[length] = '\0';
}

gboolean radios_stream_metadata_format(const gchar *format, const gchar *artist,
    const gchar *title, gchar output[1025])
{
    if (!output) return FALSE;
    output[0] = '\0';
    const gchar *pattern = format && *format ? format : DEFAULT_FORMAT;
    if (strlen(pattern) > RADIOS_STREAM_METADATA_FORMAT_BYTES) return FALSE;
    gchar *clean_pattern = line_text(pattern), *clean_artist = line_text(artist),
        *clean_title = line_text(title);
    gboolean valid = clean_pattern && *clean_pattern && clean_artist && clean_title && *clean_title;
    if (!valid) {
        g_free(clean_pattern); g_free(clean_artist); g_free(clean_title); return FALSE;
    }
    GString *text = g_string_new(NULL);
    if (!clean_artist[0] && g_str_equal(clean_pattern, DEFAULT_FORMAT))
        g_string_append(text, clean_title);
    else for (const gchar *cursor = clean_pattern; *cursor;) {
        if (g_str_has_prefix(cursor, "{artist}")) {
            g_string_append(text, clean_artist); cursor += strlen("{artist}");
        } else if (g_str_has_prefix(cursor, "{title}")) {
            g_string_append(text, clean_title); cursor += strlen("{title}");
        } else {
            g_string_append_unichar(text, g_utf8_get_char(cursor)); cursor = g_utf8_next_char(cursor);
        }
    }
    gchar *line = line_text(text->str);
    valid = line && *line;
    if (valid) copy_text(output, RADIOS_STREAM_METADATA_TEXT_BYTES + 1, line);
    g_free(line); g_string_free(text, TRUE);
    g_free(clean_pattern); g_free(clean_artist); g_free(clean_title);
    return valid;
}

static gboolean playing_main(const RadioSAudioSnapshot *audio, guint deck)
{
    if (deck >= RADIOS_AUDIO_NORMAL_DECKS) return FALSE;
    const RadioSAudioDeckSnapshot *current = &audio->decks[deck];
    return audio->connected && audio->buses[RADIOS_AUDIO_BUS_MAIN].ready &&
        audio->buses[RADIOS_AUDIO_BUS_MAIN].gate && current->generation &&
        current->state == RADIOS_AUDIO_PLAYING && !current->runner_paused &&
        current->main_routed && current->main_path_ready &&
        current->output_bus == RADIOS_AUDIO_BUS_MAIN &&
        current->cursor_frames > current->cue_in_frame;
}

static gboolean same_source(const RadioSAudioDeckSnapshot *before,
    const RadioSAudioDeckSnapshot *after)
{
    return before->generation == after->generation &&
        g_str_equal(before->media_id, after->media_id) &&
        g_str_equal(before->version_id, after->version_id) &&
        g_str_equal(before->binding.playlist_id, after->binding.playlist_id) &&
        g_str_equal(before->binding.entry_id, after->binding.entry_id) &&
        g_str_equal(before->binding.provider, after->binding.provider) &&
        g_str_equal(before->binding.source_id, after->binding.source_id);
}

static gboolean running(const RadioSAudioRunSnapshot *run)
{
    return run->epoch && run->deck_current >= 0 && run->deck_current < (gint)RADIOS_AUDIO_NORMAL_DECKS &&
        (run->state == RADIOS_AUDIO_RUN_PLAYING || run->state == RADIOS_AUDIO_RUN_OVERLAP) &&
        run->owned[run->deck_current] && run->entry_id[0];
}

/* Advertising and station IDs are audible programme items, but must not
 * replace the last song advertised to listeners. The classification comes
 * from the immutable accepted plan or the generation-bound loaded catalogue,
 * never from a title guess or a mutable editor/playlist selection. */
static gboolean exportable_kind(const gchar *kind)
{
    return g_strcmp0(kind, "jingle") && g_strcmp0(kind, "advertisement");
}

static gboolean read_metadata(RadioSViewContext *ctx, RadioSAudioEngine *engine, guint deck,
    const RadioSAudioDeckSnapshot *actual, RadioSStreamMetadataSnapshot *out)
{
    RadioSPlayoutDeckMetadata accepted = {0};
    if (radios_broadcast_player_deck_metadata(ctx, deck, &accepted)) {
        /* TRUE includes a stale managed owner: an empty title is authoritative
         * and must not turn into a filename from the unmanaged fallback. */
        if (!accepted.title[0] || !exportable_kind(accepted.kind)) return FALSE;
        copy_text(out->title, sizeof out->title, accepted.title);
        copy_text(out->artist, sizeof out->artist, accepted.artist); return TRUE;
    }
    /* Managed auxiliary voices and unmatched/stale accepted occurrences do
     * not become songs through the unmanaged filename/title fallback. */
    gint source = -2;
    if (radios_audio_engine_deck_runner_owner(engine, deck, &source)) return FALSE;
    RadioSAudioUiSnapshot ui = {0};
    if (radios_audio_ui_get_snapshot(ctx, deck, &ui) && ui.generation == actual->generation) {
        if (!exportable_kind(ui.kind)) return FALSE;
        if (ui.title[0]) {
            copy_text(out->title, sizeof out->title, ui.title);
            copy_text(out->artist, sizeof out->artist, ui.artist); return TRUE;
        }
    }
    copy_text(out->title, sizeof out->title, actual->binding.title);
    copy_text(out->artist, sizeof out->artist, actual->binding.artist);
    return out->title[0] != '\0';
}

gboolean radios_stream_metadata_resolve(RadioSViewContext *ctx,
    RadioSRoutingMetadataSource source, guint deck, const gchar *format,
    RadioSStreamMetadataSnapshot *out)
{
    if (!out) return FALSE;
    memset(out, 0, sizeof *out);
    if (!ctx || (source != RG_METADATA_PLAYER && source != RG_METADATA_AUTOMATION) ||
        (source == RG_METADATA_PLAYER && deck >= RADIOS_AUDIO_NORMAL_DECKS)) return FALSE;
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx);
    if (!engine) return FALSE;
    RadioSAudioSnapshot *before = g_new0(RadioSAudioSnapshot, 1), *after = g_new0(RadioSAudioSnapshot, 1);
    RadioSAudioRunSnapshot run = {0}, later = {0};
    radios_audio_engine_snapshot(engine, before);
    gboolean valid = TRUE;
    if (source == RG_METADATA_AUTOMATION) {
        radios_audio_engine_runner_snapshot(engine, &run);
        valid = running(&run);
        if (valid) deck = (guint)run.deck_current;
    }
    valid = valid && playing_main(before, deck);
    RadioSStreamMetadataSnapshot current = {.deck = deck};
    const RadioSAudioDeckSnapshot *actual = valid ? &before->decks[deck] : NULL;
    if (valid && source == RG_METADATA_AUTOMATION) {
        RadioSPlayoutDeckMetadata accepted = {0};
        valid = g_str_equal(run.entry_id, actual->binding.entry_id) &&
            radios_playout_ui_deck_metadata(ctx, deck, &accepted) && accepted.title[0] &&
            exportable_kind(accepted.kind);
        if (valid) {
            copy_text(current.title, sizeof current.title, accepted.title);
            copy_text(current.artist, sizeof current.artist, accepted.artist);
        }
    } else if (valid) valid = read_metadata(ctx, engine, deck, actual, &current);
    if (valid) valid = radios_stream_metadata_format(format, current.artist, current.title, current.text);
    if (valid) {
        radios_audio_engine_snapshot(engine, after);
        valid = before->loss_epoch == after->loss_epoch && playing_main(after, deck) &&
            same_source(actual, &after->decks[deck]);
        if (valid && source == RG_METADATA_AUTOMATION) {
            radios_audio_engine_runner_snapshot(engine, &later);
            valid = running(&later) && later.epoch == run.epoch && later.index == run.index &&
                later.deck_current == run.deck_current && g_str_equal(later.entry_id, run.entry_id);
        }
    }
    if (valid) { current.generation = actual->generation; *out = current; }
    g_free(before); g_free(after); radios_audio_engine_unref(engine);
    return valid;
}
