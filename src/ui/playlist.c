#define _POSIX_C_SOURCE 200809L
#include "radios/widgets.h"
#include "radios/editors.h"
#include "radios/data.h"
#include "radios/media_ui.h"
#include "radios/playout_ui.h"
#include "radios/media.h"
#include "radios/player_loader.h"
#include "radios/media_jobs.h"
#include "radios/playlist_browser.h"
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    gchar *title, *artist, *album, *duration, *genre, *key, *location, *comment;
    gdouble bpm;
    gchar *id;
    gint64 revision;
    gchar *media_id, *version_id, *media_file_id, *media_sha256;
    gchar *rekordbox_id, *rekordbox_sha256, *rekordbox_cue_policy;
    gint64 rekordbox_cues;
    gint64 duration_ms;
    gint64 intro_ms, cue_in_ms, cue_out_ms, fade_start_ms, fade_end_ms, segue_ms;
    gchar *trigger, *voice_track, *cue_trigger;
    gboolean markers_loaded;
} PlaylistTrack;

typedef struct {
    gchar *name, *purpose;
    GPtrArray *tracks;
    gchar *id, *version_id;
    gint64 revision, version_revision;
} PlaylistDraft;

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *widget;
    GtkWidget *search, *status, *table;
    GtkWidget *lists, *heading, *details, *list_count, *track_count;
    GtkWidget *empty, *source, *source_values[4], *buttons[11], *sort;
    GtkWidget *select_all;
    GtkWidget *play, *import_dialog;
    GtkWidget *target_selector, *browser;
    GtkWidget *marker_host, *marker_panel;
    guint analysis_generation;
    GCancellable *analysis_cancel;
    GPtrArray *drafts;
    gint current;
    guint matches;
    gboolean updating;
    gboolean busy, loading;
    gboolean destroyed;
    gchar *select_uuid;
    GtkListStore *store;
    GtkTreeModelFilter *filter;
} PlaylistView;

enum { PW_RENAME, PW_DELETE, PW_NEW, PW_EDIT, PW_REMOVE, PW_UP, PW_DOWN,
       PW_ADD_SELECTED, PW_ADD_ALL, PW_CSV, PW_M3U };

static void playlist_refresh(PlaylistView *view);
static void playlist_selection_changed(GtkTreeSelection *selection, gpointer data);
static void playlist_data_refresh(RadioSViewContext *ctx, GtkWidget *owner);
static void playlist_to_programme(PlaylistView *view, gboolean selected);
static void playlist_play(GtkButton *button, gpointer data);
static void playlist_markers_open(PlaylistView *view, PlaylistTrack *track);
void radios_automation_playlists_changed(GtkWidget *parent);

static void
track_free(gpointer data)
{
    PlaylistTrack *track = data;
    g_free(track->title); g_free(track->artist); g_free(track->album);
    g_free(track->duration); g_free(track->genre); g_free(track->key);
    g_free(track->location); g_free(track->comment); g_free(track->id);
    g_free(track->rekordbox_id); g_free(track->rekordbox_sha256); g_free(track->rekordbox_cue_policy);
    g_free(track->media_id); g_free(track->version_id); g_free(track->media_file_id); g_free(track->media_sha256);
    g_free(track->trigger); g_free(track->voice_track); g_free(track->cue_trigger); g_free(track);
}

static void
draft_free(gpointer data)
{
    PlaylistDraft *draft = data;
    g_free(draft->name); g_free(draft->purpose);
    g_free(draft->id); g_free(draft->version_id);
    g_ptr_array_unref(draft->tracks);
    g_free(draft);
}

static void
playlist_free(gpointer data)
{
    PlaylistView *view = data;
    g_ptr_array_unref(view->drafts);
    g_free(view->select_uuid);
    if (view->marker_panel) g_object_remove_weak_pointer(G_OBJECT(view->marker_panel), (gpointer *)&view->marker_panel);
    if (view->analysis_cancel) g_cancellable_cancel(view->analysis_cancel);
    g_clear_object(&view->analysis_cancel);
    g_free(view);
}

static void playlist_destroyed(GtkWidget *page, gpointer data)
{
    (void)page; PlaylistView *view = data; view->destroyed = TRUE;
    if (view->ctx->parent && g_object_get_data(G_OBJECT(view->ctx->parent), "radios-playlist-view") == view)
        g_object_set_data(G_OBJECT(view->ctx->parent), "radios-playlist-view", NULL);
    ++view->analysis_generation;
    if (view->analysis_cancel) g_cancellable_cancel(view->analysis_cancel);
}

static PlaylistDraft *
playlist_current(PlaylistView *view)
{
    return view->current >= 0 && view->current < (gint)view->drafts->len ?
        g_ptr_array_index(view->drafts, (guint)view->current) : NULL;
}

static PlaylistTrack *
playlist_selected(PlaylistView *view)
{
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view->table));
    GList *paths = gtk_tree_selection_get_selected_rows(selection, NULL);
    PlaylistTrack *track = NULL;
    GtkTreeIter iter;
    if (paths != NULL && gtk_tree_model_get_iter(GTK_TREE_MODEL(view->filter), &iter, paths->data))
        gtk_tree_model_get(GTK_TREE_MODEL(view->filter), &iter, 7, &track, -1);
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return track;
}

static void
playlist_notice(PlaylistView *view, const gchar *message)
{
    gtk_label_set_text(GTK_LABEL(view->status), message);
    if (view->ctx->notify != NULL)
        view->ctx->notify(view->ctx->notify_data, message);
}

static PlaylistDraft *
playlist_from_json(JsonObject *row)
{
    PlaylistDraft *draft = g_new0(PlaylistDraft, 1);
    draft->name = g_strdup(radios_json_string(row, "name", ""));
    draft->purpose = g_strdup(radios_json_string(row, "purpose", "music"));
    draft->id = g_strdup(radios_json_string(row, "_id", ""));
    draft->version_id = g_strdup(radios_json_string(row, "version_id", ""));
    draft->revision = radios_json_int(row, "_rev", 0);
    draft->version_revision = radios_json_int(row, "version_revision", 0);
    draft->tracks = g_ptr_array_new_with_free_func(track_free);
    JsonArray *tracks = json_object_has_member(row, "tracks") ? json_object_get_array_member(row, "tracks") : NULL;
    for (guint i = 0; tracks && i < json_array_get_length(tracks); ++i) {
        JsonObject *source = json_array_get_object_element(tracks, i);
        PlaylistTrack *track = g_new0(PlaylistTrack, 1);
        track->id = g_strdup(radios_json_string(source, "_id", ""));
        track->revision = radios_json_int(source, "_rev", 0);
        gchar **values[] = {&track->title, &track->artist, &track->album, &track->genre,
                           &track->key, &track->location, &track->comment};
        const gchar *keys[] = {"title", "artist", "album", "genre", "key", "location", "comment"};
        for (guint j = 0; j < G_N_ELEMENTS(keys); ++j)
            *values[j] = g_strdup(radios_json_string(source, keys[j], ""));
        gint64 milliseconds = radios_json_int(source, "duration_ms", 0);
        track->duration_ms = milliseconds;
        track->media_id = g_strdup(radios_json_string(source, "media_id", NULL));
        track->version_id = g_strdup(radios_json_string(source, "version_id", NULL));
        track->media_file_id = g_strdup(radios_json_string(source, "media_file_id", NULL));
        track->media_sha256 = g_strdup(radios_json_string(source, "media_sha256", NULL));
        track->intro_ms = radios_json_int(source, "intro_ms", 0);
        track->cue_in_ms = radios_json_int(source, "cue_in_ms", 0);
        track->cue_out_ms = radios_json_int(source, "cue_out_ms", milliseconds);
        track->fade_start_ms = radios_json_int(source, "fade_start_ms", milliseconds);
        track->fade_end_ms = radios_json_int(source, "fade_end_ms", milliseconds);
        track->segue_ms = radios_json_int(source, "segue_ms", milliseconds);
        track->trigger = g_strdup(radios_json_string(source, "trigger", ""));
        track->voice_track = g_strdup(radios_json_string(source, "voice_track", ""));
        track->cue_trigger = g_strdup(radios_json_string(source, "cue_trigger", ""));
        track->rekordbox_id = g_strdup(radios_json_string(source, "rekordbox_track_id", ""));
        track->rekordbox_sha256 = g_strdup(radios_json_string(source, "rekordbox_sha256", ""));
        track->rekordbox_cue_policy = g_strdup(radios_json_string(source, "rekordbox_cue_policy", ""));
        track->rekordbox_cues = radios_json_int(source, "rekordbox_cue_marks", 0);
        track->duration = milliseconds > 0 ? g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
            milliseconds / 60000, (milliseconds / 1000) % 60) : g_strdup("");
        track->bpm = json_object_has_member(source, "bpm") ? json_object_get_double_member(source, "bpm") : 0;
        g_ptr_array_add(draft->tracks, track);
    }
    return draft;
}

static void
playlist_apply_json(PlaylistView *view, JsonObject *row)
{
    const gchar *id = radios_json_string(row, "_id", radios_json_string(row, "playlist_id", ""));
    PlaylistTrack *selected = playlist_selected(view);
    g_free(view->select_uuid); view->select_uuid = selected ? g_strdup(selected->id) : NULL;
    gtk_list_store_clear(view->store);
    gint index = -1;
    for (guint i = 0; i < view->drafts->len; ++i) {
        PlaylistDraft *candidate = g_ptr_array_index(view->drafts, i);
        if (g_strcmp0(candidate->id, id) == 0) { index = (gint)i; break; }
    }
    if (json_object_has_member(row, "deleted") && json_object_get_boolean_member(row, "deleted")) {
        if (index >= 0) g_ptr_array_remove_index(view->drafts, (guint)index);
        view->current = view->drafts->len > 0 ? 0 : -1;
    } else {
        PlaylistDraft *draft = playlist_from_json(row);
        if (index >= 0) { draft_free(g_ptr_array_index(view->drafts, (guint)index)); g_ptr_array_index(view->drafts, (guint)index) = draft; }
        else { g_ptr_array_add(view->drafts, draft); index = (gint)view->drafts->len - 1; }
        view->current = index;
    }
    playlist_refresh(view);
    radios_automation_playlists_changed(GTK_WIDGET(view->ctx->parent));
}

typedef struct { GWeakRef page; gboolean finished; gchar *created_name; } PlaylistWrite;

static void
playlist_write_free(gpointer data)
{
    PlaylistWrite *write = data;
    GtkWidget *page = g_weak_ref_get(&write->page);
    if (page && !g_object_get_data(G_OBJECT(page), "radios-data-destroyed")) {
        PlaylistView *view = g_object_get_data(G_OBJECT(page), "playlist-view");
        view->busy = FALSE;
        playlist_refresh(view);
        if (!write->finished) playlist_data_refresh(view->ctx, page);
    }
    g_clear_object(&page); g_weak_ref_clear(&write->page); g_free(write->created_name); g_free(write);
}

static void
playlist_saved(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result,
               const GError *error, gpointer data)
{
    PlaylistWrite *write = data;
    GtkWidget *page = g_weak_ref_get(&write->page);
    if (!page || g_object_get_data(G_OBJECT(page), "radios-data-destroyed")) { g_clear_object(&page); return; }
    PlaylistView *view = g_object_get_data(G_OBJECT(page), "playlist-view");
    write->finished = TRUE;
    GtkWidget *status = g_object_get_data(G_OBJECT(owner), "pw-save-status");
    if (GTK_IS_DIALOG(owner)) gtk_widget_set_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(owner), GTK_RESPONSE_ACCEPT), TRUE);
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        const gchar *message = error ? error->message : "Veri servisi geçerli kayıt döndürmedi.";
        if (status) gtk_label_set_text(GTK_LABEL(status), message);
        playlist_notice(view, message);
        g_clear_object(&page); return;
    }
    view->busy = FALSE;
    playlist_apply_json(view, json_node_get_object(result));
    g_object_set_data(G_OBJECT(owner), "pw-request-hash", NULL);
    g_object_set_data(G_OBJECT(owner), "pw-request-key", NULL);
    const gchar *warning = radios_json_string(json_node_get_object(result), "warning", "");
    playlist_notice(view, *warning ? warning : "Playlist veritabanına kaydedildi.");
    if (GTK_IS_DIALOG(owner)) gtk_widget_destroy(owner);
    (void)ctx; g_clear_object(&page);
}

static gboolean
playlist_write(PlaylistView *view, GtkWidget *owner, const gchar *operation, JsonNode *body)
{
    if (view->busy || view->loading || !radios_data_connected(view->ctx)) {
        playlist_notice(view, "Veri bağlantısı/kayıt işlemi hazır değil; girdiler korunuyor."); return FALSE;
    }
    JsonGenerator *generator = json_generator_new(); json_generator_set_root(generator, body);
    gchar *serialized = json_generator_to_data(generator, NULL); g_object_unref(generator);
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, serialized, -1); g_free(serialized);
    const gchar *previous = g_object_get_data(G_OBJECT(owner), "pw-request-hash");
    if (g_strcmp0(previous, hash) != 0) {
        g_object_set_data_full(G_OBJECT(owner), "pw-request-hash", g_strdup(hash), g_free);
        g_object_set_data_full(G_OBJECT(owner), "pw-request-key", g_uuid_string_random(), g_free);
    }
    g_free(hash);
    json_object_set_string_member(json_node_get_object(body), "idempotency_key", g_object_get_data(G_OBJECT(owner), "pw-request-key"));
    PlaylistWrite *write = g_new0(PlaylistWrite, 1); g_weak_ref_init(&write->page, view->widget);
    if (g_str_equal(operation, "create")) write->created_name = g_strdup(radios_json_string(json_node_get_object(body), "name", ""));
    gchar *path = g_strdup_printf("/v1/workflows/desktop-playlists/%s", operation);
    view->busy = TRUE;
    if (GTK_IS_DIALOG(owner)) gtk_widget_set_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(owner), GTK_RESPONSE_ACCEPT), FALSE);
    gboolean queued = radios_data_request(view->ctx, owner, "POST", path, body, playlist_saved, write, playlist_write_free);
    g_free(path); playlist_refresh(view);
    if (queued) playlist_notice(view, "Playlist değişikliği kaydediliyor…");
    else if (GTK_IS_DIALOG(owner)) gtk_widget_set_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(owner), GTK_RESPONSE_ACCEPT), TRUE);
    return queued;
}

static JsonNode *
playlist_body(PlaylistView *view, const gchar *id, gint64 revision)
{
    JsonNode *body = radios_data_body(view->ctx); JsonObject *object = json_node_get_object(body);
    if (id) { json_object_set_string_member(object, "playlist_id", id); json_object_set_int_member(object, "expected_revision", revision); }
    return body;
}

static gchar *playlist_clock(gint64 milliseconds)
{
    gint64 seconds = MAX((gint64)0, milliseconds) / 1000;
    return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
        seconds / 3600, seconds / 60 % 60, seconds % 60);
}

static GdkPixbuf *playlist_info_icon(void)
{
    static GdkPixbuf *icon; static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 18, 18);
        cairo_t *cr = cairo_create(surface);
        cairo_pattern_t *gradient = cairo_pattern_create_linear(0, 1, 0, 17);
        cairo_pattern_add_color_stop_rgb(gradient, 0, 0.20, 0.20, 0.20);
        cairo_pattern_add_color_stop_rgb(gradient, 0.42, 0.12, 0.12, 0.12);
        cairo_pattern_add_color_stop_rgb(gradient, 1, 0.06, 0.06, 0.06);
        cairo_arc(cr, 9, 9, 8, 0, 2 * G_PI); cairo_set_source(cr, gradient); cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.40, 0.40, 0.40); cairo_set_line_width(cr, 0.7); cairo_stroke(cr);
        cairo_set_source_rgb(cr, 0.212, 0.792, 1.0); cairo_arc(cr, 9, 4.8, 1.1, 0, 2 * G_PI); cairo_fill(cr);
        cairo_rectangle(cr, 8.1, 7.3, 1.8, 6.7); cairo_fill(cr);
        icon = gdk_pixbuf_get_from_surface(surface, 0, 0, 18, 18);
        cairo_pattern_destroy(gradient); cairo_destroy(cr); cairo_surface_destroy(surface);
        g_once_init_leave(&initialized, 1);
    }
    return icon;
}

static void playlist_builder_cell(GtkTreeViewColumn *column, GtkCellRenderer *cell,
    GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)column;
    PlaylistView *view = g_object_get_data(G_OBJECT(cell), "playlist-view");
    PlaylistTrack *track = NULL; gint ordinal = 0;
    gtk_tree_model_get(model, iter, 7, &track, 1, &ordinal, -1);
    if (!track) return;
    gint field = GPOINTER_TO_INT(data);
    gchar *owned = NULL; const gchar *value = "";
    if (field == 0) { owned = g_strdup_printf("%d", ordinal); value = owned; }
    else if (field == 1) {
        gint64 elapsed = 0; PlaylistDraft *draft = playlist_current(view);
        for (guint i = 0; draft && i < draft->tracks->len && g_ptr_array_index(draft->tracks, i) != track; ++i)
            elapsed += MAX((gint64)0, ((PlaylistTrack *)g_ptr_array_index(draft->tracks, i))->duration_ms);
        owned = playlist_clock(elapsed); value = owned;
    } else if (field == 2) value = track->artist ? track->artist : "";
    else if (field == 3) value = track->title ? track->title : "";
    else if (field == 4 || field == 6 || field == 10) {
        if (track->markers_loaded) {
            owned = playlist_clock(field == 4 ? track->intro_ms : field == 6 ? track->segue_ms : track->cue_in_ms);
            value = owned;
        } else value = "—";
    } else if (field == 5) { owned = playlist_clock(track->duration_ms); value = track->duration_ms > 0 ? owned : "—"; }
    else if (field == 7) value = track->trigger ? track->trigger : "";
    else if (field == 8) value = track->voice_track ? track->voice_track : "";
    else if (field == 9) value = track->cue_trigger ? track->cue_trigger : "";
    gboolean selected = gtk_tree_selection_iter_is_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(view->table)), iter);
    if (field == 11) {
        g_object_set(cell, "pixbuf", playlist_info_icon(), "cell-background", selected ? "#252525" : ordinal % 2 ? "#080808" : "#101010", NULL);
        return;
    }
    g_object_set(cell, "text", value, "cell-background", selected ? "#252525" : field == 0 ? "#181818" : ordinal % 2 ? "#080808" : "#101010",
        "foreground", field == 11 ? "#36caff" : selected ? "#ededed" : "#b5b5b5", "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    g_free(owned);
}

typedef struct {
    GWeakRef page;
    guint generation;
    JsonNode *detail;
    gchar *media_id, *version_id, *sha256;
} PlaylistAnalysis;

static void playlist_analysis_free(gpointer data)
{
    PlaylistAnalysis *job = data; g_weak_ref_clear(&job->page);
    g_clear_pointer(&job->detail, json_node_free);
    g_free(job->media_id); g_free(job->version_id); g_free(job->sha256); g_free(job);
}

static void playlist_analysis_worker(GTask *task, gpointer owner, gpointer data, GCancellable *cancel)
{
    (void)owner; PlaylistAnalysis *job = data;
    JsonObject *version = json_object_get_object_member(json_node_get_object(job->detail), "version");
    GError *error = NULL;
    RadioSMediaAnalysis *analysis = radios_media_analyze(radios_json_string(version, "managed_path", ""), NULL, cancel, &error);
    if (analysis && g_strcmp0(analysis->sha256, job->sha256))
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Seçilen parçanın ses dosyası değişmiş; işaretler açılmadı.");
    if (error) { radios_media_analysis_free(analysis); g_task_return_error(task, error); return; }
    JsonNode *measured = radios_media_analysis_to_json(analysis); radios_media_analysis_free(analysis);
    json_object_set_member(json_node_get_object(job->detail), "analysis", measured);
    g_task_return_boolean(task, TRUE);
}

static GtkWidget *playlist_find_named(GtkWidget *widget, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(widget), name)) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget)); GtkWidget *found = NULL;
    for (GList *child = children; child && !found; child = child->next) found = playlist_find_named(child->data, name);
    g_list_free(children); return found;
}

static void playlist_marker_close(GtkButton *button, gpointer data)
{
    (void)button; PlaylistView *view = data;
    if (view->marker_panel) {
        g_object_remove_weak_pointer(G_OBJECT(view->marker_panel), (gpointer *)&view->marker_panel);
        gtk_widget_destroy(view->marker_panel); view->marker_panel = NULL;
        gtk_widget_hide(view->marker_host);
        playlist_notice(view, "İşaret düzenleyicisi kapatıldı.");
    }
}

static void playlist_analysis_done(GObject *owner, GAsyncResult *result, gpointer unused)
{
    (void)owner; (void)unused; GTask *task = G_TASK(result); PlaylistAnalysis *job = g_task_get_task_data(task);
    GError *error = NULL; gboolean okay = g_task_propagate_boolean(task, &error);
    GtkWidget *page = g_weak_ref_get(&job->page);
    PlaylistView *view = page ? g_object_get_data(G_OBJECT(page), "playlist-view") : NULL;
    if (!view || view->destroyed || gtk_widget_in_destruction(page) || view->analysis_generation != job->generation) goto done;
    if (!okay) { playlist_notice(view, error ? error->message : "Parça analizi tamamlanamadı."); goto done; }
    JsonObject *detail = json_node_get_object(job->detail);
    JsonObject *markers = json_object_has_member(detail, "markers") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(detail, "markers")) ?
        json_object_get_object_member(detail, "markers") : NULL;
    JsonObject *values = markers;
    for (guint i = 0; i < view->drafts->len; ++i) {
        PlaylistDraft *draft = g_ptr_array_index(view->drafts, i);
        for (guint j = 0; j < draft->tracks->len; ++j) {
            PlaylistTrack *track = g_ptr_array_index(draft->tracks, j);
            if (g_strcmp0(track->media_id, job->media_id) || g_strcmp0(track->version_id, job->version_id)) continue;
            track->cue_in_ms = radios_json_int(values, "cue_in_ms", 0);
            track->intro_ms = radios_json_int(values, "intro_ms", 0);
            track->cue_out_ms = radios_json_int(values, "out_ms", track->duration_ms);
            track->segue_ms = radios_json_int(values, "auto_next_ms", track->duration_ms);
            track->markers_loaded = TRUE;
        }
    }
    gtk_widget_queue_draw(view->table);
    playlist_notice(view, "Ses analizi tamamlandı. Parça işaretlerini düzenleyebilirsiniz.");
    if (view->marker_panel) {
        g_object_remove_weak_pointer(G_OBJECT(view->marker_panel), (gpointer *)&view->marker_panel);
        gtk_widget_destroy(view->marker_panel); view->marker_panel = NULL;
    }
    view->marker_panel = radios_marker_editor_embed(view->ctx, view->widget, detail);
    if (view->marker_panel) {
        g_object_add_weak_pointer(G_OBJECT(view->marker_panel), (gpointer *)&view->marker_panel);
        GtkWidget *save = playlist_find_named(view->marker_panel, "qwf-markers-save");
        GtkWidget *footer_parent = save ? gtk_widget_get_parent(save) : NULL;
        if (GTK_IS_BOX(footer_parent)) {
            GtkWidget *footer = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
            GtkWidget *close = radios_ui_button("Vazgeç / Kapat"); gtk_widget_set_name(close, "pw-marker-close");
            g_signal_connect(close, "clicked", G_CALLBACK(playlist_marker_close), view);
            g_object_ref(save); gtk_container_remove(GTK_CONTAINER(footer_parent), save);
            gtk_box_pack_end(GTK_BOX(footer), save, FALSE, FALSE, 0); gtk_box_pack_end(GTK_BOX(footer), close, FALSE, FALSE, 0);
            g_object_unref(save); gtk_box_pack_start(GTK_BOX(footer_parent), footer, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(view->marker_host), view->marker_panel, FALSE, FALSE, 0);
        gtk_widget_show(view->marker_host);
        gtk_widget_show_all(view->marker_host);
    }
done:
    g_clear_error(&error); g_clear_object(&page);
}

static void playlist_analysis_detail(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result,
    const GError *error, gpointer data)
{
    (void)ctx; PlaylistAnalysis *job = data; PlaylistView *view = g_object_get_data(G_OBJECT(owner), "playlist-view");
    if (view->analysis_generation != job->generation) return;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        playlist_notice(view, error ? error->message : "Medya ayrıntısı alınamadı."); return;
    }
    JsonObject *detail = json_node_get_object(result);
    JsonObject *version = json_object_has_member(detail, "version") ? json_object_get_object_member(detail, "version") : NULL;
    if (!version || g_strcmp0(radios_json_string(version, "_id", ""), job->version_id) ||
        g_strcmp0(radios_json_string(version, "sha256", ""), job->sha256) ||
        !json_object_get_boolean_member(version, "verified") || !json_object_get_boolean_member(version, "object_available")) {
        playlist_notice(view, "Playlistin doğrulanmış medya sürümü erişilebilir değil; işaretler açılmadı."); return;
    }
    PlaylistAnalysis *worker = g_new0(PlaylistAnalysis, 1); g_weak_ref_init(&worker->page, owner);
    worker->generation = job->generation; worker->detail = json_node_copy(result);
    worker->media_id = g_strdup(job->media_id); worker->version_id = g_strdup(job->version_id); worker->sha256 = g_strdup(job->sha256);
    GTask *task = g_task_new(NULL, view->analysis_cancel, playlist_analysis_done, NULL);
    g_task_set_task_data(task, worker, playlist_analysis_free); g_task_run_in_thread(task, playlist_analysis_worker); g_object_unref(task);
}

static void playlist_markers_open(PlaylistView *view, PlaylistTrack *track)
{
    if (!track || view->destroyed) return;
    if (view->marker_panel && radios_marker_editor_dirty(view->marker_panel)) {
        playlist_notice(view, "Başka parçayı analiz etmeden önce alttaki işaret değişikliklerini kaydedin."); return;
    }
    if (view->analysis_cancel) g_cancellable_cancel(view->analysis_cancel);
    g_clear_object(&view->analysis_cancel); ++view->analysis_generation;
    if (view->marker_panel) {
        g_object_remove_weak_pointer(G_OBJECT(view->marker_panel), (gpointer *)&view->marker_panel);
        gtk_widget_destroy(view->marker_panel); view->marker_panel = NULL;
    }
    const gchar *media_id = track->media_id, *version_id = track->version_id, *sha256 = track->media_sha256;
    if (!media_id || !*media_id || !radios_data_connected(view->ctx)) {
        playlist_notice(view, "Analiz için parçayı medya kütüphanesinden doğrulanmış bir ses dosyasına bağlayın."); return;
    }
    view->analysis_cancel = g_cancellable_new();
    PlaylistAnalysis *job = g_new0(PlaylistAnalysis, 1); g_weak_ref_init(&job->page, view->widget);
    job->generation = view->analysis_generation; job->media_id = g_strdup(media_id);
    job->version_id = g_strdup(version_id); job->sha256 = g_strdup(sha256);
    JsonNode *body = radios_data_body(view->ctx); json_object_set_string_member(json_node_get_object(body), "media_id", media_id);
    playlist_notice(view, "Şarkı analiz ediliyor…");
    if (!radios_data_request(view->ctx, view->widget, "POST", "/v1/workflows/desktop-media/get", body,
            playlist_analysis_detail, job, playlist_analysis_free)) playlist_notice(view, "Analiz başlatılamadı; veri bağlantısını kontrol edin.");
    json_node_free(body);
}

static void playlist_marker_clicked(GtkButton *button, gpointer data)
{ (void)button; PlaylistView *view = data; playlist_markers_open(view, playlist_selected(view)); }

static void playlist_row_activated(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column, gpointer data)
{
    (void)column; PlaylistView *view = data; GtkTreeIter iter; PlaylistTrack *track = NULL;
    if (gtk_tree_model_get_iter(gtk_tree_view_get_model(tree), &iter, path))
        gtk_tree_model_get(gtk_tree_view_get_model(tree), &iter, 7, &track, -1);
    playlist_markers_open(view, track);
}

static gboolean playlist_row_clicked(GtkWidget *tree, GdkEventButton *event, gpointer data)
{
    if (event->button != 1 || event->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK)) return FALSE;
    GtkTreePath *path = NULL; GtkTreeViewColumn *column = NULL; PlaylistView *view = data;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree), (gint)event->x, (gint)event->y, &path, &column, NULL, NULL)) {
        if (tree != view->table || column != gtk_tree_view_get_column(GTK_TREE_VIEW(tree), 0))
            playlist_row_activated(GTK_TREE_VIEW(tree), path, column, data);
        gtk_tree_path_free(path);
    }
    return FALSE;
}

static void
playlist_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer unused)
{
    PlaylistView *view = g_object_get_data(G_OBJECT(owner), "playlist-view"); (void)unused;
    view->loading = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { if (error) playlist_notice(view, error->message); return; }
    if (view->busy) return;
    PlaylistDraft *current = playlist_current(view); gchar *id = current ? g_strdup(current->id) : NULL;
    gtk_list_store_clear(view->store); g_ptr_array_set_size(view->drafts, 0); view->current = -1;
    JsonArray *rows = json_object_get_array_member(json_node_get_object(result), "playlists");
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        PlaylistDraft *draft = playlist_from_json(json_array_get_object_element(rows, i));
        g_ptr_array_add(view->drafts, draft);
        if (g_strcmp0(id, draft->id) == 0) view->current = (gint)i;
    }
    if (view->current < 0 && view->drafts->len > 0) view->current = 0;
    /* Readiness for other views and tests; the page shows no load notice. */
    g_object_set_data(G_OBJECT(owner), "pw-list-loaded", GINT_TO_POINTER(TRUE));
    g_free(id); playlist_refresh(view); radios_automation_playlists_changed(GTK_WIDGET(ctx->parent));
}

static void
playlist_data_refresh(RadioSViewContext *ctx, GtkWidget *owner)
{
    PlaylistView *view = g_object_get_data(G_OBJECT(owner), "playlist-view");
    if (view->busy || view->loading || !radios_data_connected(ctx)) return;
    JsonNode *body = radios_data_body(ctx); view->loading = TRUE;
    if (!radios_data_request(ctx, owner, "POST", "/v1/workflows/desktop-playlists/list", body, playlist_loaded, NULL, NULL)) view->loading = FALSE;
    json_node_free(body);
}

static const gchar *playlist_purpose_name(const gchar *purpose)
{
    const gchar *ids[] = {"music", "jingle", "advert", "sweeper", "announcement", "programme", "other"};
    const gchar *names[] = {"Ana müzik", "Jingle", "Reklam", "Sweeper", "Anons", "Program", "Diğer"};
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i)
        if (!g_strcmp0(purpose, ids[i])) return names[i];
    return names[0];
}

static void playlist_create_response(GtkDialog *dialog, gint response, gpointer data)
{
    PlaylistView *view = data;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "pw-new-name");
    GtkWidget *purpose = g_object_get_data(G_OBJECT(dialog), "pw-new-purpose");
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "pw-save-status");
    gchar *name = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))); g_strstrip(name);
    if (!*name) {
        gtk_label_set_text(GTK_LABEL(status), "Playlist adını girin.");
        gtk_widget_grab_focus(entry); g_free(name); return;
    }
    const gchar *kind = gtk_combo_box_get_active_id(GTK_COMBO_BOX(purpose));
    if (view->ctx->data) {
        JsonNode *body = playlist_body(view, NULL, 0);
        json_object_set_string_member(json_node_get_object(body), "name", name);
        json_object_set_string_member(json_node_get_object(body), "purpose", kind ? kind : "music");
        playlist_write(view, GTK_WIDGET(dialog), "create", body); json_node_free(body);
    } else {
        PlaylistDraft *draft = g_new0(PlaylistDraft, 1);
        draft->name = g_strdup(name); draft->purpose = g_strdup(kind ? kind : "music");
        draft->tracks = g_ptr_array_new_with_free_func(track_free);
        g_ptr_array_add(view->drafts, draft); view->current = (gint)view->drafts->len - 1;
        playlist_refresh(view); gtk_widget_destroy(GTK_WIDGET(dialog));
        playlist_notice(view, "Yeni playlist hazır. Üstten parçaları seçip buraya sürükleyin.");
    }
    g_free(name);
}

static void playlist_create_open(GtkButton *button, gpointer data)
{
    (void)button; PlaylistView *view = data;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Playlist ekle", view->ctx->parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL,
        "Playlist oluştur", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(dialog, "pw-create-dialog");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "pw-dialog");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, -1);
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(body), 18); gtk_box_set_spacing(GTK_BOX(body), 10);
    GtkWidget *entry = radios_ui_entry("Playlist adı"); gtk_widget_set_name(entry, "pw-new-name");
    gtk_entry_set_max_length(GTK_ENTRY(entry), 120); gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    GtkWidget *purpose = gtk_combo_box_text_new(); gtk_widget_set_name(purpose, "pw-new-purpose");
    const gchar *ids[] = {"music", "jingle", "advert", "sweeper", "announcement", "programme", "other"};
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(purpose), ids[i], playlist_purpose_name(ids[i]));
    gtk_combo_box_set_active(GTK_COMBO_BOX(purpose), 0);
    gtk_box_pack_start(GTK_BOX(body), radios_ui_label("Playlist adı", "form-label"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), radios_ui_label("Ne için kullanılacak?", "form-label"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), purpose, FALSE, FALSE, 0);
    GtkWidget *status = radios_ui_label("", "pw-status"); gtk_box_pack_start(GTK_BOX(body), status, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(dialog), "pw-new-name", entry);
    g_object_set_data(G_OBJECT(dialog), "pw-new-purpose", purpose);
    g_object_set_data(G_OBJECT(dialog), "pw-save-status", status);
    g_signal_connect_object(view->widget, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    g_signal_connect(dialog, "response", G_CALLBACK(playlist_create_response), view);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_widget_show_all(dialog); gtk_widget_grab_focus(entry);
}

static gboolean
playlist_matches(GtkTreeModel *model, GtkTreeIter *iter, const gchar *needle)
{
    gboolean visible = *needle == '\0';
    for (gint column = 2; !visible && column < 5; ++column) {
        gchar *text = NULL;
        gchar *folded;
        gtk_tree_model_get(model, iter, column, &text, -1);
        folded = g_utf8_casefold(text != NULL ? text : "", -1);
        visible = g_strstr_len(folded, -1, needle) != NULL;
        g_free(folded);
        g_free(text);
    }
    return visible;
}

static gboolean
playlist_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    gboolean visible;
    (void)data;
    gtk_tree_model_get(model, iter, 9, &visible, -1);
    return visible;
}

static void
playlist_update_filter(PlaylistView *view)
{
    gchar *needle = g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(view->search)), -1);
    GtkTreeIter iter;
    GtkTreeModel *model = GTK_TREE_MODEL(view->store);
    gboolean updating = view->updating;
    view->updating = TRUE;
    view->matches = 0;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        gboolean match = playlist_matches(model, &iter, needle);
        if (match) view->matches++;
        gtk_list_store_set(view->store, &iter, 9, match, -1);
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    gtk_tree_model_filter_refilter(view->filter);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->select_all), FALSE);
    PlaylistDraft *draft = playlist_current(view);
    gtk_label_set_text(GTK_LABEL(view->empty), draft != NULL && draft->tracks->len > 0 ?
                       "Aramaya uyan parça yok." : "");
    gtk_widget_set_visible(view->empty, view->matches == 0);
    g_free(needle);
    view->updating = updating;
    if (!updating)
        playlist_selection_changed(gtk_tree_view_get_selection(GTK_TREE_VIEW(view->table)), view);
}

static void
playlist_search_changed(GtkEditable *entry, gpointer data)
{
    PlaylistView *view = data;
    (void)entry;
    playlist_update_filter(view);
}

static void
playlist_sort(GtkComboBox *combo, gpointer data)
{
    PlaylistView *view = data;
    const gint columns[] = { 1, 2, 8, 3, 6, 5 };
    gint index = gtk_combo_box_get_active(combo);
    if (index >= 0 && index < (gint)G_N_ELEMENTS(columns))
        gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(view->store),
                                             columns[index], GTK_SORT_ASCENDING);
    playlist_update_filter(view);
}

static gint
playlist_numeric_sort(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data)
{
    PlaylistTrack *first, *second;
    gtk_tree_model_get(model, a, 7, &first, -1);
    gtk_tree_model_get(model, b, 7, &second, -1);
    if (first == NULL || second == NULL) return 0;
    gdouble x = first->bpm, y = second->bpm;
    if (GPOINTER_TO_INT(data) == 6) {
        guint minutes = 0, seconds = 0;
        x = sscanf(first->duration, "%u:%u", &minutes, &seconds) == 2 ? minutes * 60.0 + seconds : 0;
        y = sscanf(second->duration, "%u:%u", &minutes, &seconds) == 2 ? minutes * 60.0 + seconds : 0;
    }
    return x < y ? -1 : x > y ? 1 : 0;
}

static void
playlist_selection_changed(GtkTreeSelection *selection, gpointer data)
{
    PlaylistView *view = data;
    if (view->updating)
        return;
    gint count = gtk_tree_selection_count_selected_rows(selection);
    PlaylistTrack *track = playlist_selected(view);
    gboolean writable = !view->ctx->data || (!view->busy && !view->loading && radios_data_connected(view->ctx));
    gtk_widget_set_sensitive(view->buttons[PW_EDIT], count == 1 && writable);
    gtk_widget_set_sensitive(view->buttons[PW_REMOVE], count > 0 && writable);
    gtk_widget_set_sensitive(view->buttons[PW_ADD_SELECTED], count > 0);
    PlaylistDraft *draft = playlist_current(view);
    gint position = -1;
    if (draft != NULL)
        for (guint i = 0; i < draft->tracks->len; ++i)
            if (g_ptr_array_index(draft->tracks, i) == track)
                position = (gint)i;
    gtk_widget_set_sensitive(view->buttons[PW_UP], count == 1 && position > 0 && writable);
    gtk_widget_set_sensitive(view->buttons[PW_DOWN], count == 1 && draft != NULL &&
                             position >= 0 && position + 1 < (gint)draft->tracks->len && writable);
    if (track != NULL) {
        gchar *cues = track->rekordbox_id && *track->rekordbox_id ? g_strdup_printf("%s · %" G_GINT64_FORMAT " kaynak cue · %s",
            *track->key ? track->key : "Ton yok", track->rekordbox_cues,
            g_strcmp0(track->rekordbox_cue_policy, "ignore") == 0 ? "cue aktarımı kapalı" :
            g_strcmp0(track->rekordbox_cue_policy, "propose_radio_cues") == 0 ? "radyo cue incelemesi bekliyor" : "DJ kaynak bilgisi") : g_strdup(track->key);
        const gchar *values[] = { track->location, track->rekordbox_id ? track->rekordbox_id : "", cues, track->comment };
        for (guint i = 0; i < G_N_ELEMENTS(values); ++i)
            gtk_label_set_text(GTK_LABEL(view->source_values[i]), *values[i] != '\0' ? values[i] : "—");
        gtk_widget_set_tooltip_text(view->source_values[1], track->rekordbox_sha256 && *track->rekordbox_sha256 ? track->rekordbox_sha256 : NULL);
        g_free(cues);
    }
    gtk_widget_set_visible(view->source, track != NULL);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(view->filter), &iter);
    while (valid) {
        GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(view->filter), &iter);
        GtkTreeIter child;
        gtk_tree_model_filter_convert_iter_to_child_iter(view->filter, &child, &iter);
        gtk_list_store_set(view->store, &child, 0,
                           gtk_tree_selection_path_is_selected(selection, path), -1);
        gtk_tree_path_free(path);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(view->filter), &iter);
    }
}

static void
playlist_select_all(GtkToggleButton *button, gpointer data)
{
    PlaylistView *view = data;
    if (view->updating) return;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view->table));
    if (gtk_toggle_button_get_active(button))
        gtk_tree_selection_select_all(selection);
    else
        gtk_tree_selection_unselect_all(selection);
}

static void
playlist_choose(GtkButton *button, gpointer data)
{
    PlaylistView *view = data;
    if (view->analysis_cancel) g_cancellable_cancel(view->analysis_cancel);
    ++view->analysis_generation;
    view->current = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "pw-draft-index")) - 1;
    gtk_entry_set_text(GTK_ENTRY(view->search), "");
    playlist_refresh(view);
}

static void playlist_target_changed(GtkComboBox *combo, gpointer data)
{
    PlaylistView *view = data; if (view->updating) return;
    gint selected = gtk_combo_box_get_active(combo);
    if (selected >= 0 && selected < (gint)view->drafts->len) {
        if (view->analysis_cancel) g_cancellable_cancel(view->analysis_cancel);
        ++view->analysis_generation; view->current = selected; playlist_refresh(view);
    }
}

static void playlist_builder_style(GtkWidget *page)
{
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        GtkCssProvider *css = gtk_css_provider_new();
        /* Match the application's charcoal surfaces and cyan focus accent. */
        gtk_css_provider_load_from_data(css,
            ".playlist-workspace {background-color:#000000;color:#b5b5b5;font-size:14px;}"
            ".playlist-workspace label,.radios.pw-dialog label {color:#b5b5b5;margin:0;}"
            ".playlist-workspace button,.radios.pw-dialog button {min-height:22px;padding:3px 6px;background-image:none;background-color:#101010;color:#c8c8c8;border:1px solid #3b3b3b;}"
            ".playlist-workspace entry,.radios.pw-dialog entry {min-height:22px;padding:3px 6px;background-image:none;background-color:#070707;color:#c8c8c8;border:1px solid #444444;}"
            ".playlist-workspace button:hover,.radios.pw-dialog button:hover {background-color:#222222;border-color:#777777;}"
            ".playlist-workspace button:checked,.radios.pw-dialog button:checked {background-color:#292929;color:#ededed;border-color:#777777;}"
            ".playlist-workspace button:focus,.playlist-workspace entry:focus,.radios.pw-dialog button:focus,.radios.pw-dialog entry:focus {border-color:#36caff;}"
            ".playlist-workspace button:disabled,.playlist-workspace entry:disabled,.radios.pw-dialog button:disabled,.radios.pw-dialog entry:disabled {background-color:#090909;color:#707070;border-color:#282828;}"
            ".playlist-workspace button:disabled label,.radios.pw-dialog button:disabled label {color:#707070;}"
            ".playlist-workspace .pw-summary,.playlist-workspace .pw-playlist-heading,.playlist-workspace .pw-pagination {margin:0;padding:4px;background-color:#080808;}"
            ".playlist-workspace .pw-heading {background-image:none;background-color:#111111;border-bottom:1px solid #333333;padding:12px;margin:0;}"
            ".playlist-workspace .pw-heading label {color:#ededed;}"
            ".playlist-workspace .pw-heading .h2 {font-size:26px;}"
            ".playlist-workspace .pw-layout {margin:0;padding:0;background-color:#000000;}"
            ".playlist-workspace .pw-sidebar,.playlist-workspace .pw-content {background-color:#080808;border-color:#333333;}"
            ".playlist-workspace .pw-content {padding:4px;margin:0;}"
            ".playlist-workspace .pw-playlists button:checked {background-color:#252525;color:#ededed;border-color:#777777;}"
            ".playlist-workspace .pw-toolbar,.playlist-workspace .pw-bottom {padding:2px;margin:0;background-color:#080808;}"
            ".playlist-workspace .pw-source-strip {background-color:#111111;border-color:#333333;padding:6px;}"
            ".playlist-workspace .pw-source-strip label {color:#b5b5b5;}"
            ".playlist-workspace #pw-management,.playlist-workspace .pw-source,.playlist-workspace .pw-status {background-color:#080808;color:#b5b5b5;border-color:#333333;}"
            ".playlist-workspace #pw-management label,.playlist-workspace .pw-source label,.playlist-workspace .pw-status label {color:#b5b5b5;}"
            ".playlist-workspace .form-label,.playlist-workspace .task-help {color:#909090;}"
            "#pw-builder-table,#pw-source-table {background-color:#080808;color:#b5b5b5;}"
            "#pw-builder-table:selected,#pw-source-table:selected {background-color:#252525;color:#ededed;}"
            "#pw-builder-table header button,#pw-source-table header button {background-image:none;background-color:#202020;color:#b5b5b5;border-color:#333333;padding:4px;}"
            ".playlist-workspace .pw-marker-panel {padding:10px;background-color:#080808;border-color:#333333;}"
            ".playlist-workspace .pw-marker-panel label {color:#b5b5b5;}"
            ".playlist-workspace .pw-marker-panel entry {background-color:#070707;color:#c8c8c8;border-color:#444444;padding:4px;}"
            ".playlist-workspace .pw-marker-panel entry:focus {border-color:#36caff;}"
            ".playlist-workspace #pw-menu,.pw-menu-popup {background-color:#080808;color:#c8c8c8;border-color:#333333;}"
            ".playlist-workspace #pw-menu menuitem,.pw-menu-popup menuitem {color:#c8c8c8;padding:8px 18px;}"
            ".playlist-workspace #pw-menu menuitem:hover,.pw-menu-popup menuitem:hover {background-color:#252525;color:#ededed;}"
            ".radios.pw-dialog {background-color:#080808;color:#b5b5b5;border-color:#333333;}", -1, NULL);
        /* Chrome-like playlist tabs: rounded tops on a strip; the active tab
         * joins the panel below it. */
        GtkCssProvider *tabs = gtk_css_provider_new();
        gtk_css_provider_load_from_data(tabs,
            ".playlist-workspace .pw-target-tabs {background-color:#050505;border-bottom:1px solid #3a3a3a;padding:6px 6px 0 6px;}"
            ".playlist-workspace .pw-playlists button.pw-tab {border-radius:9px 9px 0 0;border:1px solid #262626;border-bottom-width:0;"
            "background-color:#121212;color:#9a9a9a;padding:6px 16px;margin:0 1px -1px 0;min-width:110px;min-height:24px;}"
            ".playlist-workspace .pw-playlists button.pw-tab:hover {background-color:#1d1d1d;color:#d8d8d8;}"
            ".playlist-workspace .pw-playlists button.pw-tab:checked {background-color:#202020;color:#f2f2f2;border-color:#4a4a4a;"
            "box-shadow:inset 0 2px 0 #36caff;}"
            ".playlist-workspace #pw-new-playlist {border-radius:50%;min-width:26px;min-height:26px;padding:0;margin:0 4px 4px 4px;}", -1, NULL);
        gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(page), GTK_STYLE_PROVIDER(tabs), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 3);
        g_object_unref(tabs);
        gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(page), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
        g_object_unref(css); g_once_init_leave(&initialized, 1);
    }
}

static gboolean playlist_append_entries(PlaylistView *view, JsonArray *entries)
{
    PlaylistDraft *draft = playlist_current(view);
    guint count = json_array_get_length(entries);
    if (!draft) { playlist_notice(view, "Önce alttaki + ile bir playlist oluşturun."); return FALSE; }
    if (!count || count > 500 || draft->tracks->len + count > 500) {
        playlist_notice(view, "Playlist en fazla 500 parça içerebilir."); return FALSE;
    }
    for (guint i = 0; i < count; ++i) {
        JsonNode *node = json_array_get_element(entries, i);
        if (!JSON_NODE_HOLDS_OBJECT(node) || !*radios_json_string(json_node_get_object(node), "title", "")) {
            playlist_notice(view, "Yalnız şarkı kayıtları playliste eklenebilir."); return FALSE;
        }
    }
    if (view->ctx->data) {
        if (view->busy || view->loading || !radios_data_connected(view->ctx)) {
            playlist_notice(view, "Playlist kaydı hazır değil; seçili parçalarınız korunuyor."); return FALSE;
        }
        JsonNode *body = playlist_body(view, draft->id, draft->revision);
        json_object_set_array_member(json_node_get_object(body), "entries", json_array_ref(entries));
        gboolean queued = playlist_write(view, view->widget, "entries-append", body);
        json_node_free(body); return queued;
    }
    JsonObject *copy = json_object_new(); json_object_set_string_member(copy, "name", "");
    json_object_set_array_member(copy, "tracks", json_array_ref(entries));
    PlaylistDraft *incoming = playlist_from_json(copy); json_object_unref(copy);
    while (incoming->tracks->len)
        g_ptr_array_add(draft->tracks, g_ptr_array_steal_index(incoming->tracks, 0));
    draft_free(incoming); playlist_refresh(view);
    gchar *notice = g_strdup_printf("%u parça playliste eklendi.", count); playlist_notice(view, notice); g_free(notice);
    return TRUE;
}

static void playlist_source_add(GtkButton *button, gpointer data)
{
    (void)button; PlaylistView *view = data;
    JsonArray *entries = radios_playlist_browser_selected(view->browser);
    playlist_append_entries(view, entries); json_array_unref(entries);
}

static void playlist_drop_entries(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
    GtkSelectionData *selection, guint info, guint time, gpointer data)
{
    (void)x; (void)y; (void)info; PlaylistView *view = data;
    /* The payload is owned by this handler; the filtered target model has no
     * GtkTreeDragDest row implementation for GTK's default handler. */
    g_signal_stop_emission_by_name(widget, "drag-data-received");
    gint length = gtk_selection_data_get_length(selection);
    gboolean accepted = FALSE;
    if (length > 0 && length <= 2 * 1024 * 1024) {
        JsonParser *parser = json_parser_new(); GError *error = NULL;
        if (json_parser_load_from_data(parser, (const gchar *)gtk_selection_data_get_data(selection), length, &error)) {
            JsonNode *root = json_parser_get_root(parser);
            if (root && JSON_NODE_HOLDS_ARRAY(root)) accepted = playlist_append_entries(view, json_node_get_array(root));
            else playlist_notice(view, "Geçerli şarkı seçimini sürükleyin.");
        } else playlist_notice(view, "Sürüklenen şarkı seçimi okunamadı.");
        g_clear_error(&error); g_object_unref(parser);
    }
    if (context) gtk_drag_finish(context, accepted, FALSE, time);
}

static gboolean playlist_alt_selection(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget; (void)data;
    if (event->button == 1 && (event->state & GDK_MOD1_MASK))
        event->state = (event->state & ~GDK_MOD1_MASK) | GDK_CONTROL_MASK;
    return FALSE;
}

static void
playlist_refresh(PlaylistView *view)
{
    PlaylistDraft *draft = playlist_current(view);
    PlaylistTrack *selected = playlist_selected(view);
    guint total = 0;
    gchar *text;
    view->updating = TRUE;
    if (view->target_selector) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(view->target_selector));
        for (guint i = 0; i < view->drafts->len; ++i) {
            PlaylistDraft *item = g_ptr_array_index(view->drafts, i);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(view->target_selector), item->name);
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(view->target_selector), view->current);
    }
    /* Other native pages may offer these user-created GUI drafts as choices.
     * Names are an owned snapshot and never imply database identities. */
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GHashTable *identities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (guint i = 0; i < view->drafts->len; ++i)
    {
        PlaylistDraft *item = g_ptr_array_index(view->drafts, i);
        g_ptr_array_add(names, g_strdup(item->name));
        if (item->id) g_hash_table_insert(identities, g_strdup(item->id), g_strdup(item->name));
    }
    g_object_set_data_full(G_OBJECT(view->widget), "radios-playlist-drafts", names, (GDestroyNotify)g_ptr_array_unref);
    g_object_set_data_full(G_OBJECT(view->widget), "radios-playlists", identities, (GDestroyNotify)g_hash_table_unref);
    gtk_widget_set_sensitive(view->lists, !view->busy);
    GList *children = gtk_container_get_children(GTK_CONTAINER(view->lists));
    for (GList *item = children; item != NULL; item = item->next)
        gtk_widget_destroy(GTK_WIDGET(item->data));
    g_list_free(children);
    for (guint i = 0; i < view->drafts->len; ++i) {
        PlaylistDraft *candidate = g_ptr_array_index(view->drafts, i); total += candidate->tracks->len;
        GtkWidget *button = gtk_toggle_button_new_with_label(candidate->name);
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "pw-tab");
        GtkWidget *caption = gtk_bin_get_child(GTK_BIN(button));
        if (GTK_IS_LABEL(caption)) { gtk_label_set_ellipsize(GTK_LABEL(caption), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(caption), 22); }
        gchar *tip = g_strdup_printf("%s · %u parça", playlist_purpose_name(candidate->purpose), candidate->tracks->len);
        gtk_widget_set_tooltip_text(button, tip); g_free(tip);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), (gint)i == view->current);
        g_object_set_data(G_OBJECT(button), "pw-draft-index", GINT_TO_POINTER((gint)i + 1));
        g_signal_connect(button, "clicked", G_CALLBACK(playlist_choose), view);
        gtk_box_pack_start(GTK_BOX(view->lists), button, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(view->lists);
    /* Summary as in playlist-workspace: bold counts, no state explanation. */
    text = g_strdup_printf("<b>%u</b> playlist", view->drafts->len);
    gtk_label_set_markup(GTK_LABEL(view->list_count), text); g_free(text);
    text = g_strdup_printf("<b>%u</b> parça kaydı", total);
    gtk_label_set_markup(GTK_LABEL(view->track_count), text); g_free(text);
    gtk_label_set_text(GTK_LABEL(view->heading), draft != NULL ? draft->name : "Playlist seçilmedi");
    gtk_label_set_text(GTK_LABEL(view->details), draft != NULL ? playlist_purpose_name(draft->purpose) : "");
    gtk_list_store_clear(view->store);
    if (draft != NULL) {
        for (guint i = 0; i < draft->tracks->len; ++i) {
            PlaylistTrack *track = g_ptr_array_index(draft->tracks, i);
            GtkTreeIter iter;
            gchar *title = g_strdup_printf("%s\n%s", track->title, track->artist);
            gchar *bpm = track->bpm > 0 ? g_strdup_printf("%.2f", track->bpm) : g_strdup("");
            gtk_list_store_append(view->store, &iter);
            gtk_list_store_set(view->store, &iter, 0, FALSE, 1, (gint)i + 1,
                2, title, 3, track->album, 4, track->genre, 5, bpm,
                6, track->duration, 7, track, 8, track->artist, -1);
            g_free(title); g_free(bpm);
        }
    }
    playlist_update_filter(view);
    for (guint i = 0; i < G_N_ELEMENTS(view->buttons); ++i)
        gtk_widget_set_sensitive(view->buttons[i], draft != NULL && (!view->ctx->data || (!view->busy && !view->loading && radios_data_connected(view->ctx))));
    gtk_widget_set_sensitive(view->buttons[PW_NEW], !view->ctx->data || radios_data_connected(view->ctx));
    for (guint i = PW_ADD_ALL; i <= PW_M3U; ++i)
        gtk_widget_set_sensitive(view->buttons[i], draft != NULL && draft->tracks->len > 0);
    if (view->play) gtk_widget_set_sensitive(view->play, draft && draft->tracks->len > 0 && !view->busy && !view->loading && radios_data_connected(view->ctx));
    view->updating = FALSE;
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(view->filter), &iter);
    while (valid) {
        PlaylistTrack *track = NULL;
        gtk_tree_model_get(GTK_TREE_MODEL(view->filter), &iter, 7, &track, -1);
        if (track == selected || (view->select_uuid && g_strcmp0(track->id, view->select_uuid) == 0)) {
            GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view->table));
            gtk_tree_selection_select_iter(selection, &iter);
            break;
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(view->filter), &iter);
    }
    playlist_selection_changed(gtk_tree_view_get_selection(GTK_TREE_VIEW(view->table)), view);
    g_clear_pointer(&view->select_uuid, g_free);
}

typedef struct {
    PlaylistView *view;
    PlaylistDraft *draft;
    PlaylistTrack *track;
    GtkWidget *fields[9], *status;
    gchar *playlist_id, *entry_id;
    gint64 playlist_revision, entry_revision;
    gchar *media_id, *version_id, *media_file_id, *media_sha256;
    gint64 duration_ms;
} TrackEditor;

static void track_editor_free(gpointer data)
{
    TrackEditor *editor = data; g_free(editor->playlist_id); g_free(editor->entry_id);
    g_free(editor->media_id); g_free(editor->version_id); g_free(editor->media_file_id); g_free(editor->media_sha256);
    g_free(editor);
}

static void track_media_picked(RadioSViewContext *ctx, GtkWidget *dialog, JsonObject *detail, gpointer data)
{
    (void)data; TrackEditor *editor = g_object_get_data(G_OBJECT(dialog), "pw-track-editor");
    JsonObject *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version");
    if (!media || !version || !g_str_equal(radios_json_string(media, "station_id", ""), radios_data_station_id(ctx)) ||
        !json_object_get_boolean_member(version, "verified") || !json_object_get_boolean_member(version, "object_available")) {
        gtk_label_set_text(GTK_LABEL(editor->status), "Erişilebilir ve doğrulanmış bir kütüphane medyası seçin."); return;
    }
    gchar **fields[] = {&editor->media_id, &editor->version_id, &editor->media_file_id, &editor->media_sha256};
    const gchar *values[] = {radios_json_string(media, "_id", ""), radios_json_string(version, "_id", ""),
        radios_json_string(version, "media_file_id", ""), radios_json_string(version, "sha256", "")};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) { g_free(*fields[i]); *fields[i] = g_strdup(values[i]); }
    editor->duration_ms = radios_json_int(version, "duration_ms", 0);
    const gchar *keys[] = {"title", "artist", "album", "genre"}; const guint indices[] = {0,1,2,5};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) gtk_entry_set_text(GTK_ENTRY(editor->fields[indices[i]]), radios_json_string(media, keys[i], ""));
    gchar *duration = g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, editor->duration_ms / 60000, (editor->duration_ms / 1000) % 60);
    gtk_entry_set_text(GTK_ENTRY(editor->fields[3]), duration); g_free(duration);
    gtk_widget_set_sensitive(editor->fields[3], FALSE); gtk_widget_set_sensitive(editor->fields[7], FALSE);
    gtk_label_set_text(GTK_LABEL(editor->status), "Doğrulanmış medya seçildi. Kaydet ile playlist kaydına bağlayın.");
}
static void track_media_open(GtkButton *button, gpointer data)
{
    (void)button; GtkWidget *dialog = data; TrackEditor *editor = g_object_get_data(G_OBJECT(dialog), "pw-track-editor");
    radios_media_library_pick(editor->view->ctx, dialog, track_media_picked, NULL);
}

static void
track_editor_response(GtkDialog *dialog, gint response, gpointer data)
{
    TrackEditor *editor = data;
    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }
    gchar *title = g_strdup(gtk_entry_get_text(GTK_ENTRY(editor->fields[0])));
    g_strstrip(title);
    const gchar *duration = gtk_entry_get_text(GTK_ENTRY(editor->fields[3]));
    if (*title == '\0' || (*duration != '\0' &&
        !g_regex_match_simple("^[0-9]{1,4}:[0-5][0-9]$", duration, 0, 0))) {
        gtk_label_set_text(GTK_LABEL(editor->status), *title == '\0' ?
            "Parça adı gerekli." : "Süreyi dk:sn biçiminde girin; saniye 00–59 olmalı.");
        gtk_widget_grab_focus(editor->fields[*title == '\0' ? 0 : 3]);
        g_free(title);
        return;
    }
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(editor->fields[8]));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *comment = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    if (g_utf8_strlen(comment, -1) > 2000) {
        gtk_label_set_text(GTK_LABEL(editor->status), "Yorum en fazla 2000 karakter olabilir.");
        g_free(title); g_free(comment);
        return;
    }
    if (editor->view->ctx->data) {
        JsonNode *body = playlist_body(editor->view, editor->playlist_id, editor->playlist_revision);
        JsonObject *object = json_node_get_object(body), *metadata = json_object_new();
        json_object_set_string_member(metadata, "title", title);
        const gchar *keys[] = {"artist", "album", "genre", "key", "location"};
        const guint field_indices[] = {1, 2, 5, 6, 7};
        for (guint i = 0; i < G_N_ELEMENTS(keys); ++i)
            json_object_set_string_member(metadata, keys[i], gtk_entry_get_text(GTK_ENTRY(editor->fields[field_indices[i]])));
        json_object_set_string_member(metadata, "comment", comment);
        json_object_set_double_member(metadata, "bpm", gtk_spin_button_get_value(GTK_SPIN_BUTTON(editor->fields[4])));
        guint minutes = 0, seconds = 0;
        if (editor->media_id) {
            json_object_set_int_member(metadata, "duration_ms", editor->duration_ms);
            json_object_set_string_member(metadata, "media_id", editor->media_id);
            json_object_set_string_member(metadata, "version_id", editor->version_id);
            json_object_set_string_member(metadata, "media_file_id", editor->media_file_id);
            json_object_set_string_member(metadata, "media_sha256", editor->media_sha256);
        } else if (*duration != '\0' && sscanf(duration, "%u:%u", &minutes, &seconds) == 2)
            json_object_set_int_member(metadata, "duration_ms", ((gint64)minutes * 60 + seconds) * 1000);
        else json_object_set_null_member(metadata, "duration_ms");
        json_object_set_object_member(object, "metadata", metadata);
        if (editor->entry_id) { json_object_set_string_member(object, "entry_id", editor->entry_id); json_object_set_int_member(object, "expected_entry_revision", editor->entry_revision); }
        playlist_write(editor->view, GTK_WIDGET(dialog), "entry-save", body);
        json_node_free(body); g_free(title); g_free(comment); return;
    }
    PlaylistTrack *track = editor->track != NULL ? editor->track : g_new0(PlaylistTrack, 1);
    g_free(track->title); track->title = title;
    gchar **destinations[] = { &track->artist, &track->album, &track->duration,
                              &track->genre, &track->key, &track->location };
    const guint indices[] = { 1, 2, 3, 5, 6, 7 };
    for (guint i = 0; i < G_N_ELEMENTS(indices); ++i) {
        g_free(*destinations[i]);
        *destinations[i] = g_strdup(gtk_entry_get_text(GTK_ENTRY(editor->fields[indices[i]])));
    }
    track->bpm = gtk_spin_button_get_value(GTK_SPIN_BUTTON(editor->fields[4]));
    guint minutes = 0, seconds = 0;
    track->duration_ms = sscanf(track->duration, "%u:%u", &minutes, &seconds) == 2 ? ((gint64)minutes * 60 + seconds) * 1000 : 0;
    g_free(track->comment); track->comment = comment;
    if (editor->track == NULL)
        g_ptr_array_add(editor->draft->tracks, track);
    playlist_refresh(editor->view);
    playlist_notice(editor->view, "Parça oturum taslağında güncellendi. Veritabanına kaydedilmedi; ses dosyası kopyalanmadı.");
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
playlist_track_editor(PlaylistView *view, gboolean is_new)
{
    PlaylistDraft *draft = playlist_current(view);
    PlaylistTrack *track = is_new ? NULL : playlist_selected(view);
    if (draft == NULL || (!is_new && track == NULL))
        return;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(is_new ? "Parça ekle" : "Parçayı düzenle",
        view->ctx->parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(dialog, "pw-track-dialog");
    g_signal_connect_object(view->widget, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "pw-dialog");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 560, 650);
    TrackEditor *editor = g_new0(TrackEditor, 1);
    editor->view = view; editor->draft = draft; editor->track = track;
    editor->playlist_id = g_strdup(draft->id); editor->playlist_revision = draft->revision;
    editor->entry_id = track ? g_strdup(track->id) : NULL; editor->entry_revision = track ? track->revision : 0;
    if (track) {
        editor->media_id = g_strdup(track->media_id); editor->version_id = g_strdup(track->version_id);
        editor->media_file_id = g_strdup(track->media_file_id); editor->media_sha256 = g_strdup(track->media_sha256); editor->duration_ms = track->duration_ms;
    }
    g_object_set_data_full(G_OBJECT(dialog), "pw-track-editor", editor, track_editor_free);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 22);
    gtk_container_add(GTK_CONTAINER(scroll), grid);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll, TRUE, TRUE, 0);
    const gchar *labels[] = { "Parça adı", "Sanatçı", "Albüm", "Süre (dk:sn)", "BPM", "Tür", "Ton", "Dosya yolu veya URI", "Yorum" };
    const gchar *ids[] = { "title", "artist", "album", "duration", "bpm", "genre", "key", "location", "comment" };
    const gchar *values[] = { track != NULL ? track->title : "", track != NULL ? track->artist : "",
        track != NULL ? track->album : "", track != NULL ? track->duration : "", "",
        track != NULL ? track->genre : "", track != NULL ? track->key : "",
        track != NULL ? track->location : "", track != NULL ? track->comment : "" };
    const gint positions[][3] = { {0,0,2}, {0,1,1}, {1,1,1}, {0,2,1}, {1,2,1},
                                 {0,3,1}, {1,3,1}, {0,4,2}, {0,5,2} };
    for (guint i = 0; i < G_N_ELEMENTS(labels); ++i) {
        GtkWidget *field = radios_ui_box(GTK_ORIENTATION_VERTICAL, 7, NULL);
        GtkWidget *input;
        if (i == 4) {
            input = gtk_spin_button_new_with_range(0, 1000, 0.01);
            gtk_spin_button_set_digits(GTK_SPIN_BUTTON(input), 2);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(input), track != NULL ? track->bpm : 0);
        } else if (i == 8) {
            input = gtk_text_view_new();
            gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(input), GTK_WRAP_WORD_CHAR);
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(input)), values[i], -1);
            gtk_widget_set_size_request(input, -1, 80);
        } else {
            input = radios_ui_entry(i == 3 ? "03:30" : (i == 7 ? "/Music/parca.mp3 veya URI" : NULL));
            gtk_entry_set_text(GTK_ENTRY(input), values[i]);
            gtk_entry_set_max_length(GTK_ENTRY(input), i == 7 ? 4096 : (i == 0 ? 160 : 120));
        }
        gchar *id = g_strdup_printf("pw-edit-%s", ids[i]);
        gtk_widget_set_name(input, id); g_free(id);
        editor->fields[i] = input;
        gtk_widget_set_hexpand(input, TRUE);
        gtk_box_pack_start(GTK_BOX(field), radios_ui_label(labels[i], "form-label"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(field), input, FALSE, FALSE, 0);
        gtk_grid_attach(GTK_GRID(grid), field, positions[i][0], positions[i][1], positions[i][2], 1);
    }
    editor->status = radios_ui_label("", "pw-status");
    g_object_set_data(G_OBJECT(dialog), "pw-save-status", editor->status);
    if (view->ctx->data) {
        GtkWidget *pick = radios_ui_button("Kütüphaneden seç");
        gtk_widget_set_name(pick, "pw-track-media");
        g_signal_connect_object(pick, "clicked", G_CALLBACK(track_media_open), dialog, 0);
        gtk_grid_attach(GTK_GRID(grid), pick, 0, 6, 2, 1);
        if (editor->media_id) { gtk_widget_set_sensitive(editor->fields[3], FALSE); gtk_widget_set_sensitive(editor->fields[7], FALSE); }
    }
    gtk_grid_attach(GTK_GRID(grid), editor->status, 0, 7, 2, 1);
    g_signal_connect(dialog, "response", G_CALLBACK(track_editor_response), editor);
    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(editor->fields[0]);
}

static void
playlist_name_response(GtkDialog *dialog, gint response, gpointer data)
{
    PlaylistView *view = data;
    GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "pw-name-entry");
    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *name = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
        g_strstrip(name);
        if (*name == '\0') {
            gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(dialog), "pw-name-status")), "Playlist adı gerekli.");
            g_free(name); gtk_widget_grab_focus(entry); return;
        }
        PlaylistDraft *draft = playlist_current(view);
        if (view->ctx->data) {
            const gchar *id = g_object_get_data(G_OBJECT(dialog), "pw-playlist-id");
            gint64 *revision = g_object_get_data(G_OBJECT(dialog), "pw-playlist-revision");
            JsonNode *body = playlist_body(view, id, revision ? *revision : 0);
            json_object_set_string_member(json_node_get_object(body), "name", name);
            playlist_write(view, GTK_WIDGET(dialog), "rename", body); json_node_free(body); g_free(name); return;
        }
        if (draft != NULL) { g_free(draft->name); draft->name = name; }
        else g_free(name);
        playlist_refresh(view);
        playlist_notice(view, "Playlist adı oturum taslağında değiştirildi. Veritabanına kaydedilmedi.");
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
playlist_delete_response(GtkDialog *dialog, gint response, gpointer data)
{
    PlaylistView *view = data;
    if (response == GTK_RESPONSE_ACCEPT && view->ctx->data) {
        const gchar *id = g_object_get_data(G_OBJECT(dialog), "pw-playlist-id");
        gint64 *revision = g_object_get_data(G_OBJECT(dialog), "pw-playlist-revision");
        JsonNode *body = playlist_body(view, id, revision ? *revision : 0);
        playlist_write(view, GTK_WIDGET(dialog), "delete", body); json_node_free(body); return;
    }
    if (response == GTK_RESPONSE_ACCEPT && playlist_current(view) != NULL) {
        view->updating = TRUE;
        gtk_list_store_clear(view->store);
        g_ptr_array_remove_index(view->drafts, (guint)view->current);
        view->current = view->drafts->len > 0 ? 0 : -1;
        playlist_refresh(view);
        playlist_notice(view, "Playlist oturum taslağı kaldırıldı. Veritabanı ve ses dosyaları değiştirilmedi.");
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
playlist_csv_cell(GString *output, const gchar *value)
{
    g_string_append_c(output, '"');
    if (value != NULL && strchr("=+-@\t\r", *value) != NULL && *value != '\0')
        g_string_append_c(output, '\'');
    for (const gchar *cursor = value != NULL ? value : ""; *cursor != '\0'; ++cursor) {
        if (*cursor == '"') g_string_append_c(output, '"');
        g_string_append_c(output, *cursor);
    }
    g_string_append_c(output, '"');
}

static void
playlist_export_response(GtkDialog *dialog, gint response, gpointer data)
{
    PlaylistView *view = data;
    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        const gchar *contents = g_object_get_data(G_OBJECT(dialog), "pw-export-data");
        GError *error = NULL;
        if (filename != NULL && g_file_set_contents(filename, contents, -1, &error))
            playlist_notice(view, "Playlist oturum taslağı dışa aktarıldı. Veritabanı ve yayın motoru değiştirilmedi.");
        else playlist_notice(view, error != NULL ? error->message : "Dosya adı seçilmedi.");
        g_clear_error(&error); g_free(filename);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
playlist_export(PlaylistView *view, gboolean csv)
{
    PlaylistDraft *draft = playlist_current(view);
    if (draft == NULL || draft->tracks->len == 0) return;
    GString *output = g_string_new(csv ? "\357\273\277Sıra,Parça,Sanatçı,Albüm,Süre,BPM,Tür,Ton,Dosya,Yorum\r\n" : "#EXTM3U\n");
    guint included = 0;
    for (guint i = 0; i < draft->tracks->len; ++i) {
        PlaylistTrack *track = g_ptr_array_index(draft->tracks, i);
        if (csv) {
            gchar *order = g_strdup_printf("%u", i + 1);
            gchar *bpm = g_strdup_printf("%.2f", track->bpm);
            const gchar *cells[] = { order, track->title, track->artist, track->album,
                track->duration, bpm, track->genre, track->key, track->location, track->comment };
            for (guint j = 0; j < G_N_ELEMENTS(cells); ++j) {
                if (j > 0) g_string_append_c(output, ',');
                playlist_csv_cell(output, cells[j]);
            }
            g_string_append(output, "\r\n"); g_free(order); g_free(bpm);
        } else {
            if (*track->location == '\0' || strpbrk(track->location, "\r\n") != NULL) continue;
            gchar **duration = g_strsplit(track->duration, ":", 2);
            gint seconds = duration[0] != NULL && duration[1] != NULL ?
                (gint)g_ascii_strtoll(duration[0], NULL, 10) * 60 + (gint)g_ascii_strtoll(duration[1], NULL, 10) : -1;
            g_strfreev(duration);
            gchar *name = g_strdup_printf("%s%s%s", track->artist, *track->artist != '\0' ? " - " : "", track->title);
            for (gchar *cursor = name; *cursor != '\0'; ++cursor)
                if (*cursor == '\n' || *cursor == '\r') *cursor = ' ';
            g_string_append_printf(output, "#EXTINF:%d,%s\n%s\n", seconds, name, track->location);
            g_free(name);
        }
        included++;
    }
    if (included == 0) { g_string_free(output, TRUE); playlist_notice(view, "M3U8 için dosya konumu olan bir parça taslağı gerekli."); return; }
    GtkWidget *dialog = gtk_file_chooser_dialog_new(csv ? "Playlist taslağını CSV olarak dışa aktar" : "Playlist taslağını M3U8 olarak dışa aktar",
        view->ctx->parent, GTK_FILE_CHOOSER_ACTION_SAVE, "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_widget_set_name(dialog, "pw-export-dialog");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    gchar *name = g_strdup_printf("%s.%s", draft->name, csv ? "csv" : "m3u8");
    for (gchar *cursor = name; *cursor != '\0'; ++cursor)
        if (strchr("<>:\"/\\|?*", *cursor) != NULL || (guchar)*cursor < 32) *cursor = '_';
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), name); g_free(name);
    g_object_set_data_full(G_OBJECT(dialog), "pw-export-data", g_string_free(output, FALSE), g_free);
    g_signal_connect(dialog, "response", G_CALLBACK(playlist_export_response), view);
    gtk_widget_show(dialog);
}

static void
playlist_action_clicked(GtkButton *button, gpointer data)
{
    PlaylistView *view = data;
    gint action = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "pw-action")) - 1;
    if (action == PW_NEW) { radios_media_import_open(view->ctx); return; }
    PlaylistDraft *draft = playlist_current(view);
    PlaylistTrack *track = playlist_selected(view);
    if (draft == NULL)
        return;
    if (view->ctx->data && (view->busy || view->loading)) { playlist_notice(view, "Önceki veri işlemi tamamlanıyor; girdiler korunuyor."); return; }
    if (action == PW_EDIT) {
        playlist_track_editor(view, FALSE);
    } else if (action == PW_RENAME || action == PW_DELETE) {
        GtkWidget *dialog = gtk_dialog_new_with_buttons(action == PW_RENAME ? "Playlist adını değiştir" : "Playlisti sil",
            view->ctx->parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            "Vazgeç", GTK_RESPONSE_CANCEL, action == PW_RENAME ? "Kaydet" : "Playlisti sil", GTK_RESPONSE_ACCEPT, NULL);
        gtk_widget_set_name(dialog, action == PW_RENAME ? "pw-name-dialog" : "pw-delete-dialog");
        gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
        gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "pw-dialog");
        GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        g_object_set_data_full(G_OBJECT(dialog), "pw-playlist-id", g_strdup(draft->id), g_free);
        gint64 *revision = g_new(gint64, 1); *revision = draft->revision;
        g_object_set_data_full(G_OBJECT(dialog), "pw-playlist-revision", revision, g_free);
        gtk_container_set_border_width(GTK_CONTAINER(body), 22);
        gtk_box_set_spacing(GTK_BOX(body), 12);
        if (action == PW_RENAME) {
            GtkWidget *entry = radios_ui_entry(NULL);
            gtk_widget_set_name(entry, "pw-edit-name");
            gtk_entry_set_max_length(GTK_ENTRY(entry), 120);
            gtk_entry_set_text(GTK_ENTRY(entry), draft->name);
            gtk_box_pack_start(GTK_BOX(body), radios_ui_label("Playlist adı", NULL), FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(body), entry, FALSE, FALSE, 0);
            GtkWidget *status = radios_ui_label("", "pw-status");
            gtk_box_pack_start(GTK_BOX(body), status, FALSE, FALSE, 0);
            g_object_set_data(G_OBJECT(dialog), "pw-name-entry", entry);
            g_object_set_data(G_OBJECT(dialog), "pw-name-status", status);
            g_object_set_data(G_OBJECT(dialog), "pw-save-status", status);
            g_signal_connect(dialog, "response", G_CALLBACK(playlist_name_response), view);
        } else {
            gchar *text = g_strdup_printf("%s (%u parça taslağı) %s kaldırılacak.", draft->name, draft->tracks->len, draft->id ? "aktif arşivden" : "oturumdan");
            gtk_box_pack_start(GTK_BOX(body), radios_ui_label(text, NULL), FALSE, FALSE, 0); g_free(text);
            GtkWidget *status = radios_ui_label("", "pw-status");
            gtk_box_pack_start(GTK_BOX(body), status, FALSE, FALSE, 0);
            g_object_set_data(G_OBJECT(dialog), "pw-save-status", status);
            g_signal_connect(dialog, "response", G_CALLBACK(playlist_delete_response), view);
        }
        gtk_widget_show_all(dialog);
    } else if (action == PW_REMOVE) {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view->table));
        GList *paths = gtk_tree_selection_get_selected_rows(selection, NULL);
        GPtrArray *removed = g_ptr_array_new();
        for (GList *item = paths; item != NULL; item = item->next) {
            GtkTreeIter iter; PlaylistTrack *item_track = NULL;
            if (gtk_tree_model_get_iter(GTK_TREE_MODEL(view->filter), &iter, item->data)) {
                gtk_tree_model_get(GTK_TREE_MODEL(view->filter), &iter, 7, &item_track, -1);
                g_ptr_array_add(removed, item_track);
            }
        }
        g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
        if (view->ctx->data) {
            JsonNode *body = playlist_body(view, draft->id, draft->revision);
            JsonArray *entries = json_array_new();
            for (guint i = 0; i < removed->len; ++i) {
                PlaylistTrack *row = g_ptr_array_index(removed, i); JsonObject *entry = json_object_new();
                json_object_set_string_member(entry, "entry_id", row->id);
                json_object_set_int_member(entry, "expected_revision", row->revision);
                json_array_add_object_element(entries, entry);
            }
            json_object_set_array_member(json_node_get_object(body), "entries", entries);
            playlist_write(view, view->widget, "entry-remove", body); json_node_free(body); g_ptr_array_unref(removed); return;
        }
        view->updating = TRUE; gtk_list_store_clear(view->store);
        for (guint i = 0; i < removed->len; ++i)
            g_ptr_array_remove(draft->tracks, g_ptr_array_index(removed, i));
        g_ptr_array_unref(removed);
        playlist_refresh(view);
        playlist_notice(view, "Seçilen parça taslakları kaldırıldı. Ses dosyaları değiştirilmedi.");
    } else if (action == PW_UP || action == PW_DOWN) {
        for (guint i = 0; i < draft->tracks->len; ++i) {
            gint next = (gint)i + (action == PW_UP ? -1 : 1);
            if (g_ptr_array_index(draft->tracks, i) == track && next >= 0 && next < (gint)draft->tracks->len) {
                if (view->ctx->data) {
                    JsonNode *body = playlist_body(view, draft->id, draft->revision); JsonArray *entries = json_array_new();
                    for (guint j = 0; j < draft->tracks->len; ++j) {
                        guint source = j == i ? (guint)next : j == (guint)next ? i : j;
                        json_array_add_string_element(entries, ((PlaylistTrack *)g_ptr_array_index(draft->tracks, source))->id);
                    }
                    json_object_set_array_member(json_node_get_object(body), "entries", entries);
                    playlist_write(view, view->widget, "reorder", body); json_node_free(body); return;
                }
                gpointer temporary = g_ptr_array_index(draft->tracks, (guint)next);
                g_ptr_array_index(draft->tracks, (guint)next) = track;
                g_ptr_array_index(draft->tracks, i) = temporary;
                gtk_combo_box_set_active(GTK_COMBO_BOX(view->sort), 0);
                playlist_refresh(view);
                playlist_notice(view, "Playlist taslak sırası değiştirildi. Veritabanına kaydedilmedi.");
                break;
            }
        }
    } else if (action == PW_CSV || action == PW_M3U) {
        playlist_export(view, action == PW_CSV);
    } else if (action == PW_ADD_SELECTED || action == PW_ADD_ALL) {
        playlist_to_programme(view, action == PW_ADD_SELECTED);
    } else {
        playlist_notice(view, "Yayın sırasına eklemek için veritabanı ve yayın motoru bağlantısı gerekli. Taslak sıraya eklenmedi.");
    }
}

static void playlist_to_programme_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)data; PlaylistView *view = g_object_get_data(G_OBJECT(owner), "playlist-view"); view->busy = FALSE; playlist_refresh(view);
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { playlist_notice(view, error ? error->message : "Playlist medyası doğrulanamadı."); return; }
    JsonArray *tracks = json_object_get_array_member(json_node_get_object(result), "tracks"); GError *failure = NULL;
    if (!radios_broadcast_append_playlist(ctx, tracks, &failure)) playlist_notice(view, failure ? failure->message : "Program sırası güncellenemedi.");
    else playlist_notice(view, "Doğrulanmış playlist öğeleri program sırasına ekleniyor.");
    g_clear_error(&failure);
}
static void playlist_to_programme(PlaylistView *view, gboolean selected)
{
    PlaylistDraft *draft = playlist_current(view);
    if (!draft || view->busy || view->loading || !radios_data_connected(view->ctx)) { playlist_notice(view, "Veri bağlantısı hazır değil; mevcut seçiminiz korunuyor."); return; }
    JsonNode *body = radios_data_body(view->ctx); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "playlist"); json_object_set_string_member(object, "playlist_id", draft->id);
    if (selected) {
        JsonArray *ids = json_array_new(); GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view->table));
        GList *paths = gtk_tree_selection_get_selected_rows(selection, NULL);
        for (GList *item = paths; item; item = item->next) {
            GtkTreeIter iter; PlaylistTrack *track = NULL;
            if (gtk_tree_model_get_iter(GTK_TREE_MODEL(view->filter), &iter, item->data)) gtk_tree_model_get(GTK_TREE_MODEL(view->filter), &iter, 7, &track, -1);
            if (track && track->id) json_array_add_string_element(ids, track->id);
        }
        g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
        if (!json_array_get_length(ids)) { json_array_unref(ids); json_node_free(body); playlist_notice(view, "Önce playlistten öğe seçin."); return; }
        json_object_set_array_member(object, "entry_ids", ids);
    }
    view->busy = TRUE; playlist_refresh(view);
    if (!radios_data_request(view->ctx, view->widget, "POST", "/v1/workflows/desktop-playout/prepare", body, playlist_to_programme_done, NULL, NULL)) {
        view->busy = FALSE; playlist_refresh(view); playlist_notice(view, "Medya kontrolü başlatılamadı; seçiminiz korunuyor.");
    } else playlist_notice(view, "Playlist medya sürümleri doğrulanıyor…");
    json_node_free(body);
}
/* ▶ plays the playlist on a live player (its deck), not directly on MAIN. */
static void playlist_play(GtkButton *button, gpointer data)
{
    (void)button; PlaylistView *view = data; PlaylistDraft *draft = playlist_current(view); if (!draft) return;
    JsonArray *items = json_array_new();
    const gchar *keys[] = {"media_id", "version_id", "media_file_id", "media_sha256"};
    for (guint i = 0; i < draft->tracks->len; ++i) {
        PlaylistTrack *track = g_ptr_array_index(draft->tracks, i);
        const gchar *binding[] = {track->media_id, track->version_id, track->media_file_id, track->media_sha256};
        JsonObject *item = json_object_new();
        json_object_set_string_member(item, "title", track->title ? track->title : "");
        json_object_set_string_member(item, "artist", track->artist ? track->artist : "");
        json_object_set_string_member(item, "album", track->album ? track->album : "");
        json_object_set_int_member(item, "duration_ms", MAX(0, track->duration_ms));
        if (g_uuid_string_is_valid(draft->id)) json_object_set_string_member(item, "playlist_id", draft->id);
        if (g_uuid_string_is_valid(track->id)) json_object_set_string_member(item, "entry_id", track->id);
        if (track->media_id && *track->media_id)
            for (guint j = 0; j < G_N_ELEMENTS(keys); ++j) json_object_set_string_member(item, keys[j], binding[j] ? binding[j] : "");
        json_array_add_object_element(items, item);
    }
    GError *error = NULL; gchar *player = NULL;
    if (!radios_broadcast_play_on_player(view->ctx, items, draft->name, &player, &error))
        playlist_notice(view, error ? error->message : "Playlist oynatıcıya yüklenemedi.");
    else { gchar *message = g_strdup_printf("%s: %s çalıyor.", player, draft->name); playlist_notice(view, message); g_free(message); }
    g_free(player); g_clear_error(&error); json_array_unref(items);
}

static void
playlist_row_toggle(GtkCellRendererToggle *renderer, gchar *path_string, gpointer data)
{
    PlaylistView *view = data;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view->table));
    (void)renderer;
    if (gtk_tree_selection_path_is_selected(selection, path))
        gtk_tree_selection_unselect_path(selection, path);
    else
        gtk_tree_selection_select_path(selection, path);
    gtk_tree_path_free(path);
}

typedef struct {
    GWeakRef page;
    GtkWidget *dialog, *controls, *file, *status, *choices, *selected, *collection, *review;
    GtkWidget *library_key, *name_policy, *duplicates, *cues, *format, *folder, *sources, *inputs;
    GVolumeMonitor *volumes;
    guint collection_tracks;
    GCancellable *cancel;
    gchar *xml_base64, *preview_hash, *usb_root, *auto_library_key;
    JsonNode *retry_body;
    gboolean closed, busy, updating, choices_loaded;
} XmlImport;

typedef struct { GWeakRef page; gboolean applying; } XmlOperation;
typedef struct { gchar *path; gboolean device; } ImportSource;

static void xml_preview_request(XmlImport *import, gboolean discovery);

static gboolean import_device(XmlImport *import)
{ return g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(import->format)), "usb") == 0; }

static GtkWidget *
xml_page(XmlImport *import)
{
    GtkWidget *page = g_weak_ref_get(&import->page);
    if (page && g_object_get_data(G_OBJECT(page), "radios-data-destroyed")) g_clear_object(&page);
    return page;
}

static guint
xml_selected_count(XmlImport *import)
{
    guint selected = 0;
    GList *children = gtk_container_get_children(GTK_CONTAINER(import->choices));
    for (GList *item = children; item; item = item->next)
        if (GTK_IS_TOGGLE_BUTTON(item->data) && gtk_toggle_button_get_active(item->data)) selected++;
    g_list_free(children);
    return selected;
}

static void
xml_update_action(XmlImport *import)
{
    gboolean selected = !import->choices_loaded || xml_selected_count(import) > 0 ||
        (import->collection_tracks > 0 && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(import->collection)));
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(import->dialog), GTK_RESPONSE_ACCEPT);
    gtk_button_set_label(GTK_BUTTON(accept), import->retry_body ? "Aynı işlemi yeniden dene" :
        import->preview_hash ? "Seçilenleri içeri aktar" : "Önizlemeyi doğrula");
    gtk_widget_set_sensitive(accept, !import->busy && selected && import->xml_base64 != NULL);
    gtk_widget_set_sensitive(import->controls, !import->busy && !import->retry_body);
    gchar *text = g_strdup_printf("%u playlist seçili%s", xml_selected_count(import),
        import->retry_body ? " · önce bekleyen işlem doğrulanmalı" :
        import->preview_hash ? " · önizleme doğrulandı" : " · önizleme doğrulaması gerekli");
    gtk_label_set_text(GTK_LABEL(import->selected), text); g_free(text);
}

static void
xml_selection_changed(GtkWidget *widget, gpointer data)
{
    XmlImport *import = data; (void)widget;
    if (import->updating || import->busy || import->retry_body) return;
    g_clear_pointer(&import->preview_hash, g_free);
    xml_update_action(import);
}

static JsonNode *
xml_request_body(XmlImport *import, PlaylistView *view, gboolean discovery)
{
    JsonNode *body = radios_data_body(view->ctx); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, import_device(import) ? "pdb_zlib_base64" : "xml_base64", import->xml_base64);
    if (import_device(import)) json_object_set_string_member(object, "usb_root", import->usb_root);
    json_object_set_string_member(object, "library_key", gtk_entry_get_text(GTK_ENTRY(import->library_key)));
    json_object_set_string_member(object, "name_policy", gtk_combo_box_get_active(GTK_COMBO_BOX(import->name_policy)) == 0 ? "append" : "copy");
    json_object_set_string_member(object, "duplicate_policy", gtk_combo_box_get_active(GTK_COMBO_BOX(import->duplicates)) == 0 ? "skip" : "keep");
    const gchar *cue_policies[] = { "preserve_as_dj", "propose_radio_cues", "ignore" };
    gint cue = gtk_combo_box_get_active(GTK_COMBO_BOX(import->cues));
    json_object_set_string_member(object, "cue_policy", cue_policies[CLAMP(cue, 0, 2)]);
    json_object_set_boolean_member(object, "include_collection", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(import->collection)));
    if (!discovery) {
        JsonArray *paths = json_array_new();
        GList *children = gtk_container_get_children(GTK_CONTAINER(import->choices));
        for (GList *item = children; item; item = item->next) {
            if (!GTK_IS_TOGGLE_BUTTON(item->data) || !gtk_toggle_button_get_active(item->data)) continue;
            JsonNode *path = g_object_get_data(G_OBJECT(item->data), "pw-xml-path");
            if (path) json_array_add_element(paths, json_node_copy(path));
        }
        g_list_free(children); json_object_set_array_member(object, "selected_node_paths", paths);
    }
    return body;
}

static void
xml_operation_free(gpointer data)
{
    XmlOperation *operation = data;
    GtkWidget *page = g_weak_ref_get(&operation->page);
    if (operation->applying && page && !g_object_get_data(G_OBJECT(page), "radios-data-destroyed")) {
        PlaylistView *view = g_object_get_data(G_OBJECT(page), "playlist-view");
        view->busy = FALSE; playlist_refresh(view); playlist_data_refresh(view->ctx, page);
    }
    g_clear_object(&page); g_weak_ref_clear(&operation->page); g_free(operation);
}

static void
xml_preview_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result,
                 const GError *error, gpointer unused)
{
    XmlImport *import = g_object_get_data(G_OBJECT(owner), "pw-xml-import");
    (void)ctx; (void)unused;
    import->busy = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        gtk_label_set_text(GTK_LABEL(import->status), error ? error->message : "Tracklist önizlemesi alınamadı.");
        xml_update_action(import); return;
    }
    JsonObject *object = json_node_get_object(result);
    const gchar *hash = radios_json_string(object, "preview_hash", "");
    if (strlen(hash) != 64) {
        gtk_label_set_text(GTK_LABEL(import->status), "Veri servisi geçerli önizleme kimliği döndürmedi.");
        xml_update_action(import); return;
    }
    import->collection_tracks = (guint)radios_json_int(object, "collection_tracks", 0);
    import->updating = TRUE;
    if (!import->choices_loaded) {
        JsonArray *nodes = json_object_get_array_member(object, "playlist_nodes");
        for (guint i = 0; nodes && i < json_array_get_length(nodes); ++i) {
            JsonObject *node = json_array_get_object_element(nodes, i);
            gchar *text = g_strdup_printf("%s · %" G_GINT64_FORMAT " parça", radios_json_string(node, "display_path", radios_json_string(node, "name", "Adsız playlist")), radios_json_int(node, "tracks", 0));
            GtkWidget *check = gtk_check_button_new_with_label(text); g_free(text);
            JsonNode *path = json_object_get_member(node, "path");
            if (!path || !JSON_NODE_HOLDS_ARRAY(path)) { gtk_widget_destroy(check); continue; }
            g_object_set_data_full(G_OBJECT(check), "pw-xml-path", json_node_copy(path), (GDestroyNotify)json_node_free);
            gtk_style_context_add_class(gtk_widget_get_style_context(check), "pw-check");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), TRUE);
            g_signal_connect(check, "toggled", G_CALLBACK(xml_selection_changed), import);
            gtk_box_pack_start(GTK_BOX(import->choices), check, FALSE, FALSE, 0);
        }
        import->choices_loaded = TRUE;
        gtk_widget_show_all(import->choices);
        /* Discovery selected all nodes implicitly. Explicit choices must receive
         * their own CAS preview before the operator can apply the import. */
        g_clear_pointer(&import->preview_hash, g_free);
    } else {
        g_free(import->preview_hash); import->preview_hash = g_strdup(hash);
    }
    import->updating = FALSE;
    JsonArray *destinations = json_object_get_array_member(object, "destinations");
    gint64 added = 0, skipped = 0;
    for (guint i = 0; destinations && i < json_array_get_length(destinations); ++i) {
        JsonObject *destination = json_array_get_object_element(destinations, i);
        added += radios_json_int(destination, "added_count", 0); skipped += radios_json_int(destination, "skipped", 0);
    }
    gchar *text = g_strdup_printf("%u hedef liste · %" G_GINT64_FORMAT " yeni öğe · %" G_GINT64_FORMAT " tekrar atlandı · %" G_GINT64_FORMAT " cue · %" G_GINT64_FORMAT " aktarım uyarısı. Ses dosyaları doğrulanmadı.",
        destinations ? json_array_get_length(destinations) : 0, added, skipped,
        radios_json_int(object, "cue_marks", 0), radios_json_int(object, "projection_loss_count", 0) + radios_json_int(object, "domain_loss_count", 0));
    gtk_label_set_text(GTK_LABEL(import->status), text); g_free(text);
    gtk_widget_set_tooltip_text(import->status, radios_json_string(object, "source_sha256", ""));
    JsonObject *counts = json_object_get_object_member(object, "catalog_counts");
    GString *review = g_string_new(NULL);
    g_string_append_printf(review, "Kaynak SHA-256: %s\nKatalog: %" G_GINT64_FORMAT " yeni, %" G_GINT64_FORMAT " güncellenen, %" G_GINT64_FORMAT " korunan, %" G_GINT64_FORMAT " çakışan parça.\n",
        radios_json_string(object, "source_sha256", ""), radios_json_int(counts, "create", 0),
        radios_json_int(counts, "update", 0), radios_json_int(counts, "preserve", 0), radios_json_int(counts, "conflict", 0));
    for (guint i = 0; destinations && i < json_array_get_length(destinations); ++i) {
        JsonObject *destination = json_array_get_object_element(destinations, i);
        g_string_append_printf(review, "\n%s: %" G_GINT64_FORMAT " mevcut · %" G_GINT64_FORMAT " eklenecek · %" G_GINT64_FORMAT " tekrar atlanacak",
            radios_json_string(destination, "name", ""), radios_json_int(destination, "existing_count", 0),
            radios_json_int(destination, "added_count", 0), radios_json_int(destination, "skipped", 0));
    }
    const gchar *loss_keys[] = { "projection_losses", "domain_losses" };
    for (guint section = 0; section < G_N_ELEMENTS(loss_keys); ++section) {
        JsonArray *losses = json_object_get_array_member(object, loss_keys[section]);
        for (guint i = 0; losses && i < json_array_get_length(losses); ++i) {
            JsonObject *loss = json_array_get_object_element(losses, i);
            g_string_append_printf(review, "\n\n%s · %s: %s", radios_json_string(loss, "source_id", radios_json_string(loss, "object_key", "")),
                radios_json_string(loss, "field", radios_json_string(loss, "source_field", "")), radios_json_string(loss, "detail", ""));
        }
    }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(import->review)), review->str, -1);
    g_string_free(review, TRUE);
    xml_update_action(import);
}

static void
xml_preview_request(XmlImport *import, gboolean discovery)
{
    GtkWidget *page = xml_page(import);
    PlaylistView *view = page ? g_object_get_data(G_OBJECT(page), "playlist-view") : NULL;
    if (!view || !radios_data_connected(view->ctx) || !import->xml_base64 || import->busy) {
        gtk_label_set_text(GTK_LABEL(import->status), "Veritabanı bağlantısı hazır değil; tracklist seçiminiz korunuyor.");
        g_clear_object(&page); return;
    }
    JsonNode *body = xml_request_body(import, view, discovery);
    import->busy = TRUE; xml_update_action(import);
    gtk_label_set_text(GTK_LABEL(import->status), "Tracklist ve hedef playlistler doğrulanıyor…");
    if (!radios_data_request(view->ctx, import->dialog, "POST", "/v1/workflows/desktop-rekordbox/preview", body, xml_preview_done, NULL, NULL)) {
        import->busy = FALSE; xml_update_action(import);
    }
    json_node_free(body); g_clear_object(&page);
}

static void
xml_worker(GTask *task, gpointer object, gpointer task_data, GCancellable *cancel)
{
    ImportSource *source = task_data;
    const gsize limit = (source->device ? 16u : 1u) * 1024 * 1024;
    GError *error = NULL;
    gchar *contents = g_malloc(limit + 1); gsize length = 0;
    (void)object;
    gint descriptor = -1;
    if (!g_cancellable_set_error_if_cancelled(cancel, &error)) {
        if (source->device) {
            gint directory = g_open(source->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
            const gchar *components[] = {"PIONEER", "rekordbox"};
            for (guint component = 0; directory >= 0 && component < G_N_ELEMENTS(components); ++component) {
                gint child = openat(directory, components[component], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                gint saved_errno = errno; close(directory); directory = child; errno = saved_errno;
            }
            if (directory >= 0) {
                descriptor = openat(directory, "export.pdb", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
                gint saved_errno = errno; close(directory); errno = saved_errno;
            }
        } else descriptor = g_open(source->path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0);
        if (descriptor < 0) g_set_error(&error, G_IO_ERROR, g_io_error_from_errno(errno),
            source->device ? "USB Device Library bulunamadı: PIONEER/rekordbox/export.pdb. OneLibrary/exportLibrary.db tek başına desteklenmiyor. (%s)" : "Dosya açılamadı: %s", g_strerror(errno));
    }
    if (descriptor >= 0) {
        struct stat info;
        if (fstat(descriptor, &info) != 0) g_set_error(&error, G_IO_ERROR, g_io_error_from_errno(errno), "Kaynak incelenemedi: %s", g_strerror(errno));
        else if (!S_ISREG(info.st_mode)) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE, "Tracklist kaynağı düzenli bir dosya olmalı.");
        else if (info.st_size > (off_t)limit) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
            source->device ? "USB export.pdb en fazla 16 MiB olabilir." : "Eski XML kaynağı en fazla 1 MiB olabilir.");
        while (!error && length <= limit) {
            if (g_cancellable_set_error_if_cancelled(cancel, &error)) break;
            ssize_t count = read(descriptor, contents + length, MIN((gsize)65536, limit + 1 - length));
            if (count == 0) break;
            if (count < 0) {
                if (errno == EINTR) continue;
                g_set_error(&error, G_IO_ERROR, g_io_error_from_errno(errno), "Tracklist kaynağı okunamadı: %s", g_strerror(errno)); break;
            }
            length += (gsize)count;
        }
        struct stat after;
        if (!error && (fstat(descriptor, &after) != 0 || after.st_size != info.st_size ||
            after.st_mtim.tv_sec != info.st_mtim.tv_sec || after.st_mtim.tv_nsec != info.st_mtim.tv_nsec ||
            after.st_ctim.tv_sec != info.st_ctim.tv_sec || after.st_ctim.tv_nsec != info.st_ctim.tv_nsec || length != (gsize)info.st_size))
            g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_BUSY, "Tracklist okunurken değişti; yeniden seçin.");
        close(descriptor);
    }
    if (!error && length > limit) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Tracklist boyut sınırını aşıyor.");
    if (!error && length == 0) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Tracklist kaynağı boş.");
    if (!error) g_cancellable_set_error_if_cancelled(cancel, &error);
    gchar *encoded = NULL;
    if (!error && source->device) {
        GOutputStream *memory = g_memory_output_stream_new_resizable();
        GZlibCompressor *compressor = g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB, 6);
        GOutputStream *stream = g_converter_output_stream_new(memory, G_CONVERTER(compressor));
        if (g_output_stream_write_all(stream, contents, length, NULL, cancel, &error) && g_output_stream_close(stream, cancel, &error)) {
            gsize size = g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(memory));
            if (size > 4u * 1024 * 1024) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Sıkıştırılmış USB verisi 4 MiB sınırını aşıyor.");
            else encoded = g_base64_encode(g_memory_output_stream_get_data(G_MEMORY_OUTPUT_STREAM(memory)), size);
        }
        g_object_unref(stream); g_object_unref(compressor); g_object_unref(memory);
    } else if (!error) encoded = g_base64_encode((const guchar *)contents, length);
    g_free(contents);
    if (error) g_task_return_error(task, error); else g_task_return_pointer(task, encoded, g_free);
}

static void import_source_free(gpointer data)
{ ImportSource *source = data; g_free(source->path); g_free(source); }

static void
xml_loaded(GObject *object, GAsyncResult *result, gpointer unused)
{
    XmlImport *import = g_object_get_data(object, "pw-xml-import");
    GError *error = NULL; gchar *encoded = g_task_propagate_pointer(G_TASK(result), &error); (void)unused;
    if (import->closed || g_task_get_cancellable(G_TASK(result)) != import->cancel) { g_clear_error(&error); g_free(encoded); return; }
    import->busy = FALSE;
    if (error) { gtk_label_set_text(GTK_LABEL(import->status), error->message); g_clear_error(&error); xml_update_action(import); return; }
    g_free(import->xml_base64); import->xml_base64 = encoded;
    xml_update_action(import); xml_preview_request(import, TRUE);
}

static void
import_file_selected(GtkFileChooserButton *chooser, gpointer data)
{
    XmlImport *import = data;
    if (import->busy || import->retry_body || import->updating) return;
    if (import->cancel) g_cancellable_cancel(import->cancel);
    g_clear_object(&import->cancel); import->cancel = g_cancellable_new();
    g_clear_pointer(&import->xml_base64, g_free); g_clear_pointer(&import->preview_hash, g_free);
    import->collection_tracks = 0; import->choices_loaded = FALSE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(import->choices));
    for (GList *item = children; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(children); xml_update_action(import);
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    if (!filename) { gtk_label_set_text(GTK_LABEL(import->status), "Dosya seçilmedi."); return; }
    import->busy = TRUE; xml_update_action(import);
    ImportSource *source = g_new0(ImportSource, 1); source->device = import_device(import);
    source->path = g_canonicalize_filename(filename, NULL); g_free(filename);
    g_clear_pointer(&import->usb_root, g_free);
    if (source->device) {
        gchar *base = g_path_get_basename(source->path);
        if (g_str_equal(base, "rekordbox")) {
            gchar *parent = g_path_get_dirname(source->path), *parent_name = g_path_get_basename(parent);
            if (g_str_equal(parent_name, "PIONEER")) { g_free(source->path); source->path = g_path_get_dirname(parent); }
            g_free(parent); g_free(parent_name);
        } else if (g_str_equal(base, "PIONEER")) {
            gchar *parent = g_path_get_dirname(source->path); g_free(source->path); source->path = parent;
        }
        g_free(base); import->usb_root = g_strdup(source->path);
        const gchar *key = gtk_entry_get_text(GTK_ENTRY(import->library_key));
        const gchar *mount = gtk_combo_box_get_active_id(GTK_COMBO_BOX(import->sources));
        if ((g_strcmp0(key, import->auto_library_key) == 0 && g_strcmp0(mount, source->path) != 0) ||
            g_str_has_prefix(key, "usb-path-") || g_str_equal(key, "native-rekordbox-usb")) {
            gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, source->path, -1);
            gchar *identity = g_strdup_printf("usb-path-%.32s", digest); gtk_entry_set_text(GTK_ENTRY(import->library_key), identity);
            g_free(import->auto_library_key); import->auto_library_key = g_strdup(identity);
            g_free(digest); g_free(identity);
        }
    }
    gtk_label_set_text(GTK_LABEL(import->status), source->device ? "USB / SD tracklist okunuyor…" : "Eski XML kopyası okunuyor…");
    GTask *task = g_task_new(import->dialog, import->cancel, xml_loaded, NULL);
    g_task_set_task_data(task, source, import_source_free); g_task_run_in_thread(task, xml_worker); g_object_unref(task);
}

static void
xml_choose_all(GtkButton *button, gpointer data)
{
    XmlImport *import = data;
    gboolean selected = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "pw-select-all"));
    GList *children = gtk_container_get_children(GTK_CONTAINER(import->choices));
    for (GList *item = children; item; item = item->next)
        if (GTK_IS_TOGGLE_BUTTON(item->data)) gtk_toggle_button_set_active(item->data, selected);
    g_list_free(children);
}

static void
xml_import_destroyed(GtkWidget *widget, gpointer data)
{
    XmlImport *import = data; (void)widget; import->closed = TRUE;
    if (import->cancel) g_cancellable_cancel(import->cancel);
    GtkWidget *page = xml_page(import);
    if (page) {
        PlaylistView *view = g_object_get_data(G_OBJECT(page), "playlist-view");
        if (view->import_dialog == import->dialog) view->import_dialog = NULL;
    }
    g_clear_object(&page);
}

static void
xml_import_free(gpointer data)
{
    XmlImport *import = data;
    g_clear_object(&import->cancel); g_weak_ref_clear(&import->page);
    if (import->volumes) g_signal_handlers_disconnect_by_data(import->volumes, import);
    g_clear_object(&import->volumes);
    g_free(import->xml_base64); g_free(import->preview_hash); g_free(import->usb_root); g_free(import->auto_library_key);
    g_clear_pointer(&import->retry_body, json_node_free); g_free(import);
}

static void
xml_apply_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result,
               const GError *error, gpointer data)
{
    XmlImport *import = g_object_get_data(G_OBJECT(owner), "pw-xml-import");
    XmlOperation *operation = data;
    import->busy = FALSE;
    GtkWidget *page = xml_page(import);
    PlaylistView *view = page ? g_object_get_data(G_OBJECT(page), "playlist-view") : NULL;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        gtk_label_set_text(GTK_LABEL(import->status), error ? error->message : "İçe aktarım sonucu doğrulanamadı; aynı işlemi yeniden deneyin.");
        gboolean rejected = error && error->domain == RADIOS_DATA_CLIENT_ERROR &&
            (error->code == RADIOS_DATA_CLIENT_ERROR_CONFLICT || error->code == RADIOS_DATA_CLIENT_ERROR_VALIDATION ||
             error->code == RADIOS_DATA_CLIENT_ERROR_FORBIDDEN || error->code == RADIOS_DATA_CLIENT_ERROR_NOT_FOUND);
        if (rejected) { g_clear_pointer(&import->retry_body, json_node_free); g_clear_pointer(&import->preview_hash, g_free); }
        xml_update_action(import); g_clear_object(&page); return;
    }
    JsonObject *object = json_node_get_object(result);
    JsonArray *lists = json_object_get_array_member(object, "playlists");
    gchar *message = g_strdup_printf("%u playlist içeri aktarıldı; seçilen cue politikası uygulandı. Ses dosyaları doğrulanmadı.", lists ? json_array_get_length(lists) : 0);
    if (view) { view->busy = FALSE; playlist_notice(view, message); playlist_data_refresh(ctx, page); }
    operation->applying = FALSE;
    g_free(message); gtk_widget_destroy(owner); g_clear_object(&page);
}

static void
playlist_import_response(GtkDialog *dialog, gint response, gpointer data)
{
    XmlImport *import = data;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    if (import->busy) return;
    if (!import->preview_hash && !import->retry_body) { xml_preview_request(import, !import->choices_loaded); return; }
    GtkWidget *page = xml_page(import);
    PlaylistView *view = page ? g_object_get_data(G_OBJECT(page), "playlist-view") : NULL;
    if (!view || view->busy || (view->loading && !import->retry_body) || !radios_data_connected(view->ctx)) {
        gtk_label_set_text(GTK_LABEL(import->status), "Veritabanı / playlist kaydı hazır değil; seçiminiz korunuyor."); g_clear_object(&page); return;
    }
    if (!import->retry_body) {
        import->retry_body = xml_request_body(import, view, FALSE);
        JsonObject *body = json_node_get_object(import->retry_body);
        json_object_set_string_member(body, "expected_preview_hash", import->preview_hash);
        gchar *key = g_uuid_string_random(); json_object_set_string_member(body, "idempotency_key", key); g_free(key);
    }
    XmlOperation *operation = g_new0(XmlOperation, 1); g_weak_ref_init(&operation->page, page); operation->applying = TRUE;
    view->busy = TRUE; playlist_refresh(view); import->busy = TRUE; xml_update_action(import);
    gtk_label_set_text(GTK_LABEL(import->status), "Seçili tracklist ve playlistler kaydediliyor…");
    if (!radios_data_request(view->ctx, import->dialog, "POST", "/v1/workflows/desktop-rekordbox/apply",
        import->retry_body, xml_apply_done, operation, xml_operation_free)) {
        import->busy = FALSE; xml_update_action(import);
    }
    g_clear_object(&page);
}

static void
import_format_changed(GtkComboBox *combo, gpointer data)
{
    (void)combo; XmlImport *import = data;
    if (import->closed || import->busy || import->retry_body) return;
    gboolean device = import_device(import);
    gtk_stack_set_visible_child_name(GTK_STACK(import->inputs), device ? "usb" : "xml");
    gtk_widget_set_sensitive(import->cues, !device);
    g_clear_pointer(&import->xml_base64, g_free); g_clear_pointer(&import->preview_hash, g_free);
    g_clear_pointer(&import->usb_root, g_free);
    g_clear_pointer(&import->auto_library_key, g_free);
    import->collection_tracks = 0; import->choices_loaded = FALSE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(import->choices));
    for (GList *item = children; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(children);
    gtk_entry_set_text(GTK_ENTRY(import->library_key), device ? "native-rekordbox-usb" : "native-rekordbox");
    gtk_label_set_text(GTK_LABEL(import->status), device ? "USB / SD aygıtını veya kök klasörünü seç." : "Eski XML dosyasını seç.");
    xml_update_action(import);
}

static void
import_mounts_changed(GVolumeMonitor *monitor, GMount *mount, gpointer data)
{
    (void)mount; XmlImport *import = data; if (import->closed) return;
    gchar *selected = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(import->sources)));
    gboolean updating = import->updating; import->updating = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(import->sources));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(import->sources), "-1", "Klasörden seç");
    GList *mounts = g_volume_monitor_get_mounts(monitor);
    for (GList *item = mounts; item; item = item->next) {
        GMount *device = item->data; GFile *root = g_mount_get_root(device);
        gchar *path = g_file_get_path(root), *name = g_mount_get_name(device);
        if (path && g_mount_can_unmount(device)) {
            gchar *label = g_strdup_printf("%s · %s", name, path);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(import->sources), path, label); g_free(label);
        }
        g_free(path); g_free(name); g_object_unref(root);
    }
    g_list_free_full(mounts, g_object_unref);
    if (!selected || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(import->sources), selected))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(import->sources), "-1");
    import->updating = updating; g_free(selected);
}

static void import_devices_refresh(GtkButton *button, gpointer data)
{ (void)button; XmlImport *import = data; import_mounts_changed(import->volumes, NULL, import); }

static void
import_device_changed(GtkComboBox *combo, gpointer data)
{
    XmlImport *import = data; if (import->closed || import->updating || import->busy || import->retry_body) return;
    const gchar *path = gtk_combo_box_get_active_id(combo); if (!path || g_str_equal(path, "-1")) return;
    GList *mounts = g_volume_monitor_get_mounts(import->volumes);
    for (GList *item = mounts; item; item = item->next) {
        GMount *mount = item->data; GFile *root = g_mount_get_root(mount); gchar *location = g_file_get_path(root);
        if (g_strcmp0(location, path) == 0) {
            gchar *uuid = g_mount_get_uuid(mount);
            if (uuid) {
                gchar *identity = g_strdup_printf("usb-%.100s", uuid);
                g_free(import->auto_library_key); import->auto_library_key = g_strdup(identity);
                gtk_entry_set_text(GTK_ENTRY(import->library_key), identity); g_free(identity);
            }
            g_free(uuid);
        }
        g_free(location); g_object_unref(root);
    }
    g_list_free_full(mounts, g_object_unref);
    if (gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(import->folder), path))
        import_file_selected(GTK_FILE_CHOOSER_BUTTON(import->folder), import);
}

static void
playlist_import(GtkButton *button, gpointer data)
{
    PlaylistView *view = data; (void)button;
    if (view->import_dialog) { gtk_window_present(GTK_WINDOW(view->import_dialog)); return; }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("rekordbox USB / SD tracklist", view->ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL, "Önizlemeyi doğrula", GTK_RESPONSE_ACCEPT, NULL);
    view->import_dialog = dialog;
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    XmlImport *import = g_new0(XmlImport, 1); g_weak_ref_init(&import->page, view->widget);
    import->dialog = dialog; import->controls = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, NULL);
    import->format = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(import->format), "usb", "USB / SD · CDJ Device Library");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(import->format), "xml", "Eski XML · uyumluluk");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(import->format), "usb");
    import->folder = gtk_file_chooser_button_new("USB / SD kök klasörü", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    import->file = gtk_file_chooser_button_new("Eski rekordbox XML dosyası", GTK_FILE_CHOOSER_ACTION_OPEN);
    import->sources = gtk_combo_box_text_new(); import->volumes = g_volume_monitor_get();
    import->inputs = gtk_stack_new(); gtk_stack_set_homogeneous(GTK_STACK(import->inputs), FALSE);
    import->status = radios_ui_label("USB / SD aygıtını veya kök klasörünü seç.", "pw-status");
    import->choices = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "pw-import-lists");
    import->selected = radios_ui_label("0 playlist seçili", "pw-status");
    import->review = gtk_text_view_new(); gtk_text_view_set_editable(GTK_TEXT_VIEW(import->review), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(import->review), FALSE); gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(import->review), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_name(import->review, "pw-import-review");
    import->collection = gtk_check_button_new_with_label("Koleksiyondaki tüm parçaları ayrıca tek listeye al");
    import->library_key = radios_ui_entry("Aynı USB kütüphanesinin sonraki aktarımlarında bu kimliği koru");
    gtk_entry_set_text(GTK_ENTRY(import->library_key), "native-rekordbox-usb"); gtk_entry_set_max_length(GTK_ENTRY(import->library_key), 120);
    const gchar *names[] = { "Mevcut listeye ekle", "Yeni kopya oluştur", NULL };
    const gchar *duplicates[] = { "Aynı dosya tekrarlarını atla", "Tekrarları koru", NULL };
    const gchar *cues[] = { "DJ cue bilgilerini koru", "Radyo cue incelemesine öner", "Cue bilgilerini atla", NULL };
    import->name_policy = radios_ui_combo(names, 0); import->duplicates = radios_ui_combo(duplicates, 0); import->cues = radios_ui_combo(cues, 0);
    GtkWidget *named[] = { import->file, import->status, import->choices, import->name_policy, import->duplicates, import->cues, import->collection, import->library_key };
    const gchar *ids[] = { "pw-import-file", "pw-import-status", "pw-import-choices", "pw-import-name-policy", "pw-import-duplicates", "pw-import-cues", "pw-import-collection", "pw-import-library-key" };
    for (guint i = 0; i < G_N_ELEMENTS(named); ++i) gtk_widget_set_name(named[i], ids[i]);
    gtk_widget_set_name(import->format, "pw-import-format"); gtk_widget_set_name(import->folder, "pw-import-folder");
    gtk_widget_set_name(import->sources, "pw-import-devices");
    gtk_label_set_max_width_chars(GTK_LABEL(import->status), 85);
    g_object_set_data_full(G_OBJECT(dialog), "pw-xml-import", import, xml_import_free);
    g_signal_connect(dialog, "destroy", G_CALLBACK(xml_import_destroyed), import);
    g_signal_connect_object(view->widget, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    gtk_widget_set_name(dialog, "pw-import-dialog");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "pw-dialog");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "pw-import-dialog");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE); gtk_window_set_default_size(GTK_WINDOW(dialog), 700, 580);
    gtk_container_set_border_width(GTK_CONTAINER(body), 22); gtk_box_set_spacing(GTK_BOX(body), 12);
    gtk_box_pack_start(GTK_BOX(body), import->controls, TRUE, TRUE, 0);
    GtkFileFilter *filter = gtk_file_filter_new(); gtk_file_filter_set_name(filter, "rekordbox XML (*.xml)");
    gtk_file_filter_add_pattern(filter, "*.xml"); gtk_file_filter_add_mime_type(filter, "application/xml");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(import->file), filter);
    gtk_box_pack_start(GTK_BOX(import->controls), import->format, FALSE, FALSE, 0);
    GtkWidget *device_input = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL);
    GtkWidget *devices = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    GtkWidget *refresh = radios_ui_button("Aygıtları yenile"); gtk_widget_set_name(refresh, "pw-import-devices-refresh");
    gtk_box_pack_start(GTK_BOX(devices), import->sources, TRUE, TRUE, 0); gtk_box_pack_start(GTK_BOX(devices), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(device_input), devices, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(device_input), import->folder, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(import->inputs), device_input, "usb");
    gtk_stack_add_named(GTK_STACK(import->inputs), import->file, "xml");
    gtk_stack_set_visible_child_name(GTK_STACK(import->inputs), "usb");
    gtk_box_pack_start(GTK_BOX(import->controls), import->inputs, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(import->controls), radios_ui_label("Kaynak kütüphane kimliği", NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(import->controls), import->library_key, FALSE, FALSE, 0);
    GtkWidget *options = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 12, "pw-import-options");
    const gchar *labels[] = { "Aynı adlı playlist", "Aynı dosya tekrarları", "Cue / işaretler" };
    GtkWidget *combos[] = { import->name_policy, import->duplicates, import->cues };
    for (guint i = 0; i < 3; ++i) {
        GtkWidget *field = radios_ui_box(GTK_ORIENTATION_VERTICAL, 7, NULL);
        gtk_box_pack_start(GTK_BOX(field), radios_ui_label(labels[i], NULL), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(field), combos[i], FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(options), field, TRUE, TRUE, 0);
        g_signal_connect(combos[i], "changed", G_CALLBACK(xml_selection_changed), import);
    }
    gtk_box_pack_start(GTK_BOX(import->controls), options, FALSE, FALSE, 0);
    GtkWidget *selection = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "pw-actions");
    const gchar *select_names[] = { "Tümünü seç", "Seçimi temizle" };
    for (guint i = 0; i < 2; ++i) {
        GtkWidget *select = radios_ui_button(select_names[i]); g_object_set_data(G_OBJECT(select), "pw-select-all", GINT_TO_POINTER(i == 0));
        g_signal_connect(select, "clicked", G_CALLBACK(xml_choose_all), import); gtk_box_pack_start(GTK_BOX(selection), select, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(import->controls), selection, FALSE, FALSE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_widget_set_size_request(scroll, -1, 130);
    gtk_container_add(GTK_CONTAINER(scroll), import->choices); gtk_box_pack_start(GTK_BOX(import->controls), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(import->controls), import->collection, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), import->selected, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(body), import->status, FALSE, FALSE, 0);
    GtkWidget *review_expander = gtk_expander_new("Hedef listeler, katalog kararları ve aktarım uyarıları");
    GtkWidget *review_scroll = gtk_scrolled_window_new(NULL, NULL); gtk_widget_set_size_request(review_scroll, -1, 140);
    gtk_container_add(GTK_CONTAINER(review_scroll), import->review); gtk_container_add(GTK_CONTAINER(review_expander), review_scroll);
    gtk_box_pack_start(GTK_BOX(body), review_expander, FALSE, FALSE, 0);
    g_signal_connect(import->file, "file-set", G_CALLBACK(import_file_selected), import);
    g_signal_connect(import->folder, "file-set", G_CALLBACK(import_file_selected), import);
    g_signal_connect(import->format, "changed", G_CALLBACK(import_format_changed), import);
    g_signal_connect(import->sources, "changed", G_CALLBACK(import_device_changed), import);
    g_signal_connect(refresh, "clicked", G_CALLBACK(import_devices_refresh), import);
    g_signal_connect(import->volumes, "mount-added", G_CALLBACK(import_mounts_changed), import);
    g_signal_connect(import->volumes, "mount-removed", G_CALLBACK(import_mounts_changed), import);
    import_mounts_changed(import->volumes, NULL, import); gtk_widget_set_sensitive(import->cues, FALSE);
    g_signal_connect(import->collection, "toggled", G_CALLBACK(xml_selection_changed), import);
    g_signal_connect(import->library_key, "changed", G_CALLBACK(xml_selection_changed), import);
    g_signal_connect(dialog, "response", G_CALLBACK(playlist_import_response), import);
    xml_update_action(import); gtk_widget_show_all(dialog);
}

static GtkWidget *
playlist_action(GtkWidget *box, const gchar *text, PlaylistView *view, gboolean enabled)
{
    GtkWidget *button = radios_ui_button(text);
    gtk_widget_set_sensitive(button, enabled);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    (void)view;
    return button;
}

GtkWidget *
radios_playlist_new(RadioSViewContext *ctx)
{
    PlaylistView *view = g_new0(PlaylistView, 1);
    /* Spacing comes from the design's own margins (playlist-workspace.css). */
    GtkWidget *page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, "task-view playlist-workspace");
    playlist_builder_style(page);
    GtkWidget *import = radios_ui_button("rekordbox USB / SD oku");
    gtk_widget_set_name(import, "pw-import");
    GtkWidget *summary = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 18, "pw-summary");
    GtkWidget *layout = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_name(layout, "pw-split");
    gtk_style_context_add_class(gtk_widget_get_style_context(layout), "pw-layout");
    GtkWidget *content = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "pw-content");
    gtk_widget_set_name(content, "pw-target-panel");
    view->ctx = ctx;
    view->current = -1;
    view->drafts = g_ptr_array_new_with_free_func(draft_free);
    g_object_set_data_full(G_OBJECT(page), "playlist-view", view, playlist_free);
    view->widget = page;
    g_signal_connect(page, "destroy", G_CALLBACK(playlist_destroyed), view);
    view->browser = radios_playlist_browser_new(ctx);
    if (ctx->parent) g_object_set_data(G_OBJECT(ctx->parent), "radios-playlist-view", view);
    GtkWidget *source_add = playlist_find_named(view->browser, "pw-source-add");
    g_signal_connect(source_add, "clicked", G_CALLBACK(playlist_source_add), view);
    gtk_paned_pack1(GTK_PANED(layout), view->browser, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(layout), content, TRUE, FALSE);
    gtk_paned_set_position(GTK_PANED(layout), 270);
    gtk_container_set_border_width(GTK_CONTAINER(content), 6);
    GtkWidget *tabs = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "pw-target-tabs");
    view->lists = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "pw-playlists");
    gtk_widget_set_name(view->lists, "pw-playlists");
    GtkWidget *tab_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_widget_set_size_request(tab_scroll, -1, 38); gtk_container_add(GTK_CONTAINER(tab_scroll), view->lists);
    gtk_box_pack_start(GTK_BOX(tabs), tab_scroll, TRUE, TRUE, 0);
    GtkWidget *add = radios_ui_button("+"); gtk_widget_set_name(add, "pw-new-playlist");
    gtk_widget_set_tooltip_text(add, "Yeni playlist ekle");
    gtk_box_pack_start(GTK_BOX(tabs), add, FALSE, FALSE, 0);
    g_signal_connect(add, "clicked", G_CALLBACK(playlist_create_open), view);
    gtk_box_pack_start(GTK_BOX(content), tabs, FALSE, FALSE, 0);
    view->list_count = radios_ui_label("0 playlist", NULL);
    view->track_count = radios_ui_label("0 parça", NULL);
    gtk_box_pack_start(GTK_BOX(summary), view->list_count, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(summary), view->track_count, FALSE, FALSE, 0);
    /* rekordbox reading sits in the source strip beside Ana medya, Sağlayıcılar
     * and Yenile; the media library offers the same reader. */
    GtkWidget *source_refresh = playlist_find_named(view->browser, "pw-source-refresh");
    GtkWidget *source_strip = source_refresh ? gtk_widget_get_parent(source_refresh) : NULL;
    if (GTK_IS_BOX(source_strip)) {
        gtk_box_pack_start(GTK_BOX(source_strip), import, FALSE, FALSE, 0);
        GValue position = G_VALUE_INIT; g_value_init(&position, G_TYPE_INT);
        gtk_container_child_get_property(GTK_CONTAINER(source_strip), source_refresh, "position", &position);
        gtk_box_reorder_child(GTK_BOX(source_strip), import, g_value_get_int(&position)); g_value_unset(&position);
    } else gtk_box_pack_end(GTK_BOX(summary), import, FALSE, FALSE, 0);
    gtk_widget_set_tooltip_text(import, "rekordbox USB / SD tracklist veya XML dosyasını oku");
    g_signal_connect(import, "clicked", G_CALLBACK(playlist_import), view);
    /* "+" beside the reader: a music folder becomes a playlist of its name. */
    GtkWidget *folder_add = radios_ui_button("+");
    gtk_widget_set_name(folder_add, "pw-import-folder");
    gtk_widget_set_tooltip_text(folder_add, "Müzik klasörü ekle · parçalar Ana medyaya alınır, klasör adıyla playlist oluşur");
    g_signal_connect_swapped(folder_add, "clicked", G_CALLBACK(radios_media_folder_import_open), ctx);
    if (GTK_IS_BOX(source_strip)) {
        gtk_box_pack_start(GTK_BOX(source_strip), folder_add, FALSE, FALSE, 0);
        gtk_box_reorder_child(GTK_BOX(source_strip), source_refresh, -1);
    } else gtk_box_pack_end(GTK_BOX(summary), folder_add, FALSE, FALSE, 0);
    GtkWidget *selected = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 16, "pw-playlist-heading");
    GtkWidget *details = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL);
    view->heading = radios_ui_label("Playlist seçilmedi", "h3 heading");
    view->details = radios_ui_label("", "task-help");
    gtk_box_pack_start(GTK_BOX(details), view->heading, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(details), view->details, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(selected), details, TRUE, TRUE, 0);
    view->target_selector = gtk_combo_box_text_new(); gtk_widget_set_name(view->target_selector, "pw-target-playlist");
    gtk_widget_set_tooltip_text(view->target_selector, "Alttaki tabloda düzenlenecek playlist");
    gtk_box_pack_start(GTK_BOX(selected), view->target_selector, TRUE, TRUE, 0);
    g_signal_connect(view->target_selector, "changed", G_CALLBACK(playlist_target_changed), view);
    GtkWidget *name_actions = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "pw-actions");
    view->buttons[PW_RENAME] = playlist_action(name_actions, "Adı değiştir", view, FALSE);
    view->buttons[PW_DELETE] = playlist_action(name_actions, "Sil", view, FALSE);
    gtk_box_pack_start(GTK_BOX(selected), name_actions, FALSE, FALSE, 0);
    /* Target tabs own selection. Keep this binding control out of the visible
     * layout so playlist creation does not duplicate the management strip. */
    gtk_widget_set_no_show_all(selected, TRUE);
    gtk_widget_hide(selected);
    gtk_box_pack_start(GTK_BOX(content), selected, FALSE, FALSE, 0);
    GtkWidget *manage = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(manage), "⋯");
    gtk_widget_set_name(manage, "pw-playlist-options");
    gtk_widget_set_tooltip_text(manage, "Seçili playlistin seçenekleri");
    GtkWidget *options = gtk_popover_new(manage);
    g_object_ref(name_actions);
    gtk_container_remove(GTK_CONTAINER(selected), name_actions);
    gtk_container_add(GTK_CONTAINER(options), name_actions);
    g_object_unref(name_actions);
    gtk_widget_show_all(name_actions);
    g_signal_connect_object(view->buttons[PW_RENAME], "clicked", G_CALLBACK(gtk_popover_popdown), options, G_CONNECT_SWAPPED);
    g_signal_connect_object(view->buttons[PW_DELETE], "clicked", G_CALLBACK(gtk_popover_popdown), options, G_CONNECT_SWAPPED);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(manage), options);
    gtk_box_pack_start(GTK_BOX(tabs), manage, FALSE, FALSE, 0);
    GtkWidget *toolbar = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 16, "pw-toolbar");
    GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "pw-actions");
    GtkWidget *new_track = playlist_action(actions, "+ Parça", view, FALSE);
    GtkWidget *edit_track = playlist_action(actions, "Düzenle", view, FALSE);
    view->buttons[PW_NEW] = new_track; view->buttons[PW_EDIT] = edit_track;
    view->buttons[PW_REMOVE] = playlist_action(actions, "Seçilenleri çıkar", view, FALSE);
    view->buttons[PW_UP] = playlist_action(actions, "↑", view, FALSE);
    view->buttons[PW_DOWN] = playlist_action(actions, "↓", view, FALSE);
    if (ctx->playout) {
        view->play = radios_ui_button("▶"); gtk_widget_set_name(view->play, "pw-play");
        gtk_widget_set_tooltip_text(view->play, "Playlisti boş bir canlı oynatıcıda çal");
        gtk_box_pack_start(GTK_BOX(actions), view->play, FALSE, FALSE, 0); g_signal_connect(view->play, "clicked", G_CALLBACK(playlist_play), view);
    }
    gtk_box_pack_start(GTK_BOX(toolbar), actions, FALSE, FALSE, 0);
    GtkWidget *filters = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "pw-filter");
    const gchar *sorts[] = { "Playlist sırası", "Parça adı", "Sanatçı", "Albüm", "Süre", "BPM", NULL };
    view->search = radios_ui_entry("Parçalarda ara…");
    GtkWidget *sort = radios_ui_combo(sorts, 0);
    view->sort = sort;
    gtk_widget_set_name(view->search, "pw-track-search");
    gtk_widget_set_name(sort, "pw-sort");
    gtk_box_pack_start(GTK_BOX(filters), view->search, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(filters), sort, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(toolbar), filters, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), toolbar, FALSE, FALSE, 0);
    const gchar *headers[] = { "", "#", "TIME", "RTS", "TITLE", "TITRO", "DURATION", "EOM", "TRIGGER", "VT", "TRIGGER", "CUE", "" };
    GtkWidget *table = radios_ui_table(headers, G_N_ELEMENTS(headers));
    view->table = g_object_get_data(G_OBJECT(table), "tree-view");
    gtk_widget_set_name(view->table, "pw-builder-table");
    for (gint i = 0; i < (gint)G_N_ELEMENTS(headers); ++i) {
        GtkTreeViewColumn *column = gtk_tree_view_get_column(GTK_TREE_VIEW(view->table), i);
        gtk_tree_view_column_set_expand(column, i == 12);
        gtk_tree_view_column_set_min_width(column, i == 1 || i == 9 || i == 12 ? 32 : i == 4 ? 82 : 74);
        if (i == 4) { gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED); gtk_tree_view_column_set_fixed_width(column, 82); }
        if (i >= 1) {
            gtk_cell_layout_clear(GTK_CELL_LAYOUT(column)); GtkCellRenderer *cell = i == 12 ? gtk_cell_renderer_pixbuf_new() : gtk_cell_renderer_text_new();
            gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(column), cell, TRUE);
            if (i == 12) g_object_set(cell, "xalign", 1.0, "xpad", 8u, NULL);
            g_object_set_data(G_OBJECT(cell), "playlist-view", view);
            gtk_tree_view_column_set_cell_data_func(column, cell, playlist_builder_cell, GINT_TO_POINTER(i - 1), NULL);
        }
    }
    view->store = gtk_list_store_new(10, G_TYPE_BOOLEAN, G_TYPE_INT, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_STRING, G_TYPE_BOOLEAN);
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(view->store), 5, playlist_numeric_sort, GINT_TO_POINTER(5), NULL);
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(view->store), 6, playlist_numeric_sort, GINT_TO_POINTER(6), NULL);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(view->store), 1, GTK_SORT_ASCENDING);
    g_object_set_data(G_OBJECT(table), "list-store", view->store);
    view->filter = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new(GTK_TREE_MODEL(view->store), NULL));
    gtk_tree_model_filter_set_visible_func(view->filter, playlist_visible, view, NULL);
    gtk_tree_view_set_model(GTK_TREE_VIEW(view->table), GTK_TREE_MODEL(view->filter));
    g_object_unref(view->store);
    g_object_unref(view->filter);
    GtkTreeViewColumn *checkbox_column = gtk_tree_view_get_column(GTK_TREE_VIEW(view->table), 0);
    gtk_cell_layout_clear(GTK_CELL_LAYOUT(checkbox_column));
    GtkCellRenderer *checkbox = gtk_cell_renderer_toggle_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(checkbox_column), checkbox, FALSE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(checkbox_column), checkbox, "active", 0);
    g_signal_connect(checkbox, "toggled", G_CALLBACK(playlist_row_toggle), view);
    gtk_style_context_add_class(gtk_widget_get_style_context(view->table), "pw-table");
    gtk_tree_view_column_set_visible(checkbox_column, FALSE);
    g_signal_connect(view->table, "row-activated", G_CALLBACK(playlist_row_activated), view);
    g_signal_connect(view->table, "button-release-event", G_CALLBACK(playlist_row_clicked), view);
    GtkWidget *select_all = gtk_check_button_new();
    view->select_all = select_all;
    gtk_widget_set_tooltip_text(select_all, "Görünen tüm parçaları seç");
    gtk_tree_view_column_set_widget(gtk_tree_view_get_column(GTK_TREE_VIEW(view->table), 0), select_all);
    gtk_widget_show(select_all);
    g_signal_connect(select_all, "toggled", G_CALLBACK(playlist_select_all), view);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view->table));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
    g_signal_connect(selection, "changed", G_CALLBACK(playlist_selection_changed), view);
    g_signal_connect(view->search, "changed", G_CALLBACK(playlist_search_changed), view);
    g_signal_connect(sort, "changed", G_CALLBACK(playlist_sort), view);
    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_set_size_request(overlay, -1, 120);
    gtk_container_add(GTK_CONTAINER(overlay), table);
    GtkWidget *empty = radios_ui_label("", "pw-empty");
    view->empty = empty;
    gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), empty);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), empty, TRUE);
    gtk_box_pack_start(GTK_BOX(content), overlay, TRUE, TRUE, 0);
    GtkWidget *bottom = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "pw-bottom");
    GtkWidget *bottom_actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "pw-actions");
    view->buttons[PW_ADD_SELECTED] = playlist_action(bottom_actions, "Seçilenleri yayın sırasına ekle", view, FALSE);
    view->buttons[PW_ADD_ALL] = playlist_action(bottom_actions, "Playlisti yayın sırasına ekle", view, FALSE);
    view->buttons[PW_CSV] = playlist_action(name_actions, "CSV olarak dışa aktar", view, FALSE);
    view->buttons[PW_M3U] = playlist_action(name_actions, "M3U8 olarak dışa aktar", view, FALSE);
    for (guint action = PW_CSV; action <= PW_M3U; ++action) {
        gtk_widget_show(view->buttons[action]);
        g_signal_connect_object(view->buttons[action], "clicked", G_CALLBACK(gtk_popover_popdown), options, G_CONNECT_SWAPPED);
    }
    gtk_box_pack_start(GTK_BOX(bottom), bottom_actions, FALSE, FALSE, 0);
    /* Keep action bindings available without showing the footer in the layout. */
    gtk_widget_set_no_show_all(bottom, TRUE);
    gtk_widget_hide(bottom);
    gtk_box_pack_start(GTK_BOX(content), bottom, FALSE, FALSE, 0);
    view->source = gtk_expander_new("Seçili parçanın dosyası ve rekordbox bilgileri");
    gtk_style_context_add_class(gtk_widget_get_style_context(view->source), "pw-source");
    GtkWidget *source_body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, NULL);
    const gchar *source_names[] = { "Dosya konumu", "rekordbox kimliği", "Ton / işaretler", "Yorum" };
    for (guint i = 0; i < G_N_ELEMENTS(source_names); ++i) {
        gtk_box_pack_start(GTK_BOX(source_body), radios_ui_label(source_names[i], "form-label"), FALSE, FALSE, 0);
        view->source_values[i] = radios_ui_label("—", NULL);
        gtk_label_set_selectable(GTK_LABEL(view->source_values[i]), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(view->source_values[i]), 90);
        gtk_box_pack_start(GTK_BOX(source_body), view->source_values[i], FALSE, FALSE, 0);
    }
    GtkWidget *markers = radios_ui_button("Cue / işaret düzenleyicisi");
    gtk_widget_set_name(markers, "pw-markers"); g_signal_connect(markers, "clicked", G_CALLBACK(playlist_marker_clicked), view);
    gtk_box_pack_start(GTK_BOX(source_body), markers, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(view->source), source_body);
    gtk_widget_show_all(view->source);
    gtk_widget_set_no_show_all(view->source, TRUE);
    gtk_widget_hide(view->source);
    gtk_box_pack_start(GTK_BOX(content), view->source, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), summary, FALSE, FALSE, 0);
    GtkTargetEntry targets[] = {{RADIOS_PLAYLIST_ENTRIES_MIME, GTK_TARGET_SAME_APP, 0}};
    gtk_drag_dest_set(view->table, GTK_DEST_DEFAULT_ALL, targets, G_N_ELEMENTS(targets), GDK_ACTION_COPY);
    g_signal_connect(view->table, "drag-data-received", G_CALLBACK(playlist_drop_entries), view);
    g_signal_connect(view->table, "button-press-event", G_CALLBACK(playlist_alt_selection), view);
    gtk_drag_dest_set(overlay, GTK_DEST_DEFAULT_ALL, targets, G_N_ELEMENTS(targets), GDK_ACTION_COPY);
    g_signal_connect(overlay, "drag-data-received", G_CALLBACK(playlist_drop_entries), view);
    gtk_box_pack_start(GTK_BOX(page), layout, TRUE, TRUE, 0);
    view->status = radios_ui_label("", "pw-status");
    gtk_box_pack_start(GTK_BOX(page), view->status, FALSE, FALSE, 0);
    view->marker_host = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "pw-marker-panel");
    gtk_widget_set_name(view->marker_host, "pw-marker-host");
    gtk_widget_set_no_show_all(view->marker_host, TRUE);
    gtk_widget_hide(view->marker_host);
    gtk_box_pack_start(GTK_BOX(view->marker_host), radios_ui_label("Parça işaretleri", "h3 heading"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), view->marker_host, FALSE, FALSE, 0);
    const gchar *action_ids[] = { "pw-rename", "pw-delete", "pw-track-new", "pw-track-edit",
        "pw-track-remove", "pw-up", "pw-down", "pw-add-selected", "pw-add-all", "pw-export-csv", "pw-export-m3u" };
    for (guint i = 0; i < G_N_ELEMENTS(view->buttons); ++i) {
        g_object_set_data(G_OBJECT(view->buttons[i]), "pw-action", GINT_TO_POINTER((gint)i + 1));
        gtk_widget_set_name(view->buttons[i], action_ids[i]);
        g_signal_connect(view->buttons[i], "clicked", G_CALLBACK(playlist_action_clicked), view);
    }
    if (ctx->data) {
        gtk_widget_set_tooltip_text(import, "USB / SD export.pdb tracklistlerini özgün parça sırasıyla oku ve seçilen listeleri aktar.");
        radios_data_register_refresh(ctx, page, playlist_data_refresh);
    }
    playlist_refresh(view);
    return page;
}

gboolean radios_playlist_visible_target(RadioSViewContext *ctx, gchar **id, gint64 *revision)
{
    PlaylistView *view = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-playlist-view") : NULL;
    if (!view || view->destroyed || !gtk_widget_is_drawable(view->widget) || view->busy) return FALSE;
    PlaylistDraft *draft = playlist_current(view);
    if (!draft || !g_uuid_string_is_valid(draft->id) || draft->revision <= 0) return FALSE;
    if (id) *id = g_strdup(draft->id);
    if (revision) *revision = draft->revision;
    return TRUE;
}
void radios_playlist_rekordbox_open(RadioSViewContext *ctx)
{
    PlaylistView *view = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-playlist-view") : NULL;
    if (view && !view->destroyed) playlist_import(NULL, view);
    else if (ctx && ctx->notify) ctx->notify(ctx->notify_data, "rekordbox okuma Playlist sayfasından kullanılabilir.");
}
