#ifndef RADIOS_AUDIO_DECODER_H
#define RADIOS_AUDIO_DECODER_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define RADIOS_AUDIO_SAMPLE_RATE 48000u
#define RADIOS_AUDIO_CHANNELS 2u
#define RADIOS_AUDIO_MAX_READ_FRAMES 4096u

typedef struct RadioSAudioDecoder RadioSAudioDecoder;
typedef enum {
    RADIOS_AUDIO_DECODER_ERROR_INVALID,
    RADIOS_AUDIO_DECODER_ERROR_IO,
    RADIOS_AUDIO_DECODER_ERROR_HASH,
    RADIOS_AUDIO_DECODER_ERROR_CHANGED,
    RADIOS_AUDIO_DECODER_ERROR_DECODE,
    RADIOS_AUDIO_DECODER_ERROR_CANCELLED,
    RADIOS_AUDIO_DECODER_ERROR_LIMIT,
    RADIOS_AUDIO_DECODER_ERROR_SEEK
} RadioSAudioDecoderError;
#define RADIOS_AUDIO_DECODER_ERROR (radios_audio_decoder_error_quark())
GQuark radios_audio_decoder_error_quark(void);

/* All operations belong to one decoder worker, never the audio callback or UI.
 * A mandatory 64-hex SHA-256 is verified over the entire regular local file
 * before decoding. Every path component must be free of symbolic links. */
RadioSAudioDecoder *radios_audio_decoder_open(const gchar *path,
    const gchar *expected_sha256, GCancellable *cancellable, GError **error);

/* Caller provides max_frames * 2 floats. Returns frame count, 0 at EOF, or -1
 * with an error. max_frames is 1..4096. The caller must ignore the buffer on
 * error: partial data is never reported as a successful decode. Full scale is
 * 1.0; floating sources may exceed it. Cancellation does not own the decoder. */
gssize radios_audio_decoder_read(RadioSAudioDecoder *decoder, gfloat *stereo,
    guint max_frames, GCancellable *cancellable, GError **error);

/* Position is in actual decoded/resampled 48 kHz output frames, independent of
 * a container's advertised duration/PTS. A forward seek continues from the
 * current position and decodes/discards only the frames in between; a
 * backward seek resets decoder/resampler and decodes/discards bounded blocks
 * from the origin. Both are exact; the latter may take O(target) time. Seeks
 * are cancellable. Beyond EOF fails without clamping. */
gboolean radios_audio_decoder_seek(RadioSAudioDecoder *decoder,
    guint64 output_frame, GCancellable *cancellable, GError **error);
guint64 radios_audio_decoder_position_frames(const RadioSAudioDecoder *decoder);
void radios_audio_decoder_close(RadioSAudioDecoder *decoder);

G_END_DECLS
#endif
