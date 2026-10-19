#include "radios/editors.h"
#include "radios/widgets.h"
#include "radios/data.h"
#include "radios/audio_ui.h"
#include "radios/media_ui.h"
#include <math.h>

/* Native counterparts of transition-editor.fragment.html and
 * queue-waveform.fragment.html. The empty canvas never fabricates PCM peaks. */
#define FIELD_COUNT 8
#define HISTORY_LIMIT 80

typedef struct {
    gchar *values[FIELD_COUNT];
    gchar *autodj[2];
    gchar *files[2];
    JsonNode *bindings[2];
    gdouble preview_level;
    gboolean flag;
} Snapshot;

typedef struct {
    Snapshot *saved;
    GtkWidget *dialog;
    RadioSDocument *document;
} EditorStore;

typedef struct {
    RadioSViewContext *ctx;
    RadioSEditorKind kind;
    EditorStore *store;
    GtkWidget *dialog, *status, *canvas, *position, *flag, *save_button, *audio_owner;
    GtkWidget *fields[FIELD_COUNT], *undo_button, *redo_button;
    GtkWidget *mix[2];
    GtkWidget *files[2], *clear[2];
    GtkWidget *preview_level;
    GtkWidget *preview_output, *preview_gain, *overlap, *selection_label, *crop_button;
    GtkWidget *autodj_fields[2];
    GtkWidget *pair_labels[2], *preview_play, *preview_stop;
    JsonNode *pair_bindings[2], *pair_details[2];
    guint pair_generation[2];
    Snapshot *current;
    GPtrArray *undo, *redo;
    JsonNode *media_detail, *marker_retry;
    gboolean media_busy, embedded;
    guint audio_timer;
    gboolean updating, dragging, dirty;
    gint drag_marker;
    gdouble cursor, zoom;
    gdouble selection[2], selection_anchor, press_x;
    gboolean selection_set[2], selecting;
    /* Owner-provided beat grid (deck inspector): drawn on the timeline and,
     * with snap on, used to round AutoDJ IN/OUT placement to bar starts. */
    RadioSRhythmAnalysis grid;
    gboolean grid_valid, snap;
    RadioSMarkerEditorSaved saved_hook;
    gpointer saved_user;
} Editor;
static JsonNode *editor_json_copy(JsonNode *node);
static void transition_bindings_restore(Editor *editor, const Snapshot *snapshot);
static void transition_preview_action(GtkButton *widget, gpointer value);

static const gchar *const marker_names[] = {
    "qwf-duration", "qwf-cue-in", "qwf-intro", "qwf-out",
    "qwf-fade-start", "qwf-fade-end", "qwf-auto-next", "qwf-overlap"
};
static const gchar *const marker_labels[] = {
    "Parça süresi · saniye", "Cue giriş", "Vokal / intro", "Cue çıkış",
    "Fade başlangıcı", "Fade bitişi", "Otomatik geçiş noktası", "Sonraki parçayla bindirme"
};
static const gchar *const transition_names[] = {
    "transition-duration", "transition-cue-in", "transition-intro", "transition-out",
    "transition-b-in", "transition-fade-in", "transition-overlap", "transition-ducking"
};
static const gchar *const transition_labels[] = {
    "A toplam süre", "A cue başlangıcı", "A vokal girişi", "A çıkış noktası",
    "B cue başlangıcı", "B giriş fade’i", "Bindirme", "B sırasında A kısma · dB"
};

static void
pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{
    gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0);
}

static void
snapshot_free(gpointer data)
{
    Snapshot *snapshot = data;
    if (snapshot == NULL)
        return;
    for (guint i = 0; i < FIELD_COUNT; ++i)
        g_free(snapshot->values[i]);
    for (guint i = 0; i < 2; ++i) {
        g_free(snapshot->autodj[i]);
        g_free(snapshot->files[i]);
        g_clear_pointer(&snapshot->bindings[i], json_node_free);
    }
    g_free(snapshot);
}

static Snapshot *
snapshot_copy(const Snapshot *source)
{
    Snapshot *copy = g_new0(Snapshot, 1);
    for (guint i = 0; i < FIELD_COUNT; ++i)
        copy->values[i] = g_strdup(source->values[i]);
    for (guint i = 0; i < 2; ++i) {
        copy->autodj[i] = g_strdup(source->autodj[i]);
        copy->files[i] = g_strdup(source->files[i]);
        copy->bindings[i] = source->bindings[i] ? json_node_copy(source->bindings[i]) : NULL;
    }
    copy->preview_level = source->preview_level;
    copy->flag = source->flag;
    return copy;
}

static Snapshot *
snapshot_read(const Editor *editor)
{
    Snapshot *snapshot = g_new0(Snapshot, 1);
    for (guint i = 0; i < FIELD_COUNT; ++i)
        snapshot->values[i] = g_strdup(gtk_entry_get_text(GTK_ENTRY(editor->fields[i])));
    snapshot->flag = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->flag));
    for (guint i = 0; i < 2; ++i)
        snapshot->autodj[i] = editor->autodj_fields[i] ? g_strdup(gtk_entry_get_text(GTK_ENTRY(editor->autodj_fields[i]))) : NULL;
    if (editor->kind == RADIOS_EDITOR_TRANSITION) {
        for (guint i = 0; i < 2; ++i) {
            snapshot->files[i] = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(editor->files[i]));
            snapshot->bindings[i] = editor->pair_bindings[i] ? json_node_copy(editor->pair_bindings[i]) : NULL;
        }
        snapshot->preview_level = gtk_range_get_value(GTK_RANGE(editor->preview_level));
    }
    return snapshot;
}

static void
store_free(gpointer data)
{
    EditorStore *store = data;
    if (store->dialog != NULL)
        gtk_widget_destroy(store->dialog);
    snapshot_free(store->saved);
    g_free(store);
}

static void
editor_free(gpointer data)
{
    Editor *editor = data;
    snapshot_free(editor->current);
    g_ptr_array_unref(editor->undo);
    g_ptr_array_unref(editor->redo);
    g_clear_pointer(&editor->media_detail, json_node_free);
    g_clear_pointer(&editor->marker_retry, json_node_free);
    for (guint i = 0; i < 2; ++i) { g_clear_pointer(&editor->pair_bindings[i], json_node_free); g_clear_pointer(&editor->pair_details[i], json_node_free); }
    if (editor->embedded) { snapshot_free(editor->store->saved); g_free(editor->store); }
    g_free(editor);
}

static void
set_status(Editor *editor, const gchar *message, gboolean error)
{
    gtk_label_set_text(GTK_LABEL(editor->status), message);
    GtkStyleContext *style = gtk_widget_get_style_context(editor->status);
    if (error)
        gtk_style_context_add_class(style, "error");
    else
        gtk_style_context_remove_class(style, "error");
}

static gboolean
read_number(GtkWidget *entry, gdouble *number)
{
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(entry));
    gchar *end = NULL;
    if (*text == '\0')
        return FALSE;
    *number = g_ascii_strtod(text, &end);
    return end != text && *end == '\0' && isfinite(*number);
}

static gboolean
duration_read(Editor *editor, gdouble *duration)
{
    return read_number(editor->fields[0], duration) && *duration >= (editor->media_detail ? 0.001 : 0.1) && *duration <= 86400;
}

static void
history_push(GPtrArray *history, Snapshot *snapshot)
{
    if (history->len == HISTORY_LIMIT)
        g_ptr_array_remove_index(history, 0);
    g_ptr_array_add(history, snapshot);
}

static void
refresh(Editor *editor)
{
    if (editor->kind == RADIOS_EDITOR_TRANSITION) {
        gboolean was_updating = editor->updating;
        editor->updating = TRUE;
        for (guint i = 0; i < 2; ++i) {
            gdouble value;
            if (editor->mix[i] != NULL && read_number(editor->fields[6 + i], &value))
                gtk_range_set_value(GTK_RANGE(editor->mix[i]), value);
        }
        editor->updating = was_updating;
    }
    if (editor->overlap) {
        gdouble in = 0, out = 0, overlap = 0;
        read_number(editor->fields[1], &in); read_number(editor->fields[3], &out);
        read_number(editor->fields[7], &overlap);
        gboolean was_updating = editor->updating; editor->updating = TRUE;
        gtk_spin_button_set_range(GTK_SPIN_BUTTON(editor->overlap), 0, MIN(30, MAX(0, out - in)));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->overlap), overlap);
        editor->updating = was_updating;
    }
    if (editor->selection_label) {
        gboolean selected = editor->selection_set[0] && editor->selection_set[1] && fabs(editor->selection[1] - editor->selection[0]) >= .001;
        gchar *text = selected ? g_strdup_printf("Seçim %.3f → %.3f sn · %.3f sn", MIN(editor->selection[0], editor->selection[1]),
            MAX(editor->selection[0], editor->selection[1]), fabs(editor->selection[1] - editor->selection[0])) : g_strdup("İki nokta seç");
        gtk_label_set_text(GTK_LABEL(editor->selection_label), text); g_free(text);
        gtk_widget_set_sensitive(editor->crop_button, selected);
    }
    if (editor->undo_button != NULL) {
        gtk_widget_set_sensitive(editor->undo_button, editor->undo->len != 0);
        gtk_widget_set_sensitive(editor->redo_button, editor->redo->len != 0);
    }
    if (editor->canvas != NULL)
        gtk_widget_queue_draw(editor->canvas);
    if (editor->position != NULL) {
        gchar *text = g_strdup_printf("Konum %.1f sn · %.1f×", editor->cursor, editor->zoom);
        gtk_label_set_text(GTK_LABEL(editor->position), text);
        g_free(text);
    }
}

static void
field_changed(GtkWidget *widget, gpointer data)
{
    Editor *editor = data;
    (void)widget;
    if (editor->updating)
        return;
    editor->dirty = TRUE;
    if (editor->kind == RADIOS_EDITOR_TRANSITION) radios_audio_ui_preview_stop(editor->ctx, editor->audio_owner);
    radios_data_document_dirty(editor->store->document, TRUE);
    history_push(editor->undo, editor->current);
    editor->current = snapshot_read(editor);
    g_ptr_array_set_size(editor->redo, 0);
    gdouble duration;
    if (duration_read(editor, &duration))
        editor->cursor = MIN(editor->cursor, duration);
    set_status(editor, editor->embedded ? "Değişiklikleri kaydet." : "Değişiklikler uygulanmadı; yalnız bu oturumun taslağıdır.", FALSE);
    refresh(editor);
}

static void
snapshot_apply(Editor *editor, Snapshot *snapshot)
{
    editor->updating = TRUE;
    for (guint i = 0; i < FIELD_COUNT; ++i)
        gtk_entry_set_text(GTK_ENTRY(editor->fields[i]), snapshot->values[i]);
    for (guint i = 0; i < 2; ++i)
        if (editor->autodj_fields[i]) gtk_entry_set_text(GTK_ENTRY(editor->autodj_fields[i]), snapshot->autodj[i] ? snapshot->autodj[i] : "");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->flag), snapshot->flag);
    if (editor->kind == RADIOS_EDITOR_TRANSITION) {
        for (guint i = 0; i < 2; ++i) {
            /* A saved draft stores "" for "no file": selecting "" would select
             * the process working directory (for a Start-menu launch the
             * program's bin folder) instead of clearing the button. */
            if (snapshot->files[i] != NULL && *snapshot->files[i] != '\0')
                gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(editor->files[i]), snapshot->files[i]);
            else
                gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(editor->files[i]));
            gtk_widget_set_sensitive(editor->clear[i], snapshot->bindings[i] != NULL || (snapshot->files[i] && *snapshot->files[i]));
        }
        gtk_range_set_value(GTK_RANGE(editor->preview_level), snapshot->preview_level);
        if (editor->ctx->audio) gtk_editable_set_editable(GTK_EDITABLE(editor->fields[0]), snapshot->bindings[0] == NULL);
        transition_bindings_restore(editor, snapshot);
    }
    editor->updating = FALSE;
    snapshot_free(editor->current);
    editor->current = snapshot;
    gdouble duration;
    if (duration_read(editor, &duration))
        editor->cursor = MIN(editor->cursor, duration);
    refresh(editor);
}

static void
history_clicked(GtkButton *button, gpointer data)
{
    Editor *editor = data;
    gboolean undo = GTK_WIDGET(button) == editor->undo_button;
    GPtrArray *from = undo ? editor->undo : editor->redo;
    GPtrArray *to = undo ? editor->redo : editor->undo;
    if (from->len == 0)
        return;
    history_push(to, snapshot_copy(editor->current));
    snapshot_apply(editor, g_ptr_array_steal_index(from, from->len - 1));
    editor->dirty = TRUE; radios_data_document_dirty(editor->store->document, TRUE);
    set_status(editor, undo ? "Düzenleme geri alındı." : "Düzenleme yinelendi.", FALSE);
}

static void
reset_clicked(GtkButton *button, gpointer data)
{
    Editor *editor = data;
    (void)button;
    Snapshot *reset = snapshot_copy(editor->current);
    for (guint i = 1; i < FIELD_COUNT; ++i) {
        g_free(reset->values[i]);
        reset->values[i] = g_strdup(i == 1 || i == 7 ? "0" : i == 3 ? reset->values[0] : "");
    }
    reset->flag = FALSE;
    for (guint i = 0; i < 2; ++i) { g_free(reset->autodj[i]); reset->autodj[i] = NULL; editor->selection_set[i] = FALSE; }
    history_push(editor->undo, snapshot_copy(editor->current));
    g_ptr_array_set_size(editor->redo, 0);
    snapshot_apply(editor, reset);
    editor->dirty = TRUE; radios_data_document_dirty(editor->store->document, TRUE);
    set_status(editor, "İşaretler sıfırlandı; Geri al ile geri getirilebilir.", FALSE);
}

static void
visible_range(Editor *editor, gdouble duration, gdouble *start, gdouble *span)
{
    *span = duration / editor->zoom;
    *start = CLAMP(editor->cursor - *span / 2, 0, duration - *span);
}

static void
draw_text(GtkWidget *widget, cairo_t *cr, const gchar *text, gdouble x, gdouble y)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, text);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

static GtkWidget *marker_field(Editor *editor, guint index)
{
    return index < FIELD_COUNT ? editor->fields[index] : index < FIELD_COUNT + 2 ? editor->autodj_fields[index - FIELD_COUNT] : NULL;
}

static gdouble timeline_time(Editor *editor, gdouble x, gdouble duration)
{
    gdouble start, span; visible_range(editor, duration, &start, &span);
    gdouble width = MAX(1, gtk_widget_get_allocated_width(editor->canvas) - 32);
    return round(CLAMP(start + (x - 16) / width * span, 0, duration) * 1000) / 1000;
}

static void shade_range(cairo_t *cr, gdouble from, gdouble to, gdouble start, gdouble span,
    gdouble usable, gdouble height, gdouble r, gdouble g, gdouble b, gdouble alpha)
{
    gdouble left = CLAMP((from - start) / span, 0, 1), right = CLAMP((to - start) / span, 0, 1);
    if (right <= left) return;
    cairo_set_source_rgba(cr, r, g, b, alpha);
    cairo_rectangle(cr, 16 + usable * left, 0, usable * (right - left), height - 24); cairo_fill(cr);
}

static gboolean
timeline_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    Editor *editor = data;
    gdouble width = gtk_widget_get_allocated_width(widget);
    gdouble height = gtk_widget_get_allocated_height(widget);
    gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0, width, height);
    cairo_set_source_rgb(cr, 0.055, 0.09, 0.12);
    cairo_paint(cr);
    gdouble duration;
    if (!duration_read(editor, &duration))
        return FALSE;
    gdouble start, span;
    visible_range(editor, duration, &start, &span);
    gdouble usable = MAX(1, width - 32);
    gdouble cue_in = 0, cue_out = duration, overlap = 0, next = duration;
    read_number(editor->fields[1], &cue_in); read_number(editor->fields[3], &cue_out);
    read_number(editor->fields[7], &overlap);
    next = MAX(cue_in, cue_out - overlap);
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->flag))) read_number(editor->fields[6], &next);
    if (editor->autodj_fields[1]) read_number(editor->autodj_fields[1], &next);
    shade_range(cr, 0, cue_in, start, span, usable, height, .01, .01, .01, .65);
    shade_range(cr, cue_out, duration, start, span, usable, height, .01, .01, .01, .65);
    if (overlap > 0) {
        shade_range(cr, next, MIN(cue_out, next + overlap), start, span, usable, height, 1, .62, .17, .27);
        gdouble x = 16 + usable * (next - start) / span;
        if (next >= start && next <= start + span) {
            gchar *amount = g_strdup_printf("↔ %.2f sn", overlap);
            cairo_set_source_rgb(cr, 1, .75, .38); draw_text(widget, cr, amount, CLAMP(x + 5, 3, width - 90), height - 42); g_free(amount);
        }
    }
    if (editor->selection_set[0] && editor->selection_set[1])
        shade_range(cr, MIN(editor->selection[0], editor->selection[1]), MAX(editor->selection[0], editor->selection[1]),
            start, span, usable, height, .27, .63, 1, .25);
    JsonObject *analysis = editor->media_detail ? json_object_get_object_member(json_node_get_object(editor->media_detail), "analysis") : NULL;
    JsonArray *waveform = analysis ? json_object_get_array_member(analysis, "waveform") : NULL;
    gint64 hop = radios_json_int(analysis, "waveform_frames_per_bin", 0), rate = radios_json_int(analysis, "sample_rate", 0);
    if (waveform && hop > 0 && rate > 0) {
        guint bins = MIN(json_array_get_length(waveform) / 2, 2048u);
        for (guint i = 0; i < bins; ++i) {
            gdouble time = (gdouble)i * hop / rate; if (time < start || time > start + span) continue;
            gdouble low = json_array_get_double_element(waveform, 2 * i), high = json_array_get_double_element(waveform, 2 * i + 1);
            gboolean clipped = low <= -1 || high >= 1;
            cairo_set_source_rgb(cr, clipped ? 0.92 : 0.27, clipped ? 0.47 : 0.69, clipped ? 0.35 : 0.67);
            gdouble x = 16 + usable * (time - start) / span, center = (height - 24) / 2, scale = MAX(1, center - 16);
            cairo_move_to(cr, x, center - CLAMP(high, -1, 1) * scale); cairo_line_to(cr, x, center - CLAMP(low, -1, 1) * scale); cairo_stroke(cr);
        }
    } else {
        cairo_set_source_rgb(cr, 0.55, 0.64, 0.69);
        draw_text(widget, cr, "Dalga biçimi verisi bekleniyor", 18, height / 2 - 10);
    }
    if (editor->grid_valid && editor->grid.bpm > 0) {
        /* Beat lines once they are at least 12 px apart, otherwise bar lines
         * (every four beats from the downbeat), thinned to stay readable. */
        gdouble beat = 60 / editor->grid.bpm, pixels = usable * beat / span;
        gboolean beats = pixels >= 12;
        gdouble period = beats ? beat : beat * 4 * MAX(1, ceil(12 / MAX(.001, pixels * 4)));
        gdouble downbeat = editor->grid.downbeat_ms / 1000;
        gdouble anchor = (beats || !editor->grid.has_downbeat ? editor->grid.first_beat_ms : editor->grid.downbeat_ms) / 1000;
        gint64 first = (gint64)ceil((MAX(0, start) - anchor) / period), last = (gint64)floor((MIN(start + span, duration) - anchor) / period);
        cairo_set_line_width(cr, 1);
        for (gint64 index = first; index <= last; ++index) {
            gdouble time = anchor + index * period, x = 16 + usable * (time - start) / span;
            gboolean bar = !beats || (editor->grid.has_downbeat && fabs(remainder((time - downbeat) / beat, 4)) < .01);
            cairo_set_source_rgba(cr, .55, .72, .77, bar ? .4 : .16);
            cairo_move_to(cr, floor(x) + .5, 12); cairo_line_to(cr, floor(x) + .5, height - 24); cairo_stroke(cr);
        }
    }
    for (guint i = 0; i < 5; ++i) {
        gdouble x = 16 + usable * i / 4;
        cairo_set_source_rgb(cr, 0.18, 0.24, 0.28);
        cairo_move_to(cr, x, 12);
        cairo_line_to(cr, x, height - 24);
        cairo_stroke(cr);
        gchar *tick = g_strdup_printf("%.1f", start + span * i / 4);
        cairo_set_source_rgb(cr, 0.62, 0.68, 0.72);
        draw_text(widget, cr, tick, MAX(0, x - (i == 4 ? 36 : 0)), height - 21);
        g_free(tick);
    }
    static const gchar *const tags[] = { "IN", "VO", "OUT", "F1", "F2", "GEÇİŞ", "AutoDJ IN", "AutoDJ OUT" };
    static const guint indices[] = {1, 2, 3, 4, 5, 6, 8, 9};
    for (guint i = 0; i < G_N_ELEMENTS(indices); ++i) {
        gdouble value;
        GtkWidget *field = marker_field(editor, indices[i]);
        if (!field || !read_number(field, &value) || value < start || value > start + span)
            continue;
        gdouble x = 16 + usable * (value - start) / span;
        cairo_set_source_rgb(cr, i == 2 || i == 7 ? .98 : .39, i == 2 || i == 7 ? .55 : .82, i >= 6 ? .94 : .53);
        cairo_move_to(cr, x, 12);
        cairo_line_to(cr, x, height - 24);
        cairo_stroke(cr);
        gdouble tag_y = 8 + i * 21;
        cairo_rectangle(cr, CLAMP(x - 4, 0, width - 8), tag_y + 3, 8, 11); cairo_fill(cr);
        draw_text(widget, cr, tags[i], CLAMP(x + 7, 3, width - (i >= 6 ? 90 : 50)), tag_y);
    }
    for (guint i = 0; i < 2; ++i) {
        if (!editor->selection_set[i] || editor->selection[i] < start || editor->selection[i] > start + span) continue;
        gdouble x = 16 + usable * (editor->selection[i] - start) / span;
        cairo_set_source_rgb(cr, .53, .78, 1); cairo_set_line_width(cr, 2);
        cairo_move_to(cr, x, height - 59); cairo_line_to(cr, x, height - 24); cairo_stroke(cr);
        draw_text(widget, cr, i == 0 ? "[" : "]", CLAMP(x - 3, 0, width - 12), height - 62);
    }
    gdouble cursor = 16 + usable * (editor->cursor - start) / span;
    cairo_set_source_rgb(cr, 0.92, 0.94, 0.96);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, cursor, 0);
    cairo_line_to(cr, cursor, height - 24);
    cairo_stroke(cr);
    return FALSE;
}

static gboolean
set_position(Editor *editor, gdouble x)
{
    gdouble duration;
    if (!duration_read(editor, &duration)) {
        set_status(editor, "Zaman çizgisi için önce parça süresini saniye olarak gir.", TRUE);
        return FALSE;
    }
    editor->cursor = timeline_time(editor, x, duration);
    if (editor->drag_marker >= 1) {
        GtkWidget *field = marker_field(editor, (guint)editor->drag_marker);
        gdouble in = 0, out = duration;
        read_number(editor->fields[1], &in); read_number(editor->fields[3], &out);
        if (editor->drag_marker == 1) editor->cursor = CLAMP(editor->cursor, 0, MAX(0, out - .001));
        if (editor->drag_marker == 3) editor->cursor = CLAMP(editor->cursor, MIN(duration, in + .001), duration);
        if (editor->drag_marker >= 8) {
            gdouble other;
            if (editor->drag_marker == 8) { if (read_number(editor->autodj_fields[1], &other)) out = other; out -= .001; }
            if (editor->drag_marker == 9) { if (read_number(editor->autodj_fields[0], &other)) in = other; in += .001; }
        }
        if (editor->drag_marker >= 4) editor->cursor = CLAMP(editor->cursor, in, MAX(in, out));
        gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_formatd(buffer, sizeof(buffer), "%.3f", editor->cursor);
        gboolean updating = editor->updating; editor->updating = TRUE;
        gtk_entry_set_text(GTK_ENTRY(field), buffer);
        if (editor->embedded && (editor->drag_marker == 4 || editor->drag_marker == 5)) {
            guint companion = editor->drag_marker == 4 ? 5 : 4; gdouble other = editor->cursor;
            if (!read_number(editor->fields[companion], &other) || (editor->drag_marker == 4 ? other < editor->cursor : other > editor->cursor))
                gtk_entry_set_text(GTK_ENTRY(editor->fields[companion]), buffer);
        }
        if (editor->embedded && (editor->drag_marker == 1 || editor->drag_marker == 3)) {
            read_number(editor->fields[1], &in); read_number(editor->fields[3], &out);
            for (guint i = 2; i <= 7; ++i) {
                gdouble number;
                if (i == 3 || !read_number(editor->fields[i], &number) || (i == 2 && number == 0)) continue;
                number = i == 7 ? MIN(number, out - in) : CLAMP(number, in, out);
                g_ascii_formatd(buffer, sizeof(buffer), "%.3f", number); gtk_entry_set_text(GTK_ENTRY(editor->fields[i]), buffer);
            }
            gdouble auto_in = in, auto_out = out;
            for (guint i = 0; i < 2; ++i) {
                gdouble number;
                if (!read_number(editor->autodj_fields[i], &number)) continue;
                number = CLAMP(number, in, out); if (i) auto_out = number; else auto_in = number;
                g_ascii_formatd(buffer, sizeof(buffer), "%.3f", number); gtk_entry_set_text(GTK_ENTRY(editor->autodj_fields[i]), buffer);
            }
            if (auto_in >= auto_out) for (guint i = 0; i < 2; ++i) gtk_entry_set_text(GTK_ENTRY(editor->autodj_fields[i]), "");
        }
        editor->updating = updating;
    } else if (editor->drag_marker == -2 || editor->drag_marker == -3) {
        guint index = editor->drag_marker == -2 ? 0 : 1;
        editor->selection[index] = editor->cursor; editor->selection_set[index] = TRUE;
    } else if (editor->selecting) {
        editor->selection[0] = editor->selection_anchor; editor->selection[1] = editor->cursor;
        editor->selection_set[0] = editor->selection_set[1] = TRUE;
    }
    refresh(editor);
    return TRUE;
}

static gboolean
timeline_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    Editor *editor = data;
    if (event->button != 1)
        return FALSE;
    gtk_widget_grab_focus(widget);
    editor->drag_marker = -1; editor->selecting = FALSE; editor->press_x = event->x;
    gdouble duration;
    if (duration_read(editor, &duration)) {
        gdouble start, span;
        visible_range(editor, duration, &start, &span);
        gdouble width = MAX(1, gtk_widget_get_allocated_width(widget) - 32);
        static const guint indices[] = {1, 2, 3, 4, 5, 6, 8, 9};
        for (guint i = 0; i < G_N_ELEMENTS(indices); ++i) {
            gdouble marker;
            GtkWidget *field = marker_field(editor, indices[i]);
            if (field && read_number(field, &marker) &&
                fabs(event->x - (16 + (marker - start) / span * width)) < 9 &&
                fabs(event->y - (17 + i * 21)) < 10)
                editor->drag_marker = (gint)indices[i];
        }
        for (guint i = 0; i < 2; ++i)
            if (editor->selection_set[i] && fabs(event->x - (16 + (editor->selection[i] - start) / span * width)) < 9 &&
                event->y > gtk_widget_get_allocated_height(widget) - 64)
                editor->drag_marker = i == 0 ? -2 : -3;
        if (event->state & GDK_SHIFT_MASK) {
            editor->drag_marker = -3;
            if (!editor->selection_set[0]) { editor->selection[0] = editor->cursor; editor->selection_set[0] = TRUE; }
        }
        editor->selection_anchor = timeline_time(editor, event->x, duration);
    }
    editor->dragging = set_position(editor, event->x);
    return TRUE;
}

static gboolean
timeline_motion(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    Editor *editor = data;
    (void)widget;
    if (editor->dragging && (event->state & GDK_BUTTON1_MASK)) {
        if (editor->drag_marker == -1 && fabs(event->x - editor->press_x) > 4) editor->selecting = TRUE;
        set_position(editor, event->x);
    }
    return TRUE;
}

static gboolean
timeline_release(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    Editor *editor = data;
    (void)widget;
    (void)event;
    if (editor->dragging && editor->drag_marker >= 1) field_changed(marker_field(editor, (guint)editor->drag_marker), editor);
    if (editor->dragging && editor->drag_marker == -1 && !editor->selecting && editor->media_detail) {
        GError *error = NULL;
        radios_audio_ui_preview_seek(editor->ctx, editor->audio_owner, json_node_get_object(editor->media_detail), (guint64)llround(editor->cursor * 1000), &error);
        if (error) { set_status(editor, error->message, TRUE); g_clear_error(&error); }
    }
    editor->dragging = FALSE;
    editor->drag_marker = -1;
    return TRUE;
}

static gboolean
timeline_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    Editor *editor = data;
    (void)widget;
    gdouble duration;
    if (!duration_read(editor, &duration))
        return FALSE;
    gdouble step = (event->state & GDK_SHIFT_MASK) ? 5 : 0.1;
    switch (event->keyval) {
    case GDK_KEY_Left: editor->cursor = MAX(0, editor->cursor - step); break;
    case GDK_KEY_Right: editor->cursor = MIN(duration, editor->cursor + step); break;
    case GDK_KEY_Home: editor->cursor = 0; break;
    case GDK_KEY_End: editor->cursor = duration; break;
    default: return FALSE;
    }
    if (editor->media_detail) {
        GError *error = NULL;
        radios_audio_ui_preview_seek(editor->ctx, editor->audio_owner, json_node_get_object(editor->media_detail), (guint64)llround(editor->cursor * 1000), &error);
        if (error) { set_status(editor, error->message, TRUE); g_clear_error(&error); }
    }
    refresh(editor);
    return TRUE;
}

static void
marker_clicked(GtkButton *button, gpointer data)
{
    Editor *editor = data;
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "marker-index"));
    gdouble duration;
    if (!duration_read(editor, &duration)) {
        set_status(editor, "İşaret koymak için önce parça süresini gir.", TRUE);
        return;
    }
    gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
    gdouble point = editor->cursor;
    if (editor->embedded && index >= 4 && index <= 6) {
        gdouble in = 0, out = duration; read_number(editor->fields[1], &in); read_number(editor->fields[3], &out);
        point = CLAMP(point, in, out);
    }
    if (index >= FIELD_COUNT) {
        gdouble in = 0, out = duration, other;
        read_number(editor->fields[1], &in); read_number(editor->fields[3], &out);
        if (index == 8 && read_number(editor->autodj_fields[1], &other)) out = other - .001;
        if (index == 9 && read_number(editor->autodj_fields[0], &other)) in = other + .001;
        if (point < in || point > out || (index == 8 && point >= out) || (index == 9 && point <= in)) {
            set_status(editor, "AutoDJ IN, OUT'tan önce ve kırpılan aralık içinde olmalı.", TRUE); return;
        }
        if (editor->snap && editor->grid_valid && editor->grid.bpm > 0) {
            /* Round to the nearest bar start that keeps the point inside the legal interval. */
            gdouble period = 240 / editor->grid.bpm, anchor = (editor->grid.has_downbeat ? editor->grid.downbeat_ms : editor->grid.first_beat_ms) / 1000;
            gdouble low = index == 8 ? in : in + .001, high = index == 8 ? out - .001 : out;
            gdouble first = ceil((low - anchor) / period - 1e-9), last = floor((high - anchor) / period + 1e-9);
            if (first > last) { set_status(editor, "IN/OUT aralığında dörtlük başı yok; hizalamayı kapatın.", TRUE); return; }
            point = round((anchor + CLAMP(round((point - anchor) / period), first, last) * period) * 1000) / 1000;
        }
    }
    g_ascii_formatd(buffer, sizeof(buffer), "%.3f", point);
    gboolean was_updating = editor->updating;
    if (editor->embedded) editor->updating = TRUE;
    gtk_entry_set_text(GTK_ENTRY(marker_field(editor, index)), buffer);
    if (editor->embedded && (index == 4 || index == 5)) {
        gdouble other = point;
        guint companion = index == 4 ? 5 : 4;
        if (!read_number(editor->fields[companion], &other) || (index == 4 ? other < point : other > point))
            gtk_entry_set_text(GTK_ENTRY(editor->fields[companion]), buffer);
    }
    if (editor->embedded && index == 6) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->flag), TRUE);
    if (editor->embedded) { editor->updating = was_updating; field_changed(marker_field(editor, index), editor); }
}

static void selection_clicked(GtkButton *button, gpointer data)
{
    Editor *editor = data;
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "selection-index"));
    editor->selection[index] = editor->cursor; editor->selection_set[index] = TRUE; refresh(editor);
}

static void snapshot_number(Snapshot *snapshot, guint index, gdouble value)
{
    gchar buffer[G_ASCII_DTOSTR_BUF_SIZE]; g_ascii_formatd(buffer, sizeof(buffer), "%.3f", value);
    g_free(snapshot->values[index]); snapshot->values[index] = g_strdup(buffer);
}

static void crop_clicked(GtkButton *button, gpointer data)
{
    (void)button; Editor *editor = data;
    if (!editor->selection_set[0] || !editor->selection_set[1]) return;
    gdouble in = MIN(editor->selection[0], editor->selection[1]), out = MAX(editor->selection[0], editor->selection[1]);
    if (out - in < .001) { set_status(editor, "Kırpmak için iki ayrı nokta seç.", TRUE); return; }
    Snapshot *crop = snapshot_read(editor);
    snapshot_number(crop, 1, in); snapshot_number(crop, 3, out);
    for (guint i = 2; i <= 6; ++i) {
        if (i == 3) continue;
        gdouble number = 0;
        if (read_number(editor->fields[i], &number) && !(i == 2 && number == 0)) snapshot_number(crop, i, CLAMP(number, in, out));
    }
    gdouble overlap = 0; read_number(editor->fields[7], &overlap); snapshot_number(crop, 7, MIN(overlap, out - in));
    gdouble auto_in = in, auto_out = out;
    for (guint i = 0; i < 2; ++i) {
        gdouble number;
        if (editor->autodj_fields[i] && read_number(editor->autodj_fields[i], &number)) {
            number = CLAMP(number, in, out); if (i) auto_out = number; else auto_in = number;
            gchar buffer[G_ASCII_DTOSTR_BUF_SIZE]; g_ascii_formatd(buffer, sizeof(buffer), "%.3f", number);
            g_free(crop->autodj[i]); crop->autodj[i] = g_strdup(buffer);
        }
    }
    if (auto_in >= auto_out) for (guint i = 0; i < 2; ++i) { g_free(crop->autodj[i]); crop->autodj[i] = NULL; }
    history_push(editor->undo, snapshot_copy(editor->current)); g_ptr_array_set_size(editor->redo, 0);
    snapshot_apply(editor, crop); editor->dirty = TRUE; radios_data_document_dirty(editor->store->document, TRUE);
    editor->cursor = in; refresh(editor);
    radios_audio_ui_preview_stop(editor->ctx, editor->audio_owner);
    set_status(editor, "Seçilen aralık çalınacak. Kaynak dosya korunuyor; değişikliği kaydet.", FALSE);
}

static void overlap_changed(GtkSpinButton *spin, gpointer data)
{
    Editor *editor = data; if (editor->updating) return;
    gdouble old_overlap = 0; read_number(editor->fields[7], &old_overlap);
    editor->updating = TRUE;
    gchar buffer[G_ASCII_DTOSTR_BUF_SIZE]; g_ascii_formatd(buffer, sizeof(buffer), "%.3f", gtk_spin_button_get_value(spin));
    gtk_entry_set_text(GTK_ENTRY(editor->fields[7]), buffer);
    gdouble next = 0, out = 0, auto_out = 0;
    read_number(editor->fields[3], &out);
    if (read_number(editor->fields[6], &next) && (fabs(next - out) < .001 || fabs(next - (out - old_overlap)) < .001) &&
        (!editor->autodj_fields[1] || !read_number(editor->autodj_fields[1], &auto_out))) {
        g_ascii_formatd(buffer, sizeof(buffer), "%.3f", out - gtk_spin_button_get_value(spin));
        gtk_entry_set_text(GTK_ENTRY(editor->fields[6]), buffer);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->flag), TRUE);
    editor->updating = FALSE; field_changed(GTK_WIDGET(spin), editor);
}

static void preview_output_changed(GObject *control, gpointer data)
{
    (void)control; Editor *editor = data; GError *error = NULL;
    if (editor->updating) return;
    GtkAdjustment *gain = g_object_get_data(G_OBJECT(editor->preview_gain), "adjustment");
    RadioSAudioUiBus bus = gtk_combo_box_get_active(GTK_COMBO_BOX(editor->preview_output)) == 1 ? RADIOS_AUDIO_UI_MONITOR : RADIOS_AUDIO_UI_CUE;
    if (!radios_audio_ui_preview_set_output(editor->ctx, editor->audio_owner, bus, gtk_adjustment_get_value(gain), &error) && error) {
        RadioSAudioUiBus actual_bus = RADIOS_AUDIO_UI_CUE; gdouble actual_gain = 0;
        if (radios_audio_ui_preview_get_output(editor->ctx, editor->audio_owner, &actual_bus, &actual_gain)) {
            editor->updating = TRUE;
            gtk_combo_box_set_active(GTK_COMBO_BOX(editor->preview_output), actual_bus == RADIOS_AUDIO_UI_MONITOR ? 1 : 0);
            gtk_adjustment_set_value(gain, actual_gain); editor->updating = FALSE;
        }
        set_status(editor, error->message, TRUE); g_clear_error(&error);
    }
}

static void
mix_changed(GtkRange *range, gpointer data)
{
    Editor *editor = data;
    if (editor->updating)
        return;
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(range), "field-index"));
    gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(buffer, sizeof(buffer), "%.1f", gtk_range_get_value(range));
    gtk_entry_set_text(GTK_ENTRY(editor->fields[index]), buffer);
}

static void
zoom_clicked(GtkButton *button, gpointer data)
{
    Editor *editor = data;
    gint direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "zoom-direction"));
    editor->zoom = direction == 0 ? 1 : CLAMP(editor->zoom * (direction > 0 ? 2 : 0.5), 1, 16);
    refresh(editor);
}

static gboolean
validate(Editor *editor)
{
    gdouble values[FIELD_COUNT] = { 0 };
    gboolean present[FIELD_COUNT] = { FALSE };
    for (guint i = 0; i < FIELD_COUNT; ++i) {
        const gchar *text = gtk_entry_get_text(GTK_ENTRY(editor->fields[i]));
        present[i] = *text != '\0';
        gboolean required = editor->kind == RADIOS_EDITOR_MARKERS ? (i == 0 || i == 1 || i == 3 || i == 7) : i != 2;
        gdouble minimum = (i == 0) ? (editor->media_detail ? 0.001 : 0.1) : editor->kind == RADIOS_EDITOR_TRANSITION && i == 7 ? -30 : 0;
        gdouble maximum = editor->kind == RADIOS_EDITOR_TRANSITION && (i == 5 || i == 6) ? 30 : editor->kind == RADIOS_EDITOR_TRANSITION && i == 7 ? 0 : 86400;
        if ((!present[i] && required) || (present[i] && (!read_number(editor->fields[i], &values[i]) || values[i] < minimum || values[i] > maximum))) {
            const gchar *label = editor->kind == RADIOS_EDITOR_MARKERS ? marker_labels[i] : transition_labels[i];
            gchar *message = g_strdup_printf("%s: %.1f–%.1f aralığında bir sayı gir.", label, minimum, maximum);
            set_status(editor, message, TRUE);
            g_free(message);
            gtk_widget_grab_focus(editor->fields[i]);
            return FALSE;
        }
    }
    const gchar *message = NULL;
    if (values[3] <= values[1] || values[3] > values[0])
        message = "Cue çıkış, cue girişten sonra ve toplam süre içinde olmalı.";
    if (present[2] && !(editor->media_detail && values[2] == 0) && (values[2] < values[1] || values[2] > values[3]))
        message = "Vokal girişi cue giriş ile cue çıkış arasında olmalı.";
    if (editor->kind == RADIOS_EDITOR_MARKERS) {
        for (guint i = 4; i <= 6; ++i)
            if (present[i] && (values[i] < values[1] || values[i] > values[3]))
                message = "Fade ve otomatik geçiş işaretleri cue aralığında olmalı.";
        if (present[4] != present[5] || (present[4] && values[5] < values[4]))
            message = "Fade başlangıcı ve bitişini birlikte ve doğru sırada gir.";
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->flag)) && !present[6])
            message = "Otomatik geçiş için geçiş noktası gerekli.";
        if (values[7] > values[3] - values[1])
            message = "Bindirme cue aralığından uzun olamaz.";
        if (editor->media_detail && values[7] > 30)
            message = "Medya bindirmesi en fazla 30 saniye olabilir.";
        gdouble auto_in = values[1], auto_out = values[3];
        for (guint i = 0; i < 2; ++i) {
            if (!editor->autodj_fields[i] || !*gtk_entry_get_text(GTK_ENTRY(editor->autodj_fields[i]))) continue;
            gdouble point;
            if (!read_number(editor->autodj_fields[i], &point) || point < values[1] || point > values[3])
                message = "AutoDJ işaretleri kırpılan aralık içinde olmalı.";
            else if (i) auto_out = point; else auto_in = point;
        }
        if (auto_in >= auto_out) message = "AutoDJ IN, AutoDJ OUT'tan önce olmalı.";
    } else if (values[6] > values[3] - values[1]) {
        message = "Bindirme A öğesinin cue aralığından uzun olamaz.";
    }
    if (editor->kind == RADIOS_EDITOR_TRANSITION && editor->ctx->audio) {
        if (editor->pair_details[0]) {
            JsonObject *version = json_object_get_object_member(json_node_get_object(editor->pair_details[0]), "version");
            gdouble measured = radios_json_int(version, "duration_ms", 0) / 1000.0;
            if (fabs(values[0] - measured) > .001 || values[3] > measured)
                message = "A toplam süre ve cue çıkış doğrulanmış medya süresiyle uyuşmalı.";
        }
        if (editor->pair_details[1]) {
            JsonObject *version = json_object_get_object_member(json_node_get_object(editor->pair_details[1]), "version");
            gdouble remaining = radios_json_int(version, "duration_ms", 0) / 1000.0 - values[4];
            if (remaining <= 0 || values[5] > remaining || values[6] > remaining)
                message = "B cue, fade ve bindirme doğrulanmış B medya süresini aşamaz.";
        }
    }
    if (message != NULL) {
        set_status(editor, message, TRUE);
        return FALSE;
    }
    return TRUE;
}

static JsonNode *editor_json_copy(JsonNode *node)
{
    JsonGenerator *generator = json_generator_new(); json_generator_set_root(generator, node);
    gchar *text = json_generator_to_data(generator, NULL); JsonParser *parser = json_parser_new();
    JsonNode *copy = json_parser_load_from_data(parser, text, -1, NULL) ? json_node_copy(json_parser_get_root(parser)) : NULL;
    g_free(text); g_object_unref(parser); g_object_unref(generator); return copy;
}

static Snapshot *media_snapshot(JsonObject *detail)
{
    Snapshot *snapshot = g_new0(Snapshot, 1);
    JsonObject *version = json_object_get_object_member(detail, "version");
    JsonNode *marker_node = json_object_get_member(detail, "markers");
    JsonObject *markers = marker_node && JSON_NODE_HOLDS_OBJECT(marker_node) ? json_node_get_object(marker_node) : NULL;
    gint64 duration = radios_json_int(version, "duration_ms", 0);
    const gchar *keys[] = { NULL, "cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms" };
    for (guint i = 0; i < FIELD_COUNT; ++i) {
        gint64 value = i == 0 ? duration : radios_json_int(markers, keys[i], i >= 3 && i <= 6 ? duration : 0);
        gchar buffer[G_ASCII_DTOSTR_BUF_SIZE]; g_ascii_formatd(buffer, sizeof(buffer), "%.3f", value / 1000.0);
        snapshot->values[i] = g_strdup(i == 2 && value == 0 ? "" : buffer);
    }
    const gchar *autodj_keys[] = {"autodj_in_ms", "autodj_out_ms"};
    for (guint i = 0; i < 2; ++i) {
        gint64 value = radios_json_int(markers, autodj_keys[i], -1);
        if (value >= 0) { gchar buffer[G_ASCII_DTOSTR_BUF_SIZE]; g_ascii_formatd(buffer, sizeof(buffer), "%.3f", value / 1000.0); snapshot->autodj[i] = g_strdup(buffer); }
    }
    snapshot->flag = markers && json_object_get_boolean_member(markers, "automatic"); return snapshot;
}

static void media_detail_apply(Editor *editor, JsonNode *result)
{
    g_clear_pointer(&editor->media_detail, json_node_free); editor->media_detail = editor_json_copy(result);
    Snapshot *snapshot = media_snapshot(json_node_get_object(editor->media_detail));
    snapshot_free(editor->store->saved); editor->store->saved = snapshot_copy(snapshot);
    snapshot_apply(editor, snapshot); editor->dirty = FALSE;
    g_ptr_array_set_size(editor->undo, 0); g_ptr_array_set_size(editor->redo, 0); refresh(editor);
}

static void media_reload_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer unused)
{
    (void)ctx; (void)unused; Editor *editor = g_object_get_data(G_OBJECT(owner), "radios-editor");
    editor->media_busy = FALSE; gtk_widget_set_sensitive(editor->save_button, TRUE);
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { set_status(editor, error ? error->message : "Medya işaretleri yüklenemedi; girdiler korunuyor.", TRUE); return; }
    g_clear_pointer(&editor->marker_retry, json_node_free); media_detail_apply(editor, result);
    set_status(editor, "Medyanın güncel işaretleri yüklendi.", FALSE);
}

static void media_reload_response(GtkDialog *dialog, gint response, gpointer value)
{
    Editor *editor = value;
    if (response == GTK_RESPONSE_ACCEPT && !editor->media_busy) {
        JsonObject *media = json_object_get_object_member(json_node_get_object(editor->media_detail), "media");
        JsonNode *body = radios_data_body(editor->ctx); json_object_set_string_member(json_node_get_object(body), "media_id", radios_json_string(media, "_id", ""));
        editor->media_busy = radios_data_request(editor->ctx, editor->dialog, "POST", "/v1/workflows/desktop-media/get", body, media_reload_done, NULL, NULL);
        gtk_widget_set_sensitive(editor->save_button, !editor->media_busy); json_node_free(body);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void media_reload_offer(Editor *editor)
{
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(editor->dialog)), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_NONE, "İşaret girdileri korunuyor.");
    gtk_widget_set_name(dialog, "media-marker-reload-dialog");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "Başka bir oturum kaydı değiştirdi veya önceki kayıt sonucu kesinleşmedi. Güncel kaydı yüklemek yerel değişiklikleri geri alır.");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Girdileri koru", GTK_RESPONSE_CANCEL, "Yeniden yükle", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    g_signal_connect(dialog, "response", G_CALLBACK(media_reload_response), editor);
    g_signal_connect_object(editor->dialog, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    gtk_widget_show_all(dialog);
}

static void media_markers_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer unused)
{
    (void)ctx; (void)unused; Editor *editor = g_object_get_data(G_OBJECT(owner), "radios-editor");
    editor->media_busy = FALSE; gtk_widget_set_sensitive(editor->save_button, TRUE);
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        set_status(editor, error ? error->message : "İşaret kaydı doğrulanamadı; girdiler korunuyor.", TRUE);
        if (error && error->domain == RADIOS_DATA_CLIENT_ERROR && (error->code == RADIOS_DATA_CLIENT_ERROR_CONFLICT || error->code == RADIOS_DATA_CLIENT_ERROR_NOT_FOUND)) {
            g_clear_pointer(&editor->marker_retry, json_node_free); media_reload_offer(editor);
        } else if (error && error->domain == RADIOS_DATA_CLIENT_ERROR && (error->code == RADIOS_DATA_CLIENT_ERROR_VALIDATION || error->code == RADIOS_DATA_CLIENT_ERROR_FORBIDDEN || error->code == RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED)) g_clear_pointer(&editor->marker_retry, json_node_free);
        return;
    }
    g_clear_pointer(&editor->marker_retry, json_node_free);
    /* A user may continue moving markers while the write is in flight. */
    Snapshot *local = snapshot_read(editor); JsonNode *submitted = g_object_get_data(G_OBJECT(owner), "media-marker-submitted");
    gboolean unchanged = TRUE; JsonArray *values = submitted ? json_node_get_array(submitted) : NULL;
    for (guint i = 0; values && i < FIELD_COUNT; ++i)
        if (g_strcmp0(local->values[i], json_array_get_string_element(values, i)) != 0) unchanged = FALSE;
    for (guint i = 0; values && i < 2 && json_array_get_length(values) > FIELD_COUNT + i; ++i)
        if (g_strcmp0(local->autodj[i] ? local->autodj[i] : "", json_array_get_string_element(values, FIELD_COUNT + i)) != 0) unchanged = FALSE;
    if (values && local->flag != GPOINTER_TO_INT(g_object_get_data(G_OBJECT(owner), "media-marker-submitted-flag"))) unchanged = FALSE;
    if (unchanged) media_detail_apply(editor, result);
    else { g_clear_pointer(&editor->media_detail, json_node_free); editor->media_detail = editor_json_copy(result); editor->dirty = TRUE; }
    snapshot_free(local); set_status(editor, unchanged ? (editor->embedded ? "İşaretler kaydedildi." : "Medya işaretleri veritabanına kaydedildi; yayın onayı ayrı.") : "Gönderilen işaretler kaydedildi; sonraki yerel değişiklikler korunuyor.", FALSE);
    if (editor->saved_hook && editor->media_detail) editor->saved_hook(editor->dialog, json_node_get_object(editor->media_detail), editor->saved_user);
}

static void media_markers_save(Editor *editor)
{
    if (editor->media_busy) return;
    JsonObject *detail = json_node_get_object(editor->media_detail), *media = json_object_get_object_member(detail, "media");
    JsonNode *marker_node = json_object_get_member(detail, "markers");
    JsonObject *old = marker_node && JSON_NODE_HOLDS_OBJECT(marker_node) ? json_node_get_object(marker_node) : NULL;
    JsonNode *body = radios_data_body(editor->ctx); JsonObject *object = json_node_get_object(body), *markers = json_object_new();
    const gchar *keys[] = { "cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms" };
    gdouble out = 0; read_number(editor->fields[3], &out);
    for (guint i = 1; i < FIELD_COUNT; ++i) {
        gdouble number = i >= 4 && i <= 6 ? out : 0; read_number(editor->fields[i], &number);
        json_object_set_int_member(markers, keys[i - 1], (gint64)llround(number * 1000));
    }
    const gchar *autodj_keys[] = {"autodj_in_ms", "autodj_out_ms"};
    for (guint i = 0; i < 2; ++i) {
        gdouble number = 0;
        if (editor->autodj_fields[i] && read_number(editor->autodj_fields[i], &number))
            json_object_set_int_member(markers, autodj_keys[i], (gint64)llround(number * 1000));
        else json_object_set_null_member(markers, autodj_keys[i]);
    }
    json_object_set_boolean_member(markers, "automatic", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->flag)));
    json_object_set_object_member(object, "markers", markers);
    if (editor->marker_retry) {
        if (!json_node_equal(json_object_get_member(json_node_get_object(editor->marker_retry), "markers"), json_object_get_member(object, "markers"))) { json_node_free(body); media_reload_offer(editor); return; }
        json_node_free(body); body = editor_json_copy(editor->marker_retry);
    } else {
        json_object_set_string_member(object, "media_id", radios_json_string(media, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(media, "_rev", 0));
        if (old) json_object_set_int_member(object, "expected_marker_revision", radios_json_int(old, "_rev", 0));
        else json_object_set_null_member(object, "expected_marker_revision");
        gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
        editor->marker_retry = editor_json_copy(body);
    }
    JsonNode *submitted = json_node_new(JSON_NODE_ARRAY); JsonArray *values = json_array_new(); json_node_take_array(submitted, values);
    for (guint i = 0; i < FIELD_COUNT; ++i) json_array_add_string_element(values, gtk_entry_get_text(GTK_ENTRY(editor->fields[i])));
    for (guint i = 0; i < 2; ++i) json_array_add_string_element(values, editor->autodj_fields[i] ? gtk_entry_get_text(GTK_ENTRY(editor->autodj_fields[i])) : "");
    g_object_set_data_full(G_OBJECT(editor->dialog), "media-marker-submitted", submitted, (GDestroyNotify)json_node_free);
    g_object_set_data(G_OBJECT(editor->dialog), "media-marker-submitted-flag", GINT_TO_POINTER(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->flag))));
    editor->media_busy = radios_data_request(editor->ctx, editor->dialog, "POST", "/v1/workflows/desktop-media/markers-save", body, media_markers_done, NULL, NULL);
    gtk_widget_set_sensitive(editor->save_button, !editor->media_busy);
    set_status(editor, editor->media_busy ? "Gerçek medyanın işaretleri kaydediliyor." : "Veri servisi bağlı değil; işaret girdileri korunuyor.", !editor->media_busy); json_node_free(body);
}

/* Typed cue edits keep the AutoDJ points inside the trimmed range exactly as
 * dragging the cue markers does: a point outside is clamped, and a pair that
 * is no longer ordered is cleared (the library pass places it again). Points
 * an analysis placed before the operator trimmed the cue never block a save. */
static void
autodj_follow_cue(Editor *editor)
{
    if (editor->kind != RADIOS_EDITOR_MARKERS || !editor->autodj_fields[0] || !editor->autodj_fields[1]) return;
    gdouble in = 0, out = 0;
    if (!read_number(editor->fields[1], &in) || !read_number(editor->fields[3], &out) || in >= out) return;
    gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
    gboolean updating = editor->updating; editor->updating = TRUE;
    gdouble auto_in = in, auto_out = out;
    for (guint i = 0; i < 2; ++i) {
        gdouble number;
        if (!read_number(editor->autodj_fields[i], &number)) continue;
        if (number < in || number > out) {
            number = CLAMP(number, in, out);
            g_ascii_formatd(buffer, sizeof buffer, "%.3f", number); gtk_entry_set_text(GTK_ENTRY(editor->autodj_fields[i]), buffer);
        }
        if (i) auto_out = number; else auto_in = number;
    }
    if (auto_in >= auto_out) for (guint i = 0; i < 2; ++i) gtk_entry_set_text(GTK_ENTRY(editor->autodj_fields[i]), "");
    editor->updating = updating;
}

static void
editor_response(GtkDialog *dialog, gint response, gpointer data)
{
    Editor *editor = data;
    if (response == GTK_RESPONSE_ACCEPT) {
        autodj_follow_cue(editor);
        if (!validate(editor))
            return;
        if (editor->media_detail) { media_markers_save(editor); return; }
        snapshot_free(editor->store->saved);
        editor->store->saved = snapshot_read(editor);
        if (editor->store->document) {
            Snapshot *saved = editor->store->saved;
            JsonNode *config = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(config, object);
            JsonArray *values = json_array_new(), *files = json_array_new();
            for (guint i = 0; i < FIELD_COUNT; ++i) json_array_add_string_element(values, saved->values[i] ? saved->values[i] : "");
            for (guint i = 0; i < 2; ++i) json_array_add_string_element(files, saved->files[i] ? saved->files[i] : "");
            json_object_set_array_member(object, "values", values); json_object_set_array_member(object, "files", files);
            json_object_set_boolean_member(object, "flag", saved->flag); json_object_set_double_member(object, "preview_level", saved->preview_level);
            if (editor->kind == RADIOS_EDITOR_TRANSITION) {
                if (saved->bindings[0]) json_object_set_member(object, "media_a", json_node_copy(saved->bindings[0]));
                if (saved->bindings[1]) json_object_set_member(object, "media_b", json_node_copy(saved->bindings[1]));
            }
            gboolean queued = radios_data_document_save(editor->store->document, config); json_node_free(config);
            set_status(editor, queued ? "İşaret/geçiş taslağı veritabanına gönderildi; onaylı medya veya yayın değişmedi." : "Veritabanı kaydı hazır değil; girdiler korunuyor.", !queued);
            return;
        }
        const gchar *message = "Taslak bu oturumda uygulandı. Veritabanına kaydedilmedi; ses veya yayın komutu gönderilmedi.";
        set_status(editor, message, FALSE);
        if (editor->ctx->notify != NULL)
            editor->ctx->notify(editor->ctx->notify_data, message);
        return;
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void inline_save_clicked(GtkButton *widget, gpointer value)
{
    (void)widget; editor_response(NULL, GTK_RESPONSE_ACCEPT, value);
}

static void
editor_destroyed(GtkWidget *widget, gpointer data)
{
    Editor *editor = data;
    (void)widget;
    if (editor->audio_timer) { g_source_remove(editor->audio_timer); editor->audio_timer = 0; }
    radios_audio_ui_preview_stop(editor->ctx, editor->audio_owner);
    editor->store->dialog = NULL;
}

static gboolean editor_audio_tick(gpointer value)
{
    Editor *editor = value; RadioSAudioUiSnapshot snapshot = {0};
    if (editor->dragging || !radios_audio_ui_preview_owned(editor->ctx, editor->audio_owner) ||
        !radios_audio_ui_get_snapshot(editor->ctx, 32, &snapshot)) return G_SOURCE_CONTINUE;
    if (snapshot.state == RADIOS_AUDIO_UI_PLAYING || snapshot.state == RADIOS_AUDIO_UI_PAUSED || snapshot.state == RADIOS_AUDIO_UI_EOF) {
        editor->cursor = snapshot.position_ms / 1000.0;
        RadioSAudioUiBus bus = RADIOS_AUDIO_UI_CUE;
        radios_audio_ui_preview_get_output(editor->ctx, editor->audio_owner, &bus, NULL);
        gchar *text = g_strdup_printf("Konum %.3f sn · %s", editor->cursor,
            bus == RADIOS_AUDIO_UI_MONITOR ? "Monitör" : "Kulaklık / CUE");
        gtk_label_set_text(GTK_LABEL(editor->position), text); g_free(text);
        gtk_widget_queue_draw(editor->canvas);
    } else if (snapshot.state == RADIOS_AUDIO_UI_ERROR && snapshot.error[0]) set_status(editor, snapshot.error, TRUE);
    return G_SOURCE_CONTINUE;
}
static void editor_preview_action(GtkButton *widget, gpointer value)
{
    Editor *editor = value; GError *error = NULL;
    const gchar *name = gtk_widget_get_name(GTK_WIDGET(widget));
    if (g_str_equal(name, "qwf-preview-stop")) radios_audio_ui_preview_stop(editor->ctx, editor->audio_owner);
    else if (g_str_equal(name, "qwf-preview-pause"))
        radios_audio_ui_preview_pause(editor->ctx, editor->audio_owner, &error);
    else if (editor->media_detail) {
        JsonObject *detail = json_node_get_object(editor->media_detail);
        if (g_str_equal(name, "qwf-preview-cue")) {
            gdouble cue = 0; read_number(editor->fields[1], &cue);
            if (radios_audio_ui_preview_cue(editor->ctx, editor->audio_owner, detail,
                    (guint64)llround(MAX(0, cue) * 1000), &error)) {
                editor->cursor = MAX(0, cue); refresh(editor);
                set_status(editor, "CUE girişine dönüldü; ön dinleme duraklatıldı.", FALSE);
            }
        } else {
            gdouble in = 0, out = 0; read_number(editor->fields[1], &in); read_number(editor->fields[3], &out);
            gboolean played = out > in ? radios_audio_ui_preview_play_range(editor->ctx, editor->audio_owner, detail,
                (guint64)llround(MAX(0, editor->cursor) * 1000), (guint64)llround(MAX(0, in) * 1000),
                (guint64)llround(out * 1000), &error) : radios_audio_ui_preview_play(editor->ctx, editor->audio_owner, detail,
                (guint64)llround(MAX(0, editor->cursor) * 1000), &error);
            if (played) set_status(editor, "Ön dinleme oynatılıyor.", FALSE);
        }
    }
    if (error) { set_status(editor, error->message, TRUE); g_clear_error(&error); }
}

static void
source_changed(GtkFileChooserButton *chooser, gpointer data)
{
    Editor *editor = data;
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    guint deck = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(chooser), "deck"));
    gtk_widget_set_sensitive(editor->clear[deck], filename != NULL);
    field_changed(GTK_WIDGET(chooser), editor);
    set_status(editor, "Yerel dosya seçildi. Ses çözümleme, ölçülmüş waveform ve ön dinleme bağlantısı bekleniyor.", FALSE);
    g_free(filename);
}

static void
source_clear(GtkButton *button, gpointer data)
{
    Editor *editor = data;
    guint deck = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "deck"));
    if (editor->ctx->audio) {
        Snapshot *next = snapshot_read(editor); g_clear_pointer(&next->bindings[deck], json_node_free);
        g_clear_pointer(&next->files[deck], g_free); history_push(editor->undo, editor->current); editor->current = NULL;
        snapshot_apply(editor, next); editor->dirty = TRUE; radios_data_document_dirty(editor->store->document, TRUE);
        radios_audio_ui_preview_stop(editor->ctx, editor->audio_owner); set_status(editor, "Geçiş kaynağı kaldırıldı; eski dosya kullanılmaz.", FALSE); return;
    }
    gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(editor->files[deck]));
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    field_changed(GTK_WIDGET(button), editor);
    set_status(editor, "Yerel dosya seçimi kaldırıldı.", FALSE);
}
static JsonNode *transition_binding(JsonObject *detail)
{
    JsonObject *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version");
    if (!media || !version) return NULL;
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *o = json_object_new(); json_node_take_object(node, o);
    json_object_set_string_member(o, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_string_member(o, "version_id", radios_json_string(version, "_id", ""));
    json_object_set_string_member(o, "media_file_id", radios_json_string(media, "media_file_id", ""));
    json_object_set_string_member(o, "media_sha256", radios_json_string(version, "sha256", "")); return node;
}
static gboolean transition_matches(JsonNode *binding, JsonObject *detail)
{
    JsonNode *actual = transition_binding(detail); gboolean matches = actual && binding && json_node_equal(binding, actual);
    g_clear_pointer(&actual, json_node_free); return matches;
}
static void transition_controls(Editor *editor)
{
    if (editor->preview_play) gtk_widget_set_sensitive(editor->preview_play, editor->ctx->audio && editor->pair_details[0] && editor->pair_details[1]);
    if (editor->preview_stop) gtk_widget_set_sensitive(editor->preview_stop, editor->ctx->audio != NULL);
}
typedef struct { guint deck, generation; } PairRequest;
static void transition_detail_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; PairRequest *request = value; Editor *editor = g_object_get_data(G_OBJECT(owner), "radios-editor");
    guint deck = request->deck; if (!editor || request->generation != editor->pair_generation[deck]) return;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result) || !transition_matches(editor->pair_bindings[deck], json_node_get_object(result))) {
        set_status(editor, error ? error->message : "Geçişin medya sürümü değişmiş; kaynağı yeniden seç.", TRUE); return;
    }
    g_clear_pointer(&editor->pair_details[deck], json_node_free); editor->pair_details[deck] = editor_json_copy(result);
    JsonObject *media = json_object_get_object_member(json_node_get_object(result), "media"), *version = json_object_get_object_member(json_node_get_object(result), "version");
    gchar *text = g_strdup_printf("%s · %.3f sn · SHA %.12s", radios_json_string(media, "title", ""), radios_json_int(version, "duration_ms", 0) / 1000.0, radios_json_string(version, "sha256", ""));
    gtk_label_set_text(GTK_LABEL(editor->pair_labels[deck]), text); g_free(text); transition_controls(editor);
}
static void transition_bindings_restore(Editor *editor, const Snapshot *snapshot)
{
    if (!editor->ctx->audio) return;
    for (guint i = 0; i < 2; ++i) {
        gboolean same = (!editor->pair_bindings[i] && !snapshot->bindings[i]) || (editor->pair_bindings[i] && snapshot->bindings[i] && json_node_equal(editor->pair_bindings[i], snapshot->bindings[i]));
        if (same) continue;
        ++editor->pair_generation[i]; g_clear_pointer(&editor->pair_bindings[i], json_node_free); g_clear_pointer(&editor->pair_details[i], json_node_free);
        editor->pair_bindings[i] = snapshot->bindings[i] ? json_node_copy(snapshot->bindings[i]) : NULL;
        gtk_widget_set_sensitive(editor->clear[i], editor->pair_bindings[i] != NULL);
        gtk_label_set_text(GTK_LABEL(editor->pair_labels[i]), editor->pair_bindings[i] ? "Bağlı medya doğrulanıyor…" : "Doğrulanmış kaynak seçilmedi.");
        if (editor->pair_bindings[i]) {
            PairRequest *request = g_new0(PairRequest, 1); request->deck = i; request->generation = editor->pair_generation[i];
            radios_media_library_get(editor->ctx, editor->dialog, radios_json_string(json_node_get_object(editor->pair_bindings[i]), "media_id", ""), transition_detail_done, request, g_free);
        }
    }
    transition_controls(editor);
}
static void transition_picked(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail, gpointer value)
{
    (void)ctx; Editor *editor = g_object_get_data(G_OBJECT(owner), "radios-editor"); guint deck = GPOINTER_TO_UINT(value);
    if (!editor || deck > 1) return;
    JsonObject *version = json_object_get_object_member(detail, "version");
    if (!version || !json_object_get_boolean_member(version, "verified") || !json_object_get_boolean_member(version, "object_available")) {
        set_status(editor, "Geçişe atamak için dosyası mevcut ve doğrulanmış bir medya sürümü seç.", TRUE); return;
    }
    JsonNode *binding = transition_binding(detail); if (!binding) return;
    Snapshot *next = snapshot_read(editor); g_clear_pointer(&next->bindings[deck], json_node_free); next->bindings[deck] = binding;
    g_clear_pointer(&next->files[deck], g_free); next->files[deck] = g_strdup("");
    if (deck == 0) {
        const gchar *keys[] = {"cue_in_ms", "intro_ms", "out_ms"}; JsonNode *marks = json_object_get_member(detail, "markers");
        JsonObject *markers = marks && JSON_NODE_HOLDS_OBJECT(marks) ? json_node_get_object(marks) : NULL;
        guint64 duration = (guint64)radios_json_int(version, "duration_ms", 0);
        for (guint i = 0; i < 4; ++i) {
            gdouble number = i == 0 ? duration / 1000.0 : radios_json_int(markers, keys[i - 1], i == 3 ? (gint64)duration : 0) / 1000.0;
            gchar buffer[G_ASCII_DTOSTR_BUF_SIZE]; g_ascii_formatd(buffer, sizeof buffer, "%.3f", number);
            g_free(next->values[i]); next->values[i] = g_strdup(i == 2 && number == 0 ? "" : buffer);
        }
    }
    history_push(editor->undo, editor->current); editor->current = NULL; g_ptr_array_set_size(editor->redo, 0);
    snapshot_apply(editor, next); editor->dirty = TRUE; radios_data_document_dirty(editor->store->document, TRUE);
    radios_audio_ui_preview_stop(editor->ctx, editor->audio_owner); set_status(editor, "Doğrulanmış medya sürümü geçişe bağlandı; Kaydet ile kalıcı hale gelir.", FALSE);
}
static void transition_pick(GtkButton *button, gpointer value)
{
    Editor *editor = value; guint deck = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "deck"));
    radios_media_library_pick(editor->ctx, editor->dialog, transition_picked, GUINT_TO_POINTER(deck));
}
static void transition_preview_action(GtkButton *widget, gpointer value)
{
    Editor *editor = value; GError *error = NULL;
    if (GTK_WIDGET(widget) == editor->preview_stop) { radios_audio_ui_preview_stop(editor->ctx, editor->audio_owner); set_status(editor, "CUE geçiş önizlemesi durduruldu.", FALSE); return; }
    if (!validate(editor) || !editor->pair_details[0] || !editor->pair_details[1]) return;
    gdouble n[8] = {0}; for (guint i = 0; i < 8; ++i) read_number(editor->fields[i], &n[i]);
    gboolean started = radios_audio_ui_transition_preview(editor->ctx, editor->dialog,
        json_node_get_object(editor->pair_details[0]), json_node_get_object(editor->pair_details[1]),
        (guint64)llround(n[1] * 1000), (guint64)llround(n[3] * 1000), (guint64)llround(n[4] * 1000),
        (guint64)llround(n[5] * 1000), (guint64)llround(n[6] * 1000), n[7], gtk_range_get_value(GTK_RANGE(editor->preview_level)), &error);
    set_status(editor, started ? "A/B geçişi hazırlanıyor; örnek zamanıyla yalnız CUE çıkışında çalacak." : error ? error->message : "Geçiş hazırlanamadı.", !started);
    g_clear_error(&error);
}

static GtkWidget *
source_new(Editor *editor, guint deck)
{
    GtkWidget *box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "transition-source");
    pack(box, radios_ui_label(deck == 0 ? "A · Çıkış öğesi" : "B · Giriş öğesi", "heading"), FALSE);
    pack(box, radios_ui_label("Program öğesi", "form-label"), FALSE);
    static const gchar *const empty[] = { "Program öğesi seçilmedi", NULL };
    GtkWidget *combo = radios_ui_combo(empty, 0);
    gtk_widget_set_name(combo, deck == 0 ? "transition-source-a" : "transition-source-b");
    gtk_widget_set_sensitive(combo, FALSE);
    pack(box, combo, FALSE);
    if (editor->ctx->audio) {
        GtkWidget *pick_button = radios_ui_button("Kütüphaneden seç");
        gtk_widget_set_name(pick_button, deck == 0 ? "transition-pick-a" : "transition-pick-b");
        g_object_set_data(G_OBJECT(pick_button), "deck", GUINT_TO_POINTER(deck));
        g_signal_connect(pick_button, "clicked", G_CALLBACK(transition_pick), editor); pack(box, pick_button, FALSE);
    }
    editor->files[deck] = gtk_file_chooser_button_new("Yerel ses dosyası seç", GTK_FILE_CHOOSER_ACTION_OPEN);
    gtk_widget_set_name(editor->files[deck], deck == 0 ? "transition-file-a" : "transition-file-b");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Ses dosyaları");
    radios_ui_file_filter_add_audio(filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(editor->files[deck]), filter);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(editor->files[deck]), TRUE);
    if (editor->ctx->audio) { gtk_widget_set_sensitive(editor->files[deck], FALSE); gtk_widget_set_tooltip_text(editor->files[deck], "Yerel dosyayı önce medya kütüphanesine içe aktar; doğrulanmış sürümü seç."); }
    g_object_set_data(G_OBJECT(editor->files[deck]), "deck", GUINT_TO_POINTER(deck));
    g_signal_connect(editor->files[deck], "file-set", G_CALLBACK(source_changed), editor);
    pack(box, editor->files[deck], FALSE);
    editor->clear[deck] = radios_ui_button("Kaldır");
    gtk_widget_set_name(editor->clear[deck], deck == 0 ? "transition-clear-a" : "transition-clear-b");
    gtk_widget_set_sensitive(editor->clear[deck], FALSE);
    g_object_set_data(G_OBJECT(editor->clear[deck]), "deck", GUINT_TO_POINTER(deck));
    g_signal_connect(editor->clear[deck], "clicked", G_CALLBACK(source_clear), editor);
    pack(box, editor->clear[deck], FALSE);
    editor->pair_labels[deck] = radios_ui_label("Doğrulanmış kaynak seçilmedi.", "transition-file-info");
    pack(box, editor->pair_labels[deck], FALSE);
    return box;
}

static void
marker_timeline_new(Editor *editor, GtkWidget *body)
{
    GtkWidget *toolbar = editor->embedded ? gtk_flow_box_new() : radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "qwf-toolbar");
    if (editor->embedded) {
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(toolbar), GTK_SELECTION_NONE);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(toolbar), 6); gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(toolbar), 6);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(toolbar), 6);
    }
    const guint embedded_indices[] = {8, 9, 6, 4, 5, 2};
    const gchar *embedded_labels[] = {"AutoDJ IN", "AutoDJ OUT", "Geçiş noktası", "Fade başlangıcı", "Fade sonu", "Intro"};
    for (guint slot = 0; slot < 6; ++slot) {
        guint i = editor->embedded ? embedded_indices[slot] : slot + 1;
        GtkWidget *button = radios_ui_button(editor->embedded ? embedded_labels[slot] : marker_labels[i]);
        gchar *name = g_strdup_printf("qwf-set-%u", i);
        gtk_widget_set_name(button, name);
        g_free(name);
        g_object_set_data(G_OBJECT(button), "marker-index", GUINT_TO_POINTER(i));
        g_signal_connect(button, "clicked", G_CALLBACK(marker_clicked), editor);
        if (editor->embedded) gtk_container_add(GTK_CONTAINER(toolbar), button); else pack(toolbar, button, FALSE);
    }
    pack(body, toolbar, FALSE);
    editor->position = radios_ui_label("", "qwf-position output");
    pack(body, editor->position, FALSE);
    if (editor->embedded) {
        GtkWidget *transport = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "qwf-transport");
        const gchar *labels[] = { "▶ Oynat", "Ⅱ Duraklat", "■", "CUE" };
        const gchar *names[] = { "qwf-preview-play", "qwf-preview-pause", "qwf-preview-stop", "qwf-preview-cue" };
        for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
            GtkWidget *control = radios_ui_button(labels[i]); gtk_widget_set_name(control, names[i]);
            gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
            gtk_widget_set_sensitive(control, editor->media_detail && editor->ctx->audio);
            g_signal_connect(control, "clicked", G_CALLBACK(editor_preview_action), editor); pack(transport, control, FALSE);
        }
        editor->preview_output = gtk_combo_box_text_new();
        gtk_widget_set_valign(editor->preview_output, GTK_ALIGN_CENTER);
        gtk_widget_set_name(editor->preview_output, "qwf-preview-output");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(editor->preview_output), "Kulaklık / CUE");
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(editor->preview_output), "Monitör");
        RadioSAudioUiBus bus = RADIOS_AUDIO_UI_CUE; gdouble gain = 0;
        radios_audio_ui_preview_get_output(editor->ctx, editor->audio_owner, &bus, &gain);
        gtk_combo_box_set_active(GTK_COMBO_BOX(editor->preview_output), bus == RADIOS_AUDIO_UI_MONITOR ? 1 : 0);
        gtk_widget_set_sensitive(editor->preview_output, editor->ctx->audio != NULL);
        atk_object_set_name(gtk_widget_get_accessible(editor->preview_output), "Ön dinleme çıkışı");
        pack(transport, editor->preview_output, TRUE);
        editor->preview_gain = radios_knob_new("Ön dinleme · dB", -60, 6, .5, gain);
        gtk_widget_set_name(editor->preview_gain, "qwf-preview-gain"); gtk_widget_set_sensitive(editor->preview_gain, editor->ctx->audio != NULL);
        GtkWidget *dial = g_object_get_data(G_OBJECT(editor->preview_gain), "dial");
        gtk_widget_set_tooltip_text(dial, "Ön dinleme seviyesini ayarla: sürükle, tekerleği veya ok tuşlarını kullan.");
        pack(transport, editor->preview_gain, FALSE);
        g_signal_connect(editor->preview_output, "changed", G_CALLBACK(preview_output_changed), editor);
        g_signal_connect(g_object_get_data(G_OBJECT(editor->preview_gain), "adjustment"), "value-changed", G_CALLBACK(preview_output_changed), editor);
        pack(body, transport, FALSE);
    }
    editor->canvas = gtk_drawing_area_new();
    gtk_widget_set_name(editor->canvas, "qwf-timeline");
    gtk_widget_set_size_request(editor->canvas, 500, editor->embedded ? 230 : 170);
    gtk_widget_set_can_focus(editor->canvas, TRUE);
    atk_object_set_name(gtk_widget_get_accessible(editor->canvas), "Parça işaretlerinin zaman çizgisi");
    atk_object_set_description(gtk_widget_get_accessible(editor->canvas), "Ok tuşları konumu değiştirir; Home ve End başa ve sona gider. Ölçülmüş dalga biçimi bekleniyor.");
    gtk_widget_add_events(editor->canvas, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(editor->canvas, "draw", G_CALLBACK(timeline_draw), editor);
    g_signal_connect(editor->canvas, "button-press-event", G_CALLBACK(timeline_press), editor);
    g_signal_connect(editor->canvas, "button-release-event", G_CALLBACK(timeline_release), editor);
    g_signal_connect(editor->canvas, "motion-notify-event", G_CALLBACK(timeline_motion), editor);
    g_signal_connect(editor->canvas, "key-press-event", G_CALLBACK(timeline_key), editor);
    pack(body, editor->canvas, FALSE);
    if (editor->embedded) {
        GtkWidget *selection = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "qwf-selection");
        const gchar *selection_names[] = {"qwf-selection-in", "qwf-selection-out"};
        const gchar *selection_labels[] = {"Seçim IN", "Seçim OUT"};
        for (guint i = 0; i < 2; ++i) {
            GtkWidget *button = radios_ui_button(selection_labels[i]); gtk_widget_set_name(button, selection_names[i]);
            g_object_set_data(G_OBJECT(button), "selection-index", GUINT_TO_POINTER(i));
            g_signal_connect(button, "clicked", G_CALLBACK(selection_clicked), editor); pack(selection, button, FALSE);
        }
        editor->crop_button = radios_ui_button("Seçimi kırp"); gtk_widget_set_name(editor->crop_button, "qwf-crop-selection");
        g_signal_connect(editor->crop_button, "clicked", G_CALLBACK(crop_clicked), editor); pack(selection, editor->crop_button, FALSE);
        editor->selection_label = radios_ui_label("İki nokta seç", "qwf-selection-summary");
        gtk_widget_set_name(editor->selection_label, "qwf-selection-summary"); pack(selection, editor->selection_label, TRUE);
        pack(body, selection, FALSE);
        GtkWidget *overlap = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "qwf-overlap-control");
        pack(overlap, radios_ui_label("Birlikte çalma ↔", "form-label"), FALSE);
        editor->overlap = gtk_spin_button_new_with_range(0, 30, .05); gtk_spin_button_set_digits(GTK_SPIN_BUTTON(editor->overlap), 2);
        gtk_widget_set_name(editor->overlap, "qwf-overlap-seconds");
        atk_object_set_name(gtk_widget_get_accessible(editor->overlap), "İki parçanın birlikte çalma süresi, saniye");
        g_signal_connect(editor->overlap, "value-changed", G_CALLBACK(overlap_changed), editor); pack(overlap, editor->overlap, FALSE);
        pack(overlap, radios_ui_label("sn", "form-label"), FALSE); pack(body, overlap, FALSE);
        gtk_widget_set_tooltip_text(editor->canvas, "Sürükleyerek iki nokta seç; Seçimi kırp ile çalınacak aralığı ayarla. İşaret tutamaklarını sürükleyebilirsin.");
    }
    GtkWidget *tools = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "qwf-tools");
    static const gchar *const zoom_labels[] = { "+", "−", "Tüm süre" };
    static const gchar *const zoom_names[] = { "qwf-zoom-in", "qwf-zoom-out", "qwf-fit" };
    for (guint i = 0; i < 3; ++i) {
        GtkWidget *button = radios_ui_button(zoom_labels[i]);
        gtk_widget_set_name(button, zoom_names[i]);
        g_object_set_data(G_OBJECT(button), "zoom-direction", GINT_TO_POINTER(i == 0 ? 1 : i == 1 ? -1 : 0));
        g_signal_connect(button, "clicked", G_CALLBACK(zoom_clicked), editor);
        pack(tools, button, FALSE);
    }
    editor->undo_button = radios_ui_button("Geri al");
    editor->redo_button = radios_ui_button("İleri al");
    gtk_widget_set_name(editor->undo_button, "qwf-undo");
    gtk_widget_set_name(editor->redo_button, "qwf-redo");
    g_signal_connect(editor->undo_button, "clicked", G_CALLBACK(history_clicked), editor);
    g_signal_connect(editor->redo_button, "clicked", G_CALLBACK(history_clicked), editor);
    pack(tools, editor->undo_button, FALSE);
    pack(tools, editor->redo_button, FALSE);
    GtkWidget *reset = radios_ui_button("İşaretleri sıfırla");
    gtk_widget_set_name(reset, "qwf-reset");
    g_signal_connect(reset, "clicked", G_CALLBACK(reset_clicked), editor);
    pack(tools, reset, FALSE);
    if (!editor->embedded) {
    GtkWidget *preview = radios_ui_button("▶ Ön dinle");
    gtk_widget_set_name(preview, "qwf-preview-play");
    gtk_widget_set_sensitive(preview, editor->media_detail && editor->ctx->audio);
    g_signal_connect(preview, "clicked", G_CALLBACK(editor_preview_action), editor);
    pack(tools, preview, FALSE);
    GtkWidget *pause = radios_ui_button("Ⅱ"); gtk_widget_set_name(pause, "qwf-preview-pause");
    GtkWidget *stop = radios_ui_button("■"); gtk_widget_set_name(stop, "qwf-preview-stop");
    gtk_widget_set_sensitive(pause, editor->media_detail && editor->ctx->audio);
    gtk_widget_set_sensitive(stop, editor->media_detail && editor->ctx->audio);
    g_signal_connect(pause, "clicked", G_CALLBACK(editor_preview_action), editor);
    g_signal_connect(stop, "clicked", G_CALLBACK(editor_preview_action), editor);
    pack(tools, pause, FALSE); pack(tools, stop, FALSE);
    }
    pack(body, tools, FALSE);
}

static void editor_config_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; (void)owner; EditorStore *store = data;
    Snapshot *snapshot = g_new0(Snapshot, 1);
    JsonArray *values = json_object_get_array_member(config, "values"), *files = json_object_get_array_member(config, "files");
    for (guint i = 0; i < FIELD_COUNT; ++i) snapshot->values[i] = g_strdup(json_array_get_string_element(values, i));
    for (guint i = 0; files && i < MIN(2u, json_array_get_length(files)); ++i) snapshot->files[i] = g_strdup(json_array_get_string_element(files, i));
    const gchar *bindings[] = {"media_a", "media_b"};
    for (guint i = 0; i < 2; ++i) {
        JsonNode *binding = json_object_get_member(config, bindings[i]);
        if (binding && JSON_NODE_HOLDS_OBJECT(binding)) snapshot->bindings[i] = editor_json_copy(binding);
    }
    snapshot->flag = json_object_get_boolean_member(config, "flag"); snapshot->preview_level = json_object_get_double_member(config, "preview_level");
    snapshot_free(store->saved); store->saved = snapshot;
    if (store->dialog) {
        Editor *editor = g_object_get_data(G_OBJECT(store->dialog), "radios-editor");
        if (editor) { editor->dirty = FALSE; snapshot_apply(editor, snapshot_copy(snapshot)); }
        if (editor) set_status(editor, "Veritabanındaki işaret/geçiş taslağı hazır; gerçek medya doğrulaması ayrı yapılır.", FALSE);
    }
}
static GtkWidget *
editor_open(RadioSViewContext *ctx, RadioSEditorKind kind, JsonObject *media_detail, GtkWidget *inline_owner)
{
    gchar *media_key = media_detail ? g_strdup_printf("radios-media-marker-store-%s", radios_json_string(json_object_get_object_member(media_detail, "media"), "_id", "")) : NULL;
    const gchar *key = media_key ? media_key : kind == RADIOS_EDITOR_MARKERS ? "radios-marker-store" : "radios-transition-store";
    EditorStore *store = inline_owner ? g_new0(EditorStore, 1) : g_object_get_data(G_OBJECT(ctx->parent), key);
    if (store == NULL) {
        store = g_new0(EditorStore, 1);
        g_object_set_data_full(G_OBJECT(ctx->parent), key, store, store_free);
    }
    g_free(media_key);
    if (store->dialog != NULL) {
        gtk_window_present(GTK_WINDOW(store->dialog));
        return store->dialog;
    }
    Editor *editor = g_new0(Editor, 1);
    editor->ctx = ctx;
    editor->kind = kind;
    editor->store = store; editor->embedded = inline_owner != NULL;
    if (media_detail) {
        JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, media_detail);
        editor->media_detail = editor_json_copy(node); json_node_free(node);
        snapshot_free(store->saved); store->saved = media_snapshot(media_detail);
    }
    editor->zoom = 1;
    editor->drag_marker = -1;
    editor->undo = g_ptr_array_new_with_free_func(snapshot_free);
    editor->redo = g_ptr_array_new_with_free_func(snapshot_free);
    GtkWidget *body;
    if (inline_owner) {
        editor->dialog = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "qwf-form");
        gtk_widget_set_name(editor->dialog, "media-marker-panel");
        editor->audio_owner = inline_owner;
        /* Preserve the original marker selector scope inside the reusable
         * panel; async request ownership stays on the outer widget. */
        body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "qwf-form");
        gtk_widget_set_name(body, "queue-waveform-dialog");
        GtkStyleContext *style = gtk_widget_get_style_context(body);
        gtk_style_context_add_class(style, "radios");
        gtk_style_context_add_class(style, "qwf-dialog");
        pack(editor->dialog, body, FALSE);
    } else {
        editor->dialog = gtk_dialog_new_with_buttons(kind == RADIOS_EDITOR_MARKERS ? "Parça işaretleri" : "Cue ve geçiş editörü", ctx->parent,
            GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç / Kapat", GTK_RESPONSE_CANCEL,
            media_detail ? "Medya işaretlerini kaydet" : "Oturum taslağına uygula", GTK_RESPONSE_ACCEPT, NULL);
        editor->audio_owner = editor->dialog;
        editor->save_button = gtk_dialog_get_widget_for_response(GTK_DIALOG(editor->dialog), GTK_RESPONSE_ACCEPT);
        gtk_widget_set_name(editor->dialog, kind == RADIOS_EDITOR_MARKERS ? "queue-waveform-dialog" : "transition-editor");
        GtkStyleContext *style = gtk_widget_get_style_context(editor->dialog);
        gtk_style_context_add_class(style, "radios");
        gtk_style_context_add_class(style, kind == RADIOS_EDITOR_MARKERS ? "qwf-dialog" : "transition-editor-dialog");
        gtk_window_set_default_size(GTK_WINDOW(editor->dialog), 880, 700);
        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, kind == RADIOS_EDITOR_MARKERS ? "qwf-form" : "transition-form");
        gtk_container_set_border_width(GTK_CONTAINER(body), 20); gtk_container_add(GTK_CONTAINER(scroll), body);
        pack(gtk_dialog_get_content_area(GTK_DIALOG(editor->dialog)), scroll, TRUE);
    }
    store->dialog = editor->dialog;
    pack(body, radios_ui_label(editor->embedded ? "Dalga biçimi ve parça işaretleri" : media_detail ? "Parça işaretleri · Doğrulanmış medya" : kind == RADIOS_EDITOR_MARKERS ? "Parça işaretleri · Oturum taslağı" : "Cue ve geçiş editörü · Oturum taslağı", "heading"), FALSE);
    if (media_detail && !editor->embedded) {
        JsonObject *media = json_object_get_object_member(media_detail, "media"), *version = json_object_get_object_member(media_detail, "version"), *analysis = json_object_get_object_member(media_detail, "analysis");
        gchar *text = g_strdup_printf("%s · ölçülen süre %.3f sn · %" G_GINT64_FORMAT " Hz\nSHA-256: %s · kırpılan örnek: %" G_GINT64_FORMAT,
            radios_json_string(media, "title", ""), radios_json_int(version, "duration_ms", 0) / 1000.0,
            radios_json_int(analysis, "sample_rate", 0), radios_json_string(version, "sha256", ""), radios_json_int(analysis, "clipped_samples", 0));
        GtkWidget *label = radios_ui_label(text, "qwf-help"); gtk_widget_set_name(label, "qwf-media-info"); pack(body, label, FALSE); g_free(text);
    }
    if (kind == RADIOS_EDITOR_TRANSITION) {
        GtkWidget *sources = gtk_grid_new();
        gtk_grid_set_column_spacing(GTK_GRID(sources), 14);
        gtk_grid_set_column_homogeneous(GTK_GRID(sources), TRUE);
        gtk_grid_attach(GTK_GRID(sources), source_new(editor, 0), 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(sources), source_new(editor, 1), 1, 0, 1, 1);
        pack(body, sources, FALSE);
    }
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    for (guint i = 0; i < FIELD_COUNT; ++i) {
        GtkWidget *field = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "qwf-field");
        const gchar *label = kind == RADIOS_EDITOR_MARKERS ? marker_labels[i] : transition_labels[i];
        pack(field, radios_ui_label(label, "form-label"), FALSE);
        editor->fields[i] = radios_ui_entry(i == 2 || (kind == RADIOS_EDITOR_MARKERS && i >= 4 && i <= 6) ? "İsteğe bağlı" : "Saniye");
        gtk_widget_set_name(editor->fields[i], kind == RADIOS_EDITOR_MARKERS ? marker_names[i] : transition_names[i]);
        gtk_entry_set_input_purpose(GTK_ENTRY(editor->fields[i]), GTK_INPUT_PURPOSE_NUMBER);
        gtk_entry_set_max_length(GTK_ENTRY(editor->fields[i]), 20);
        if (media_detail && i == 0) gtk_editable_set_editable(GTK_EDITABLE(editor->fields[i]), FALSE);
        gtk_widget_set_hexpand(editor->fields[i], TRUE);
        pack(field, editor->fields[i], FALSE);
        if (kind == RADIOS_EDITOR_TRANSITION && i >= 6) {
            editor->mix[i - 6] = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                i == 6 ? 0 : -30, i == 6 ? 30 : 0, i == 6 ? 0.1 : 1);
            gtk_scale_set_draw_value(GTK_SCALE(editor->mix[i - 6]), FALSE);
            g_object_set_data(G_OBJECT(editor->mix[i - 6]), "field-index", GUINT_TO_POINTER(i));
            g_signal_connect(editor->mix[i - 6], "value-changed", G_CALLBACK(mix_changed), editor);
            pack(field, editor->mix[i - 6], FALSE);
        }
        gtk_grid_attach(GTK_GRID(grid), field, i % 2, i / 2, 1, 1);
    }
    if (kind == RADIOS_EDITOR_MARKERS) {
        for (guint i = 0; i < 2; ++i) {
            GtkWidget *field = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "qwf-field");
            pack(field, radios_ui_label(i ? "AutoDJ OUT" : "AutoDJ IN", "form-label"), FALSE);
            editor->autodj_fields[i] = radios_ui_entry("İsteğe bağlı");
            gtk_widget_set_name(editor->autodj_fields[i], i ? "qwf-autodj-out" : "qwf-autodj-in");
            gtk_entry_set_input_purpose(GTK_ENTRY(editor->autodj_fields[i]), GTK_INPUT_PURPOSE_NUMBER);
            pack(field, editor->autodj_fields[i], FALSE); gtk_grid_attach(GTK_GRID(grid), field, i, 4, 1, 1);
        }
    }
    if (editor->embedded) { gtk_widget_set_no_show_all(grid, TRUE); gtk_widget_hide(grid); }
    pack(body, grid, FALSE);
    editor->flag = gtk_check_button_new_with_label(kind == RADIOS_EDITOR_MARKERS ? "Otomatik geçiş taslağı" : "Elle kilitle · otomatik önerilere kapalı");
    gtk_widget_set_name(editor->flag, kind == RADIOS_EDITOR_MARKERS ? "qwf-automatic" : "transition-lock");
    if (editor->embedded) { gtk_widget_set_no_show_all(editor->flag, TRUE); gtk_widget_hide(editor->flag); }
    pack(body, editor->flag, FALSE);
    if (kind == RADIOS_EDITOR_MARKERS) {
        marker_timeline_new(editor, body);
    } else {
        GtkWidget *preview = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "transition-preview-box");
        GtkWidget *play = radios_ui_button("Ön dinle");
        GtkWidget *stop = radios_ui_button("Durdur");
        editor->preview_play = play; editor->preview_stop = stop;
        gtk_widget_set_name(play, "transition-preview-play"); gtk_widget_set_name(stop, "transition-preview-stop");
        g_signal_connect(play, "clicked", G_CALLBACK(transition_preview_action), editor);
        g_signal_connect(stop, "clicked", G_CALLBACK(transition_preview_action), editor);
        gtk_widget_set_sensitive(play, FALSE);
        gtk_widget_set_sensitive(stop, FALSE);
        pack(preview, play, FALSE);
        pack(preview, stop, FALSE);
        pack(preview, radios_ui_label("Seviye · dB", "form-label"), FALSE);
        GtkWidget *level = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -36, 0, 1);
        editor->preview_level = level;
        gtk_range_set_value(GTK_RANGE(level), -12);
        gtk_widget_set_name(level, "transition-preview-level");
        g_signal_connect(level, "value-changed", G_CALLBACK(field_changed), editor);
        pack(preview, level, TRUE);
        pack(body, preview, FALSE);
    }
    editor->status = radios_ui_label("", kind == RADIOS_EDITOR_MARKERS ? "qwf-status" : "transition-editor-status");
    gtk_widget_set_name(editor->status, "editor-status");
    pack(body, editor->status, FALSE);
    Snapshot *initial = store->saved != NULL ? snapshot_copy(store->saved) : g_new0(Snapshot, 1);
    if (store->saved == NULL) {
        initial->preview_level = -12;
        for (guint i = 0; i < FIELD_COUNT; ++i)
            initial->values[i] = g_strdup(i == 1 || i == 7 ? "0" : kind == RADIOS_EDITOR_TRANSITION && i == 4 ? "0" : kind == RADIOS_EDITOR_TRANSITION && (i == 5 || i == 6) ? "2" : "");
    }
    snapshot_apply(editor, initial);
    for (guint i = 0; i < FIELD_COUNT; ++i)
        g_signal_connect(editor->fields[i], "changed", G_CALLBACK(field_changed), editor);
    for (guint i = 0; i < 2; ++i)
        if (editor->autodj_fields[i]) g_signal_connect(editor->autodj_fields[i], "changed", G_CALLBACK(field_changed), editor);
    g_signal_connect(editor->flag, "toggled", G_CALLBACK(field_changed), editor);
    if (inline_owner) {
        editor->save_button = radios_ui_button("İşaretleri kaydet");
        gtk_widget_set_name(editor->save_button, "qwf-markers-save");
        gtk_widget_set_halign(editor->save_button, GTK_ALIGN_END);
        g_signal_connect(editor->save_button, "clicked", G_CALLBACK(inline_save_clicked), editor);
        pack(body, editor->save_button, FALSE);
    } else g_signal_connect(editor->dialog, "response", G_CALLBACK(editor_response), editor);
    g_signal_connect(editor->dialog, "destroy", G_CALLBACK(editor_destroyed), editor);
    g_object_set_data_full(G_OBJECT(editor->dialog), "radios-editor", editor, editor_free);
    if (!media_detail && !store->document && ctx->data)
        store->document = radios_data_document_new(ctx, GTK_WIDGET(ctx->parent), kind == RADIOS_EDITOR_MARKERS ? "markers" : "transition",
            kind == RADIOS_EDITOR_MARKERS ? "Parça işaret taslağı" : "Geçiş ayarları", editor_config_apply, store);
    if (!inline_owner) gtk_widget_show_all(editor->dialog);
    if (media_detail && ctx->audio) editor->audio_timer = g_timeout_add(100, editor_audio_tick, editor);
    if (media_detail) {
        atk_object_set_description(gtk_widget_get_accessible(editor->canvas), "Gerçek PCM çözümlemesinden hesaplanan min/max waveform ve sürüklenebilir medya işaretleri.");
        g_object_set_data_full(G_OBJECT(editor->canvas), "radios-waveform-analysis", editor_json_copy(json_object_get_member(media_detail, "analysis")), (GDestroyNotify)json_node_free);
        set_status(editor, editor->embedded ? "" : "Gerçek medya waveform'u hazır. İşaretler bu SHA-256 sürümüne bağlı taslak olarak kaydedilir; yayın onayı ve ön dinleme ayrı.", FALSE);
    }
    return editor->dialog;
}

GtkWidget *radios_marker_editor_embed(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail)
{
    if (!detail) {
        GtkWidget *panel = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "qwf-form");
        gtk_widget_set_name(panel, "media-marker-panel");
        pack(panel, radios_ui_label("Parça işaretleri", "heading"), FALSE);
        return panel;
    }
    return editor_open(ctx, RADIOS_EDITOR_MARKERS, detail, owner);
}

gboolean radios_marker_editor_dirty(GtkWidget *panel)
{
    Editor *editor = panel ? g_object_get_data(G_OBJECT(panel), "radios-editor") : NULL;
    return editor && (editor->dirty || editor->media_busy || editor->marker_retry != NULL);
}

JsonObject *radios_marker_editor_detail(GtkWidget *panel)
{
    Editor *editor = panel ? g_object_get_data(G_OBJECT(panel), "radios-editor") : NULL;
    return editor && editor->media_detail ? json_node_get_object(editor->media_detail) : NULL;
}

void radios_marker_editor_set_grid(GtkWidget *panel, const RadioSRhythmAnalysis *grid)
{
    Editor *editor = panel ? g_object_get_data(G_OBJECT(panel), "radios-editor") : NULL; if (!editor) return;
    editor->grid_valid = grid != NULL && grid->bpm > 0;
    if (grid) editor->grid = *grid;
    if (editor->canvas) gtk_widget_queue_draw(editor->canvas);
}

void radios_marker_editor_set_snap(GtkWidget *panel, gboolean snap)
{
    Editor *editor = panel ? g_object_get_data(G_OBJECT(panel), "radios-editor") : NULL;
    if (editor) editor->snap = snap;
}

gboolean radios_marker_editor_cursor(GtkWidget *panel, gdouble *seconds)
{
    Editor *editor = panel ? g_object_get_data(G_OBJECT(panel), "radios-editor") : NULL; gdouble duration;
    if (!editor || !duration_read(editor, &duration)) return FALSE;
    *seconds = CLAMP(editor->cursor, 0, duration); return TRUE;
}

void radios_marker_editor_clear_autodj(GtkWidget *panel)
{
    Editor *editor = panel ? g_object_get_data(G_OBJECT(panel), "radios-editor") : NULL;
    if (!editor || !editor->current || !editor->autodj_fields[0] || !editor->autodj_fields[1]) return;
    Snapshot *next = snapshot_copy(editor->current);
    for (guint i = 0; i < 2; ++i) { g_free(next->autodj[i]); next->autodj[i] = NULL; }
    history_push(editor->undo, snapshot_copy(editor->current));
    g_ptr_array_set_size(editor->redo, 0);
    snapshot_apply(editor, next);
    editor->dirty = TRUE; radios_data_document_dirty(editor->store->document, TRUE);
    set_status(editor, "AutoDJ IN/OUT temizlendi; kaydedince varsayılan noktalar kullanılır.", FALSE);
}

void radios_marker_editor_set_saved(GtkWidget *panel, RadioSMarkerEditorSaved saved, gpointer user)
{
    Editor *editor = panel ? g_object_get_data(G_OBJECT(panel), "radios-editor") : NULL;
    if (!editor) return;
    editor->saved_hook = saved; editor->saved_user = user;
}

void
radios_transition_editor_open(RadioSViewContext *ctx)
{
    editor_open(ctx, RADIOS_EDITOR_TRANSITION, NULL, NULL);
}

void
radios_marker_editor_open(RadioSViewContext *ctx)
{
    editor_open(ctx, RADIOS_EDITOR_MARKERS, NULL, NULL);
}

void radios_marker_editor_open_media(RadioSViewContext *ctx, JsonObject *detail)
{
    JsonNode *media = detail ? json_object_get_member(detail, "media") : NULL;
    JsonNode *version = detail ? json_object_get_member(detail, "version") : NULL;
    JsonNode *analysis = detail ? json_object_get_member(detail, "analysis") : NULL;
    if (!media || !version || !analysis || !JSON_NODE_HOLDS_OBJECT(media) || !JSON_NODE_HOLDS_OBJECT(version) || !JSON_NODE_HOLDS_OBJECT(analysis) ||
        !g_uuid_string_is_valid(radios_json_string(json_node_get_object(media), "_id", "")) || radios_json_int(json_node_get_object(version), "duration_ms", 0) < 1) {
        if (ctx->notify) ctx->notify(ctx->notify_data, "Doğrulanmış medya ayrıntısı eksik; işaret editörü açılamadı.");
        return;
    }
    editor_open(ctx, RADIOS_EDITOR_MARKERS, detail, NULL);
}

typedef struct {
    RadioSViewContext *ctx;
    RadioSEditorKind kind;
} EditorBinding;

static void
binding_free(gpointer data, GClosure *closure)
{
    (void)closure;
    g_free(data);
}

static void
editor_clicked(GtkButton *button, gpointer data)
{
    EditorBinding *binding = data;
    (void)button;
    editor_open(binding->ctx, binding->kind, NULL, NULL);
}

void
radios_ui_bind_editor(GtkWidget *button, RadioSViewContext *ctx, RadioSEditorKind kind)
{
    EditorBinding *binding = g_new0(EditorBinding, 1);
    binding->ctx = ctx;
    binding->kind = kind;
    g_signal_connect_data(button, "clicked", G_CALLBACK(editor_clicked), binding, binding_free, 0);
}
