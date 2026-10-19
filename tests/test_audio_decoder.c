#define _POSIX_C_SOURCE 200809L
#include "radios/audio_decoder.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef RADIOS_TEST_PROJECT_ROOT
#define RADIOS_TEST_PROJECT_ROOT "."
#endif
typedef struct { gchar *directory, *wave, *sha; } Fixture;

static void remove_tree(const gchar *path)
{
    struct stat status; if (lstat(path, &status) != 0) return;
    if (S_ISDIR(status.st_mode)) {
        GDir *dir = g_dir_open(path, 0, NULL); const gchar *name;
        while (dir && (name = g_dir_read_name(dir))) {
            gchar *child = g_build_filename(path, name, NULL); remove_tree(child); g_free(child);
        }
        if (dir) g_dir_close(dir);
        g_assert_cmpint(g_rmdir(path), ==, 0);
    } else g_assert_cmpint(g_unlink(path), ==, 0);
}
static void le16(GByteArray *bytes, guint16 value)
{ guchar data[] = {(guchar)value, (guchar)(value >> 8)}; g_byte_array_append(bytes, data, 2); }
static void le32(GByteArray *bytes, guint32 value)
{ guchar data[] = {(guchar)value, (guchar)(value >> 8), (guchar)(value >> 16), (guchar)(value >> 24)}; g_byte_array_append(bytes, data, 4); }
static gint16 sample(guint frame, guint rate, guint channel)
{
    gdouble time = (gdouble)frame / rate;
    return (gint16)lrint((0.4 * sin(2 * G_PI * (237 * time + 131 * time * time)) +
        0.1 * time) * (channel % 2 ? -0.5 : 1.0) * 32767);
}
static void write_wave(const gchar *path, guint frames, guint rate, guint channels)
{
    guint size = frames * channels * 2; GByteArray *bytes = g_byte_array_sized_new(size + 44);
    g_byte_array_append(bytes, (const guchar *)"RIFF", 4); le32(bytes, size + 36);
    g_byte_array_append(bytes, (const guchar *)"WAVEfmt ", 8); le32(bytes, 16);
    le16(bytes, 1); le16(bytes, (guint16)channels); le32(bytes, rate); le32(bytes, rate * channels * 2);
    le16(bytes, (guint16)(channels * 2)); le16(bytes, 16);
    g_byte_array_append(bytes, (const guchar *)"data", 4); le32(bytes, size);
    for (guint i = 0; i < frames; ++i) for (guint channel = 0; channel < channels; ++channel)
        le16(bytes, (guint16)sample(i, rate, channel));
    g_assert_true(g_file_set_contents(path, (const gchar *)bytes->data, bytes->len, NULL));
    g_assert_cmpint(g_chmod(path, 0600), ==, 0); g_byte_array_unref(bytes);
}
static gchar *hash_file(const gchar *path)
{
    gchar *bytes = NULL; gsize length = 0; g_assert_true(g_file_get_contents(path, &bytes, &length, NULL));
    gchar *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *)bytes, length); g_free(bytes); return sha;
}
static void setup(Fixture *fixture, gconstpointer data)
{
    (void)data; gchar *root = g_canonicalize_filename(RADIOS_TEST_PROJECT_ROOT, NULL);
    gchar *build = g_build_filename(root, "build", NULL); g_assert_cmpint(g_mkdir_with_parents(build, 0700), ==, 0);
    fixture->directory = g_build_filename(build, "audio-decoder-XXXXXX", NULL); g_assert_nonnull(g_mkdtemp(fixture->directory));
    fixture->wave = g_build_filename(fixture->directory, "Ölçülen özel ses.wav", NULL);
    write_wave(fixture->wave, 48000, 48000, 2); fixture->sha = hash_file(fixture->wave);
    g_free(build); g_free(root);
}
static void teardown(Fixture *fixture, gconstpointer data)
{
    (void)data; remove_tree(fixture->directory); g_free(fixture->directory); g_free(fixture->wave); g_free(fixture->sha);
}
static RadioSAudioDecoder *open_audio(const gchar *path)
{
    gchar *sha = hash_file(path); GError *error = NULL;
    RadioSAudioDecoder *decoder = radios_audio_decoder_open(path, sha, NULL, &error); g_free(sha);
    g_assert_no_error(error); g_assert_nonnull(decoder); return decoder;
}
static GArray *read_all(RadioSAudioDecoder *decoder, guint chunk)
{
    GArray *pcm = g_array_new(FALSE, FALSE, sizeof(gfloat)); gfloat buffer[RADIOS_AUDIO_MAX_READ_FRAMES * 2];
    GError *error = NULL; gssize frames;
    while ((frames = radios_audio_decoder_read(decoder, buffer, chunk, NULL, &error)) > 0)
        g_array_append_vals(pcm, buffer, (guint)frames * 2);
    g_assert_no_error(error); g_assert_cmpint(frames, ==, 0); return pcm;
}
static gchar *transcode(Fixture *fixture, const gchar *codec, const gchar *extension)
{
    gchar *path = g_strdup_printf("%s/encoded.%s", fixture->directory, extension);
    gchar *arguments[] = {"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", fixture->wave,
        "-c:a", (gchar *)codec, path, NULL};
    GError *error = NULL; gchar *output = NULL; gint status = 0;
    g_assert_true(g_spawn_sync(fixture->directory, arguments, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &output, &status, &error));
    g_assert_no_error(error); if (!g_spawn_check_wait_status(status, &error)) g_error("Encoding failed: %s", output);
    g_assert_no_error(error); g_free(output); return path;
}
static void compare_seek(RadioSAudioDecoder *decoder, const GArray *pcm, guint64 position)
{
    GError *error = NULL; g_assert_true(radios_audio_decoder_seek(decoder, position, NULL, &error)); g_assert_no_error(error);
    g_assert_cmpuint(radios_audio_decoder_position_frames(decoder), ==, position);
    gfloat buffer[514]; gssize expected = (gssize)MIN(G_GUINT64_CONSTANT(257), pcm->len / 2 - position);
    gssize actual = radios_audio_decoder_read(decoder, buffer, 257, NULL, &error);
    g_assert_no_error(error); g_assert_cmpint(actual, ==, expected);
    for (guint i = 0; i < (guint)actual * 2; ++i)
        g_assert_cmpfloat(fabs(buffer[i] - g_array_index(pcm, gfloat, (guint)position * 2 + i)), <, 0.000001);
    g_assert_cmpuint(radios_audio_decoder_position_frames(decoder), ==, position + (guint)actual);
}

static void pcm_incremental(Fixture *fixture, gconstpointer data)
{
    (void)data; RadioSAudioDecoder *decoder = open_audio(fixture->wave); GArray *pcm = read_all(decoder, 313);
    g_assert_cmpuint(pcm->len, ==, 96000); g_assert_cmpuint(radios_audio_decoder_position_frames(decoder), ==, 48000);
    for (guint i = 0; i < 48000; ++i) for (guint channel = 0; channel < 2; ++channel)
        g_assert_cmpfloat(fabs(g_array_index(pcm, gfloat, i * 2 + channel) - (gdouble)sample(i, 48000, channel) / 32768), <, 0.000001);
    compare_seek(decoder, pcm, 23017); compare_seek(decoder, pcm, 0); compare_seek(decoder, pcm, 47993);
    compare_seek(decoder, pcm, 48000);
    GError *error = NULL; g_assert_false(radios_audio_decoder_seek(decoder, 48001, NULL, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_SEEK); g_clear_error(&error);
    g_assert_cmpuint(radios_audio_decoder_position_frames(decoder), ==, 48000);
    compare_seek(decoder, pcm, 100); radios_audio_decoder_close(decoder); g_array_unref(pcm);
}
static void mono_resample(Fixture *fixture, gconstpointer data)
{
    (void)data; gchar *path = g_build_filename(fixture->directory, "mono44100.wav", NULL); write_wave(path, 44100, 44100, 1);
    RadioSAudioDecoder *decoder = open_audio(path); GArray *pcm = read_all(decoder, 4096);
    g_assert_cmpuint(pcm->len, ==, 96000);
    gdouble squares = 0;
    for (guint i = 0; i < pcm->len; i += 2) {
        gfloat left = g_array_index(pcm, gfloat, i), right = g_array_index(pcm, gfloat, i + 1);
        g_assert_cmpfloat(left, ==, right); g_assert_true(isfinite(left)); squares += left * left;
    }
    g_assert_cmpfloat(sqrt(squares / 48000), >, 0.25); g_assert_cmpfloat(sqrt(squares / 48000), <, 0.33);
    compare_seek(decoder, pcm, 23017); compare_seek(decoder, pcm, 77); compare_seek(decoder, pcm, 47991);
    radios_audio_decoder_close(decoder); g_array_unref(pcm); g_free(path);
}
static void multichannel(Fixture *fixture, gconstpointer data)
{
    (void)data; gchar *path = g_build_filename(fixture->directory, "six-channel.wav", NULL); write_wave(path, 16000, 16000, 6);
    RadioSAudioDecoder *decoder = open_audio(path); GArray *pcm = read_all(decoder, 1024);
    g_assert_cmpuint(pcm->len, ==, 96000); gdouble left = 0, right = 0;
    for (guint i = 0; i < pcm->len; i += 2) {
        g_assert_true(isfinite(g_array_index(pcm, gfloat, i))); g_assert_true(isfinite(g_array_index(pcm, gfloat, i + 1)));
        left += fabs(g_array_index(pcm, gfloat, i)); right += fabs(g_array_index(pcm, gfloat, i + 1));
    }
    g_assert_cmpfloat(left / 48000, >, 0.01); g_assert_cmpfloat(right / 48000, >, 0.01);
    compare_seek(decoder, pcm, 32113); radios_audio_decoder_close(decoder); g_array_unref(pcm); g_free(path);
}
static void compressed(Fixture *fixture, gconstpointer data)
{
    (void)data; const gchar *codecs[] = {"libmp3lame", "flac", "libvorbis", "aac", "aac"};
    const gchar *extensions[] = {"mp3", "flac", "ogg", "m4a", "aac"};
    for (guint i = 0; i < G_N_ELEMENTS(codecs); ++i) {
        gchar *path = transcode(fixture, codecs[i], extensions[i]); RadioSAudioDecoder *decoder = open_audio(path);
        GArray *pcm = read_all(decoder, 997); g_test_message("Decoded %s: %u output frames", extensions[i], pcm->len / 2);
        g_assert_cmpuint(pcm->len / 2, >=, 45000); g_assert_cmpuint(pcm->len / 2, <=, 50000);
        gdouble squares = 0; for (guint j = 0; j < pcm->len; ++j) { gfloat value = g_array_index(pcm, gfloat, j); g_assert_true(isfinite(value)); squares += value * value; }
        g_assert_cmpfloat(sqrt(squares / pcm->len), >, 0.15);
        compare_seek(decoder, pcm, 23717); compare_seek(decoder, pcm, 11); compare_seek(decoder, pcm, pcm->len / 2 - 99);
        radios_audio_decoder_close(decoder); g_array_unref(pcm); g_free(path);
    }
}
static void webm_opus_cached_names(Fixture *fixture, gconstpointer data)
{
    (void)data; gchar *encoded = transcode(fixture, "libopus", "webm"), *bytes = NULL; gsize length = 0;
    g_assert_true(g_file_get_contents(encoded, &bytes, &length, NULL));
    gchar *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)bytes, length);
    const gchar *names[] = {"cached.audio", sha}; GArray *reference = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        gchar *path = g_build_filename(fixture->directory, names[i], NULL);
        g_assert_true(g_file_set_contents(path, bytes, length, NULL));
        GError *error = NULL; RadioSAudioDecoder *decoder = radios_audio_decoder_open(path, sha, NULL, &error);
        g_assert_no_error(error); g_assert_nonnull(decoder);
        GArray *pcm = read_all(decoder, 997); g_assert_cmpuint(pcm->len / 2, >=, 45000); g_assert_cmpuint(pcm->len / 2, <=, 50000);
        gdouble squares = 0;
        for (guint j = 0; j < pcm->len; ++j) { gfloat sample = g_array_index(pcm, gfloat, j); g_assert_true(isfinite(sample)); squares += sample * sample; }
        g_assert_cmpfloat(sqrt(squares / pcm->len), >, 0.15);
        if (reference) {
            g_assert_cmpuint(pcm->len, ==, reference->len);
            g_assert_cmpmem(pcm->data, pcm->len * sizeof(gfloat), reference->data, reference->len * sizeof(gfloat));
        }
        compare_seek(decoder, pcm, 23717); compare_seek(decoder, pcm, 11);
        radios_audio_decoder_close(decoder);
        if (!reference) reference = pcm; else g_array_unref(pcm);
        g_free(path);
    }
    g_array_unref(reference); g_free(sha); g_free(bytes); g_free(encoded);
}
static void cancellation(Fixture *fixture, gconstpointer data)
{
    (void)data; GCancellable *cancel = g_cancellable_new(); g_cancellable_cancel(cancel); GError *error = NULL;
    g_assert_null(radios_audio_decoder_open(fixture->wave, fixture->sha, cancel, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_CANCELLED); g_clear_error(&error);
    RadioSAudioDecoder *decoder = open_audio(fixture->wave); gfloat buffer[512];
    g_assert_cmpint(radios_audio_decoder_read(decoder, buffer, 256, cancel, &error), ==, -1);
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_CANCELLED); g_clear_error(&error);
    g_assert_cmpuint(radios_audio_decoder_position_frames(decoder), ==, 0);
    g_assert_false(radios_audio_decoder_seek(decoder, 21000, cancel, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_CANCELLED); g_clear_error(&error);
    g_cancellable_reset(cancel); g_assert_true(radios_audio_decoder_seek(decoder, 21000, cancel, &error)); g_assert_no_error(error);
    g_assert_cmpint(radios_audio_decoder_read(decoder, buffer, 256, cancel, &error), ==, 256); g_assert_no_error(error);
    radios_audio_decoder_close(decoder); g_object_unref(cancel);
}
static gpointer cancel_soon(gpointer value)
{ g_usleep(1000); g_cancellable_cancel(value); return NULL; }
static void cancellation_during_work(Fixture *fixture, gconstpointer data)
{
    (void)data; gchar *large = g_build_filename(fixture->directory, "large-private.wav", NULL);
    gint fd = open(large, O_CREAT | O_WRONLY, 0600); g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, 64 * 1024 * 1024), ==, 0); close(fd);
    GCancellable *cancel = g_cancellable_new(); GError *error = NULL;
    GThread *thread = g_thread_new("cancel-verification", cancel_soon, cancel);
    g_assert_null(radios_audio_decoder_open(large, fixture->sha, cancel, &error)); g_thread_join(thread);
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_CANCELLED); g_clear_error(&error);
    /* Actual rewind/discard has enough work for cancellation to arrive after
     * seeking begins; failed/cancelled work must not make the reader unusable. */
    write_wave(large, 48000 * 100, 48000, 2); RadioSAudioDecoder *decoder = open_audio(large);
    g_cancellable_reset(cancel); thread = g_thread_new("cancel-seek", cancel_soon, cancel);
    g_assert_false(radios_audio_decoder_seek(decoder, 48000 * 99, cancel, &error)); g_thread_join(thread);
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_CANCELLED); g_clear_error(&error);
    g_cancellable_reset(cancel); g_assert_true(radios_audio_decoder_seek(decoder, 0, cancel, &error)); g_assert_no_error(error);
    gfloat buffer[256]; g_assert_cmpint(radios_audio_decoder_read(decoder, buffer, 128, cancel, &error), ==, 128); g_assert_no_error(error);
    for (guint i = 0; i < 128; ++i) g_assert_cmpfloat(fabs(buffer[i * 2] - (gdouble)sample(i, 48000, 0) / 32768), <, 0.000001);
    radios_audio_decoder_close(decoder); g_object_unref(cancel); g_free(large);
}
static void float_amplitudes(Fixture *fixture, gconstpointer data)
{
    (void)data; gchar *path = g_build_filename(fixture->directory, "float.wav", NULL);
    guint size = 512 * 2 * 4; GByteArray *bytes = g_byte_array_sized_new(size + 44);
    g_byte_array_append(bytes, (const guchar *)"RIFF", 4); le32(bytes, size + 36);
    g_byte_array_append(bytes, (const guchar *)"WAVEfmt ", 8); le32(bytes, 16); le16(bytes, 3); le16(bytes, 2);
    le32(bytes, 48000); le32(bytes, 48000 * 8); le16(bytes, 8); le16(bytes, 32);
    g_byte_array_append(bytes, (const guchar *)"data", 4); le32(bytes, size);
    for (guint i = 0; i < 1024; ++i) { gfloat value = i % 2 ? -1.25f : 1.25f; guint32 bits; memcpy(&bits, &value, sizeof bits); le32(bytes, bits); }
    g_assert_true(g_file_set_contents(path, (const gchar *)bytes->data, bytes->len, NULL));
    RadioSAudioDecoder *decoder = open_audio(path); GArray *pcm = read_all(decoder, 111);
    g_assert_cmpuint(pcm->len, ==, 1024);
    for (guint i = 0; i < pcm->len; ++i) g_assert_cmpfloat(g_array_index(pcm, gfloat, i), ==, i % 2 ? -1.25f : 1.25f);
    radios_audio_decoder_close(decoder); g_array_unref(pcm);
    /* Explicit quiet NaN in the first decoded frame must fail before reaching
     * an audio engine, rather than being clamped into a manufactured sample. */
    bytes->data[44] = 0; bytes->data[45] = 0; bytes->data[46] = 0xc0; bytes->data[47] = 0x7f;
    g_assert_true(g_file_set_contents(path, (const gchar *)bytes->data, bytes->len, NULL));
    gchar *sha = hash_file(path); GError *error = NULL;
    g_assert_null(radios_audio_decoder_open(path, sha, NULL, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_DECODE); g_clear_error(&error);
    g_byte_array_unref(bytes); g_free(sha); g_free(path);
}
static void rejected_paths_hash(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL;
    g_assert_null(radios_audio_decoder_open(fixture->wave, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", NULL, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_HASH); g_clear_error(&error);
    g_assert_null(radios_audio_decoder_open(fixture->wave, "", NULL, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_INVALID); g_clear_error(&error);
    gchar *link = g_build_filename(fixture->directory, "link.wav", NULL); g_assert_cmpint(symlink(fixture->wave, link), ==, 0);
    g_assert_null(radios_audio_decoder_open(link, fixture->sha, NULL, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_INVALID); g_clear_error(&error);
    gchar *dir_link = g_build_filename(fixture->directory, "linked-directory", NULL); g_assert_cmpint(symlink(fixture->directory, dir_link), ==, 0);
    gchar *parent_link = g_build_filename(dir_link, "Ölçülen özel ses.wav", NULL);
    g_assert_null(radios_audio_decoder_open(parent_link, fixture->sha, NULL, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_IO); g_clear_error(&error);
    gchar *fifo = g_build_filename(fixture->directory, "fifo.wav", NULL); g_assert_cmpint(mkfifo(fifo, 0600), ==, 0);
    g_assert_null(radios_audio_decoder_open(fifo, fixture->sha, NULL, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_INVALID); g_clear_error(&error);
    g_assert_null(radios_audio_decoder_open("https://example.invalid/audio.wav", fixture->sha, NULL, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_INVALID); g_clear_error(&error);
    gchar *upper = g_ascii_strup(fixture->sha, -1); RadioSAudioDecoder *decoder = radios_audio_decoder_open(fixture->wave, upper, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(decoder); radios_audio_decoder_close(decoder);
    g_free(upper); g_free(link); g_free(dir_link); g_free(parent_link); g_free(fifo);
}
static void changed_file(Fixture *fixture, gconstpointer data)
{
    (void)data; RadioSAudioDecoder *decoder = open_audio(fixture->wave);
    gint fd = open(fixture->wave, O_WRONLY); g_assert_cmpint(fd, >=, 0); guchar bytes[] = {123, 45};
    g_assert_cmpint(pwrite(fd, bytes, sizeof bytes, 48), ==, sizeof bytes); close(fd);
    gfloat buffer[256]; GError *error = NULL;
    g_assert_cmpint(radios_audio_decoder_read(decoder, buffer, 128, NULL, &error), ==, -1);
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_CHANGED); g_clear_error(&error);
    g_assert_false(radios_audio_decoder_seek(decoder, 0, NULL, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_CHANGED); g_clear_error(&error);
    radios_audio_decoder_close(decoder);
}
static void malformed(Fixture *fixture, gconstpointer data)
{
    (void)data; gchar *broken = g_build_filename(fixture->directory, "truncated.wav", NULL);
    gchar *bytes = NULL; gsize length = 0; g_assert_true(g_file_get_contents(fixture->wave, &bytes, &length, NULL));
    g_assert_true(g_file_set_contents(broken, bytes, (gssize)length / 2, NULL)); g_free(bytes);
    gchar *sha = hash_file(broken); GError *error = NULL;
    g_assert_null(radios_audio_decoder_open(broken, sha, NULL, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_DECODE); g_clear_error(&error); g_free(sha);
    g_assert_true(g_file_set_contents(broken, "RIFF invalid file", -1, NULL)); sha = hash_file(broken);
    g_assert_null(radios_audio_decoder_open(broken, sha, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error); g_free(sha);
    const gchar *codecs[] = {"flac", "libvorbis", "aac"}; const gchar *extensions[] = {"flac", "ogg", "m4a"};
    for (guint i = 0; i < G_N_ELEMENTS(codecs); ++i) {
        gchar *path = transcode(fixture, codecs[i], extensions[i]); g_assert_true(g_file_get_contents(path, &bytes, &length, NULL));
        g_assert_true(g_file_set_contents(path, bytes, (gssize)length * 3 / 4, NULL)); g_free(bytes); sha = hash_file(path);
        RadioSAudioDecoder *decoder = radios_audio_decoder_open(path, sha, NULL, &error);
        if (decoder) {
            gfloat pcm[8192]; while (radios_audio_decoder_read(decoder, pcm, 4096, NULL, &error) > 0) {}
            radios_audio_decoder_close(decoder);
        }
        g_assert_nonnull(error); g_clear_error(&error); g_free(sha); g_free(path);
    }
    g_free(broken);
}
static void limits(Fixture *fixture, gconstpointer data)
{
    (void)data; RadioSAudioDecoder *decoder = open_audio(fixture->wave); gfloat buffer[2]; GError *error = NULL;
    g_assert_cmpint(radios_audio_decoder_read(decoder, buffer, 0, NULL, &error), ==, -1);
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_INVALID); g_clear_error(&error);
    g_assert_cmpint(radios_audio_decoder_read(decoder, buffer, 4097, NULL, &error), ==, -1);
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_INVALID); g_clear_error(&error);
    g_assert_false(radios_audio_decoder_seek(decoder, G_MAXUINT64, NULL, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_SEEK); g_clear_error(&error);
    g_assert_cmpuint(radios_audio_decoder_position_frames(decoder), ==, 0);
    g_assert_cmpint(radios_audio_decoder_read(decoder, buffer, 1, NULL, &error), ==, 1); g_assert_no_error(error);
    radios_audio_decoder_close(decoder);
    gchar *huge = g_build_filename(fixture->directory, "too-large.wav", NULL); gint fd = open(huge, O_CREAT | O_WRONLY, 0600);
    g_assert_cmpint(fd, >=, 0); g_assert_cmpint(ftruncate(fd, (off_t)2 * 1024 * 1024 * 1024 + 1), ==, 0); close(fd);
    g_assert_null(radios_audio_decoder_open(huge, fixture->sha, NULL, &error));
    g_assert_error(error, RADIOS_AUDIO_DECODER_ERROR, RADIOS_AUDIO_DECODER_ERROR_LIMIT); g_clear_error(&error); g_free(huge);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/audio-decoder/pcm-incremental-seek", Fixture, NULL, setup, pcm_incremental, teardown);
    g_test_add("/audio-decoder/mono-resample-seek", Fixture, NULL, setup, mono_resample, teardown);
    g_test_add("/audio-decoder/multichannel-downmix", Fixture, NULL, setup, multichannel, teardown);
    g_test_add("/audio-decoder/compressed-seek", Fixture, NULL, setup, compressed, teardown);
    g_test_add("/audio-decoder/webm-opus-cache-and-sha-names", Fixture, NULL, setup, webm_opus_cached_names, teardown);
    g_test_add("/audio-decoder/cancellation", Fixture, NULL, setup, cancellation, teardown);
    g_test_add("/audio-decoder/cancellation-during-work", Fixture, NULL, setup, cancellation_during_work, teardown);
    g_test_add("/audio-decoder/float-amplitude-invalid", Fixture, NULL, setup, float_amplitudes, teardown);
    g_test_add("/audio-decoder/paths-hash-private", Fixture, NULL, setup, rejected_paths_hash, teardown);
    g_test_add("/audio-decoder/changed-file", Fixture, NULL, setup, changed_file, teardown);
    g_test_add("/audio-decoder/corrupt-truncated", Fixture, NULL, setup, malformed, teardown);
    g_test_add("/audio-decoder/limits", Fixture, NULL, setup, limits, teardown);
    return g_test_run();
}
