#define _POSIX_C_SOURCE 200809L
#include "radios/media.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef RADIOS_TEST_PROJECT_ROOT
#define RADIOS_TEST_PROJECT_ROOT "."
#endif
typedef struct {gchar *directory, *project, *wave; RadioSConfig config;} Fixture;

static void remove_tree(const gchar *path)
{
    struct stat status;
    if (lstat(path, &status) != 0) return;
    if (S_ISDIR(status.st_mode)) {
        GDir *dir = g_dir_open(path, 0, NULL); const gchar *name;
        while (dir && (name = g_dir_read_name(dir))) { gchar *child = g_build_filename(path, name, NULL); remove_tree(child); g_free(child); }
        if (dir) g_dir_close(dir);
        g_assert_cmpint(g_rmdir(path), ==, 0);
    } else g_assert_cmpint(g_unlink(path), ==, 0);
}
static void le16(GByteArray *bytes, guint16 value)
{ guchar data[] = {(guchar)value, (guchar)(value >> 8)}; g_byte_array_append(bytes, data, 2); }
static void le32(GByteArray *bytes, guint32 value)
{ guchar data[] = {(guchar)value, (guchar)(value >> 8), (guchar)(value >> 16), (guchar)(value >> 24)}; g_byte_array_append(bytes, data, 4); }

static void write_wave(const gchar *path, guint frames, guint channels, gboolean floating)
{
    guint bytes_per_sample = floating ? 4 : 2, size = frames * channels * bytes_per_sample;
    GByteArray *bytes = g_byte_array_sized_new(size + 44);
    g_byte_array_append(bytes, (const guchar *)"RIFF", 4); le32(bytes, size + 36);
    g_byte_array_append(bytes, (const guchar *)"WAVEfmt ", 8); le32(bytes, 16);
    le16(bytes, floating ? 3 : 1); le16(bytes, (guint16)channels); le32(bytes, 48000);
    le32(bytes, 48000 * channels * bytes_per_sample); le16(bytes, (guint16)(channels * bytes_per_sample)); le16(bytes, (guint16)(bytes_per_sample * 8));
    g_byte_array_append(bytes, (const guchar *)"data", 4); le32(bytes, size);
    for (guint i = 0; i < frames; ++i) for (guint channel = 0; channel < channels; ++channel) {
        if (floating) {
            gfloat value = channel % 2 ? -1.25f : 1.25f; guint32 bits; memcpy(&bits, &value, sizeof bits); le32(bytes, bits);
        } else le16(bytes, (guint16)(gint16)lrint(sin((gdouble)i * 2 * G_PI * 1000 / 48000) * (channel ? 0.25 : 0.5) * 32767));
    }
    GError *error = NULL; g_assert_true(g_file_set_contents(path, (const gchar *)bytes->data, bytes->len, &error));
    g_assert_no_error(error); g_byte_array_unref(bytes);
}

static void setup(Fixture *fixture, gconstpointer data)
{
    (void)data; gchar *root = g_canonicalize_filename(RADIOS_TEST_PROJECT_ROOT, NULL);
    gchar *build = g_build_filename(root, "build", NULL); g_assert_cmpint(g_mkdir_with_parents(build, 0700), ==, 0);
    fixture->directory = g_build_filename(build, "media-core-XXXXXX", NULL); g_assert_nonnull(g_mkdtemp(fixture->directory));
    fixture->project = g_build_filename(fixture->directory, "Türkçe RadioS proje", NULL); g_assert_cmpint(g_mkdir(fixture->project, 0700), ==, 0);
    fixture->config.project_root = g_strdup(fixture->project);
    fixture->config.import_root = g_build_filename(fixture->project, "data", "içe aktar", NULL);
    fixture->wave = g_build_filename(fixture->directory, "Ölçülen stereo ses.wav", NULL); write_wave(fixture->wave, 48000, 2, FALSE);
    g_free(build); g_free(root);
}
static void teardown(Fixture *fixture, gconstpointer data)
{
    (void)data; remove_tree(fixture->directory); radios_config_clear(&fixture->config);
    g_free(fixture->project); g_free(fixture->wave); g_free(fixture->directory);
}

static RadioSMediaAnalysis *analyze(const gchar *path)
{
    GError *error = NULL; RadioSMediaAnalysis *result = radios_media_analyze(path, NULL, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(result); return result;
}
static void waveform_check(RadioSMediaAnalysis *result)
{
    g_assert_cmpuint(result->waveform->len, >, 0); g_assert_cmpuint(result->waveform->len % 2, ==, 0);
    g_assert_cmpuint(result->waveform->len, <=, RADIOS_MEDIA_MAX_WAVEFORM_BINS * 2);
    g_assert_cmpuint(result->waveform->len / 2, ==, (result->sample_frames + result->waveform_frames_per_bin - 1) / result->waveform_frames_per_bin);
    for (guint i = 0; i < result->waveform->len; i += 2) {
        gfloat low = g_array_index(result->waveform, gfloat, i), high = g_array_index(result->waveform, gfloat, i + 1);
        g_assert_true(isfinite(low) && isfinite(high)); g_assert_cmpfloat(low, <=, high);
        g_assert_cmpfloat(fabs(low), <=, result->peak + 0.000001); g_assert_cmpfloat(fabs(high), <=, result->peak + 0.000001);
    }
}

static void pcm_measurement(Fixture *fixture, gconstpointer data)
{
    (void)data; RadioSMediaAnalysis *result = analyze(fixture->wave);
    g_assert_cmpuint(result->sample_frames, ==, 48000); g_assert_cmpint(result->duration_ms, ==, 1000);
    g_assert_cmpuint(result->sample_rate, ==, 48000); g_assert_cmpuint(result->channels, ==, 2);
    g_assert_cmpstr(result->codec, ==, "pcm_s16le"); g_assert_cmpstr(result->format, ==, "wav");
    g_assert_cmpint(result->bitrate, ==, 1536000);
    g_assert_cmpfloat(fabs(result->peak - 0.5), <, 0.0001);
    g_assert_cmpfloat(fabs(result->rms - sqrt(0.078125)), <, 0.0001); g_assert_false(result->clipped);
    waveform_check(result);
    gchar *bytes = NULL; gsize size = 0; g_assert_true(g_file_get_contents(fixture->wave, &bytes, &size, NULL));
    gchar *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *)bytes, size);
    g_assert_cmpstr(hash, ==, result->sha256); g_assert_cmpuint(result->size_bytes, ==, size);
    JsonNode *node = radios_media_analysis_to_json(result); JsonObject *object = json_node_get_object(node);
    g_assert_cmpuint(json_object_get_size(object), ==, 17); g_assert_false(json_object_has_member(object, "path"));
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(object, "rhythm"), "algorithm"), ==, RADIOS_RHYTHM_ALGORITHM);
    /* The programme loudness travels with every analysis. */
    RadioSLoudness loudness; g_assert_true(radios_loudness_read(object, &loudness));
    g_assert_cmpint(loudness.measured, ==, result->loudness.measured); g_assert_true(radios_media_analysis_complete(object));
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(object, "waveform")), ==, result->waveform->len);
    json_node_free(node); g_free(hash); g_free(bytes); radios_media_analysis_free(result);
}

static void clipping_float(Fixture *fixture, gconstpointer data)
{
    (void)data; gchar *path = g_build_filename(fixture->directory, "float.wav", NULL); write_wave(path, 8000, 2, TRUE);
    RadioSMediaAnalysis *result = analyze(path); waveform_check(result);
    g_assert_cmpfloat(result->peak, ==, 1.25); g_assert_cmpfloat(result->rms, ==, 1.25);
    g_assert_true(result->clipped); g_assert_cmpuint(result->clipped_samples, ==, 16000);
    g_assert_cmpfloat(g_array_index(result->waveform, gfloat, 0), ==, -1.25);
    g_assert_cmpfloat(g_array_index(result->waveform, gfloat, 1), ==, 1.25);
    radios_media_analysis_free(result); g_free(path);
}

static gchar *transcode(Fixture *fixture, const gchar *codec, const gchar *extension)
{
    gchar *path = g_strdup_printf("%s/encoded.%s", fixture->directory, extension);
    gchar *arguments[] = {"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", fixture->wave,
        "-c:a", (gchar *)codec, "-metadata", "title=Gerçek Türkçe başlık", "-metadata", "artist=Sanatçı",
        "-metadata", "album=Albüm", "-metadata", "genre=Test", path, NULL};
    GError *error = NULL; gchar *output = NULL; gint status = 0;
    g_assert_true(g_spawn_sync(fixture->directory, arguments, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &output, &status, &error));
    g_assert_no_error(error); if (!g_spawn_check_wait_status(status, &error)) g_error("Fixture encoding failed: %s", output);
    g_assert_no_error(error); g_free(output); return path;
}
static void compressed_formats(Fixture *fixture, gconstpointer data)
{
    (void)data; const gchar *codecs[] = {"libmp3lame", "flac", "libvorbis", "aac", "aac"};
    const gchar *extensions[] = {"mp3", "flac", "ogg", "m4a", "aac"};
    const gchar *expected_codecs[] = {"mp3", "flac", "vorbis", "aac", "aac"};
    for (guint i = 0; i < G_N_ELEMENTS(codecs); ++i) {
        gchar *path = transcode(fixture, codecs[i], extensions[i]); RadioSMediaAnalysis *result = analyze(path);
        g_test_message("Decoded %s: %" G_GINT64_FORMAT " ms, %" G_GUINT64_FORMAT " frames", extensions[i], result->duration_ms, result->sample_frames);
        g_assert_cmpint(result->duration_ms, >=, 950); g_assert_cmpint(result->duration_ms, <=, 1100);
        g_assert_cmpuint(result->channels, ==, 2); g_assert_cmpuint(result->sample_rate, ==, 48000);
        g_assert_cmpstr(result->codec, ==, expected_codecs[i]);
        g_assert_cmpfloat(result->peak, >, 0.35); g_assert_cmpfloat(result->rms, >, 0.2); waveform_check(result);
        if (i != 4) { g_assert_cmpstr(result->title, ==, "Gerçek Türkçe başlık"); g_assert_cmpstr(result->artist, ==, "Sanatçı");
            g_assert_cmpstr(result->album, ==, "Albüm"); g_assert_cmpstr(result->genre, ==, "Test"); }
        radios_media_analysis_free(result); g_free(path);
    }
}

static void webm_opus_cached_names(Fixture *fixture, gconstpointer data)
{
    (void)data; gchar *encoded = transcode(fixture, "libopus", "webm"), *bytes = NULL; gsize length = 0;
    g_assert_true(g_file_get_contents(encoded, &bytes, &length, NULL));
    gchar *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)bytes, length);
    const gchar *names[] = {"cached.audio", sha};
    /* Cache exports use .audio; managed object names are extensionless SHA.
     * Both must retain the same verified, fully decoded Opus recording. */
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        gchar *path = g_build_filename(fixture->directory, names[i], NULL);
        g_assert_true(g_file_set_contents(path, bytes, length, NULL));
        RadioSMediaAnalysis *result = analyze(path);
        g_assert_cmpstr(result->codec, ==, "opus"); g_assert_nonnull(strstr(result->format, "webm"));
        g_assert_cmpstr(result->sha256, ==, sha); g_assert_cmpuint(result->size_bytes, ==, length);
        g_assert_cmpuint(result->sample_rate, ==, 48000); g_assert_cmpuint(result->channels, ==, 2);
        g_assert_cmpint(result->duration_ms, >=, 950); g_assert_cmpint(result->duration_ms, <=, 1100);
        g_assert_cmpuint(result->sample_frames, >, 45000); g_assert_cmpfloat(result->rms, >, 0.2);
        g_assert_cmpstr(result->title, ==, "Gerçek Türkçe başlık"); waveform_check(result);
        radios_media_analysis_free(result);
        GError *error = NULL;
        result = radios_media_stage_analyze(&fixture->config, path, NULL, NULL, &error);
        g_assert_no_error(error); g_assert_nonnull(result);
        g_assert_true(g_str_has_suffix(result->path, ".audio"));
        g_assert_cmpstr(result->sha256, ==, sha); g_assert_cmpstr(result->codec, ==, "opus");
        waveform_check(result); radios_media_analysis_free(result); g_free(path);
    }
    g_free(sha); g_free(bytes); g_free(encoded);
}

static void private_staging(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL;
    RadioSMediaAnalysis *result = radios_media_stage_analyze(&fixture->config, fixture->wave, NULL, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(result); waveform_check(result);
    g_assert_true(g_str_has_prefix(result->relative_path, "data/içe aktar/")); g_assert_false(g_path_is_absolute(result->relative_path));
    g_assert_cmpstr(result->source_name, ==, "Ölçülen stereo ses.wav");
    struct stat status; g_assert_cmpint(lstat(result->path, &status), ==, 0); g_assert_true(S_ISREG(status.st_mode));
    g_assert_cmpuint(status.st_mode & 0777, ==, 0600);
    gchar *resolved = g_build_filename(fixture->project, result->relative_path, NULL); g_assert_cmpstr(resolved, ==, result->path);
    RadioSMediaAnalysis *repeat = analyze(result->path); g_assert_cmpstr(repeat->sha256, ==, result->sha256);
    radios_media_analysis_free(repeat); radios_media_analysis_free(result); g_free(resolved);
}

static void malformed_truncated(Fixture *fixture, gconstpointer data)
{
    (void)data; gchar *broken = g_build_filename(fixture->directory, "broken.wav", NULL); gchar *bytes = NULL; gsize length = 0;
    g_assert_true(g_file_get_contents(fixture->wave, &bytes, &length, NULL)); g_assert_true(g_file_set_contents(broken, bytes, (gssize)length / 2, NULL)); g_free(bytes);
    GError *error = NULL; g_assert_null(radios_media_analyze(broken, NULL, NULL, &error)); g_assert_error(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_DECODE); g_clear_error(&error);
    g_assert_null(radios_media_stage_analyze(&fixture->config, broken, NULL, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
    GDir *directory = g_dir_open(fixture->config.import_root, 0, &error); g_assert_no_error(error); g_assert_null(g_dir_read_name(directory)); g_dir_close(directory);
    const gchar *codecs[] = {"flac", "libvorbis", "aac"}, *extensions[] = {"flac", "ogg", "m4a"};
    for (guint i = 0; i < G_N_ELEMENTS(codecs); ++i) {
        gchar *path = transcode(fixture, codecs[i], extensions[i]); g_assert_true(g_file_get_contents(path, &bytes, &length, NULL));
        g_assert_true(g_file_set_contents(broken, bytes, (gssize)length * 2 / 3, NULL)); g_free(bytes); bytes = NULL;
        g_assert_null(radios_media_analyze(broken, NULL, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error); g_free(path);
    }
    g_assert_true(g_file_set_contents(broken, "not audio data", -1, NULL));
    g_assert_null(radios_media_analyze(broken, NULL, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error); g_free(broken);
}

static void nonregular_bounds(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL; gchar *link = g_build_filename(fixture->directory, "linked.wav", NULL);
    g_assert_cmpint(symlink(fixture->wave, link), ==, 0); g_assert_null(radios_media_analyze(link, NULL, NULL, &error));
    g_assert_error(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_INVALID_FILE); g_clear_error(&error); g_unlink(link);
    g_assert_cmpint(mkfifo(link, 0600), ==, 0); g_assert_null(radios_media_analyze(link, NULL, NULL, &error));
    g_assert_error(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_INVALID_FILE); g_clear_error(&error); g_unlink(link);
    g_assert_null(radios_media_analyze(fixture->directory, NULL, NULL, &error)); g_clear_error(&error);
    gint fd = open(link, O_WRONLY | O_CREAT | O_EXCL, 0600); g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, (off_t)RADIOS_MEDIA_MAX_SIZE_BYTES + 1), ==, 0); close(fd);
    g_assert_null(radios_media_analyze(link, NULL, NULL, &error)); g_assert_error(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_LIMIT); g_clear_error(&error);
    g_free(link);
}

static void confinement(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL; gchar *outside = g_build_filename(fixture->directory, "outside", NULL);
    g_assert_cmpint(g_mkdir(outside, 0700), ==, 0); g_free(fixture->config.import_root); fixture->config.import_root = g_strdup(outside);
    g_assert_null(radios_media_stage_analyze(&fixture->config, fixture->wave, NULL, NULL, &error));
    g_assert_error(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CONFINEMENT); g_clear_error(&error);
    g_free(fixture->config.import_root); fixture->config.import_root = g_build_filename(fixture->project, "link", "nested", NULL);
    gchar *link = g_build_filename(fixture->project, "link", NULL); g_assert_cmpint(symlink(outside, link), ==, 0);
    g_assert_null(radios_media_stage_analyze(&fixture->config, fixture->wave, NULL, NULL, &error));
    g_assert_error(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CONFINEMENT); g_clear_error(&error);
    GDir *dir = g_dir_open(outside, 0, NULL); g_assert_null(g_dir_read_name(dir)); g_dir_close(dir);
    gchar *parent_link = g_build_filename(fixture->directory, "source-link", NULL); g_assert_cmpint(symlink(fixture->project, parent_link), ==, 0);
    gchar *source = g_build_filename(fixture->project, "source.wav", NULL); write_wave(source, 100, 1, FALSE);
    gchar *aliased = g_build_filename(parent_link, "source.wav", NULL); g_assert_null(radios_media_analyze(aliased, NULL, NULL, &error));
    g_assert_nonnull(error); g_clear_error(&error); g_free(source); g_free(aliased); g_free(parent_link); g_free(link); g_free(outside);
}

typedef struct {const gchar *path; gint started; RadioSMediaAnalysis *result; GError *error;} Work;
static gchar *cancel_work(gpointer data, const RadioSCancelToken *cancel, GError **error)
{
    Work *work = data; (void)error; g_atomic_int_set(&work->started, 1);
    work->result = radios_media_analyze(work->path, cancel, NULL, &work->error); return NULL;
}
static void cancellation(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL; GCancellable *cancellable = g_cancellable_new(); g_cancellable_cancel(cancellable);
    g_assert_null(radios_media_stage_analyze(&fixture->config, fixture->wave, NULL, cancellable, &error));
    g_assert_error(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CANCELLED); g_clear_error(&error);
    g_assert_false(g_file_test(fixture->config.import_root, G_FILE_TEST_EXISTS)); g_object_unref(cancellable);
    Work work = {.path = fixture->wave}; RadioSRuntime *runtime = radios_runtime_new(2, 4);
    g_assert_cmpuint(radios_runtime_submit(runtime, cancel_work, &work, NULL, &error), >, 0); g_assert_no_error(error);
    while (!g_atomic_int_get(&work.started)) g_thread_yield();
    radios_runtime_shutdown(runtime); g_assert_null(work.result);
    g_assert_error(work.error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CANCELLED); g_clear_error(&work.error); radios_runtime_free(runtime);
}

typedef struct {Fixture *fixture; GCancellable *cancel; RadioSMediaAnalysis *result; GError *error; gint done;} Stage;
static gpointer stage_work(gpointer data)
{
    Stage *stage = data;
    stage->result = radios_media_stage_analyze(&stage->fixture->config, stage->fixture->wave, NULL, stage->cancel, &stage->error);
    g_atomic_int_set(&stage->done, 1); return NULL;
}
static void cancellation_cleanup(Fixture *fixture, gconstpointer data)
{
    (void)data; write_wave(fixture->wave, 48000 * 30, 2, FALSE);
    Stage stage = {.fixture = fixture, .cancel = g_cancellable_new()};
    GThread *thread = g_thread_new("fixture-import-cancel", stage_work, &stage); gboolean observed = FALSE;
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!g_atomic_int_get(&stage.done) && !observed && g_get_monotonic_time() < deadline) {
        GDir *dir = g_dir_open(fixture->config.import_root, 0, NULL);
        if (dir) { observed = g_dir_read_name(dir) != NULL; g_dir_close(dir); }
        if (!observed) g_usleep(100);
    }
    g_cancellable_cancel(stage.cancel); g_thread_join(thread); g_assert_true(observed);
    g_assert_null(stage.result); g_assert_error(stage.error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CANCELLED);
    GDir *dir = g_dir_open(fixture->config.import_root, 0, NULL); g_assert_nonnull(dir); g_assert_null(g_dir_read_name(dir)); g_dir_close(dir);
    g_clear_error(&stage.error); g_object_unref(stage.cancel);
}

typedef struct {const gchar *path; gint running, stop;} Mutator;
static gpointer mutate(gpointer data)
{
    Mutator *mutator = data; gint fd = open(mutator->path, O_WRONLY); g_assert_cmpint(fd, >=, 0); guchar byte = 0;
    g_atomic_int_set(&mutator->running, 1);
    while (!g_atomic_int_get(&mutator->stop)) { ++byte; g_assert_cmpint(pwrite(fd, &byte, 1, 100), ==, 1); g_usleep(1000); }
    close(fd); return NULL;
}
static void changed_source(Fixture *fixture, gconstpointer data)
{
    (void)data; write_wave(fixture->wave, 48000 * 30, 2, FALSE);
    Mutator mutator = {.path = fixture->wave}; GThread *thread = g_thread_new("fixture-audio-mutator", mutate, &mutator);
    while (!g_atomic_int_get(&mutator.running)) g_thread_yield();
    GError *error = NULL; RadioSMediaAnalysis *result = radios_media_analyze(fixture->wave, NULL, NULL, &error);
    g_atomic_int_set(&mutator.stop, 1); g_thread_join(thread);
    g_assert_null(result); g_assert_error(error, RADIOS_MEDIA_ERROR, RADIOS_MEDIA_ERROR_CHANGED); g_clear_error(&error);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/media/pcm-measurement", Fixture, NULL, setup, pcm_measurement, teardown);
    g_test_add("/media/float-clipping", Fixture, NULL, setup, clipping_float, teardown);
    g_test_add("/media/compressed-formats", Fixture, NULL, setup, compressed_formats, teardown);
    g_test_add("/media/webm-opus-cache-and-sha-names", Fixture, NULL, setup, webm_opus_cached_names, teardown);
    g_test_add("/media/private-staging", Fixture, NULL, setup, private_staging, teardown);
    g_test_add("/media/malformed-truncated", Fixture, NULL, setup, malformed_truncated, teardown);
    g_test_add("/media/nonregular-bounds", Fixture, NULL, setup, nonregular_bounds, teardown);
    g_test_add("/media/confinement", Fixture, NULL, setup, confinement, teardown);
    g_test_add("/media/cancellation", Fixture, NULL, setup, cancellation, teardown);
    g_test_add("/media/cancellation-cleanup", Fixture, NULL, setup, cancellation_cleanup, teardown);
    g_test_add("/media/source-changed", Fixture, NULL, setup, changed_source, teardown);
    return g_test_run();
}
