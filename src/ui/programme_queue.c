#include "radios/programme_queue.h"
#include "radios/widgets.h"
#include "radios/playout_ui.h"
#include <string.h>

/* The event overview and an event's flat playback list share the same table. */

static const GtkTargetEntry targets[] = {
    {(gchar *)RADIOS_DRAG_ROW, GTK_TARGET_SAME_APP, RADIOS_DRAG_INFO_ROW},
    {(gchar *)RADIOS_DRAG_MEDIA, GTK_TARGET_SAME_APP, RADIOS_DRAG_INFO_MEDIA},
    {(gchar *)RADIOS_DRAG_PROGRAMME, GTK_TARGET_SAME_APP, RADIOS_DRAG_INFO_PROGRAMME},
    {(gchar *)RADIOS_DRAG_PLAYLIST, GTK_TARGET_SAME_APP, RADIOS_DRAG_INFO_PLAYLIST},
};

const GtkTargetEntry *radios_drag_targets(guint *count)
{
    if (count) *count = G_N_ELEMENTS(targets);
    return targets;
}
void radios_drag_set_payload(GtkSelectionData *selection, const gchar *payload)
{
    gtk_selection_data_set(selection, gtk_selection_data_get_target(selection), 8,
        (const guchar *)payload, (gint)strlen(payload));
}
gchar *radios_drag_payload(GtkSelectionData *selection)
{
    gint length = gtk_selection_data_get_length(selection);
    const guchar *data = gtk_selection_data_get_data(selection);
    return length > 0 && data ? g_strndup((const gchar *)data, (gsize)length) : NULL;
}

typedef struct {
    RadioSProgrammeQueueHooks hooks;
    GtkWidget *root, *list, *scroll, *mark, *navigation, *title, *column;
    JsonObject *config;
    RadioSProgrammeTree *tree, *run_tree;
    gchar *opened, *selected, *playing, *next, *drag_id;
    GHashTable *completed, *event_runs, *event_run_completed;
    GHashTable *players; /* leaf id -> player (1..32) it actually sounded on */
    guint default_player;
    gchar *player_names[32];
    guint64 event_completion_serial;
    GtkCssProvider *row_style, *progress_style;
    guint64 position_ms, duration_ms;
    guint progress_grey;
    const gchar *mark_class;
    gboolean paused, rendering, drag_block, destroyed, hide_hints;
    guint expand_idle;
    /* Kılavuz 2026-10-18: display window, transition (G), ending blink,
     * live/talk state and the states of planned 1.1 overlays. */
    GHashTable *completed_leaves, *overlays, *completed_overlays;
    gchar *transition_source, *transition_target, *ending_event, *live_event, *talk_event;
    GtkWidget *talk_button; /* the R held by pointer or keyboard; weak */
    guint event_show, content_show, ending_timer, rebuild_idle;
    gboolean live_active, talk_held, ending_phase, rebuild_pending;
    /* The live step the flow waits at; run-time live labels (event id ->
     * "dj"/"talk"/"music"); the row whose selection a click has just
     * cleared (compared only, never dereferenced) until that click is over. */
    gchar *live_step;
    GHashTable *live_labels;
    GtkWidget *deselect_row;
    guint deselect_idle;
    gboolean multi_click;
} Queue;

/* S · V · R · L · × share the controls column; sub-rows indent by BRANCH_WIDTH. */
enum { NUMBER_WIDTH = 66, DURATION_WIDTH = 78, CONTROLS_WIDTH = 176, BRANCH_WIDTH = 18 };

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
static Queue *queue_get(GtkWidget *widget) { return g_object_get_data(G_OBJECT(widget), "radios-programme-queue"); }
static RadioSProgrammeTree *playback_tree(Queue *q) { return q->run_tree ? q->run_tree : q->tree; }
static void add_class(GtkWidget *widget, const gchar *name) { gtk_style_context_add_class(gtk_widget_get_style_context(widget), name); }
static void remove_class(GtkWidget *widget, const gchar *name) { gtk_style_context_remove_class(gtk_widget_get_style_context(widget), name); }
static void set_class(GtkWidget *widget, const gchar *name, gboolean enabled)
{ if (enabled) add_class(widget, name); else remove_class(widget, name); }
static void progress_apply(Queue *q);
static void event_runs_apply(Queue *q);
static void players_apply(Queue *q);
static void overlay_apply(Queue *q);
static void transition_apply(Queue *q);
static void live_apply(Queue *q);
static void ending_apply(Queue *q);
static void ending_clear(Queue *q);
static void talk_end(Queue *q, gboolean notify);
static gboolean rebuild_idle_cb(gpointer data);
static gboolean window_refresh(Queue *q);
static gboolean event_flag(JsonObject *run, const gchar *key);
/* Providers are attached only to this queue's rows and descendants, so their
 * contrast rules cannot change player cards or any other broadcast table. */
static void row_style_attach(GtkWidget *widget, gpointer data)
{
    Queue *q = data;
    gtk_style_context_add_provider(gtk_widget_get_style_context(widget),
        GTK_STYLE_PROVIDER(q->row_style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 20);
    if (GTK_IS_CONTAINER(widget)) gtk_container_foreach(GTK_CONTAINER(widget), row_style_attach, q);
}
static void pack(GtkWidget *box, GtkWidget *child, gboolean expand) { gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static GtkWidget *line(const gchar *value, const gchar *style)
{
    GtkWidget *label = radios_ui_label(value, style);
    gtk_label_set_line_wrap(GTK_LABEL(label), FALSE); gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(label), 0); return label;
}
static gchar *duration_text(gint64 milliseconds, gboolean known)
{
    if (!known) return g_strdup("—:—");
    gint64 seconds = (milliseconds + 500) / 1000;
    return g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, seconds / 60, seconds % 60);
}
static gboolean leaf_bound(JsonObject *leaf) { return json_object_has_member(leaf, "media_id"); }
static const gchar *row_id(GtkWidget *row) { return g_object_get_data(G_OBJECT(row), "programme-item-id"); }
const gchar *radios_programme_queue_row_id(GtkListBoxRow *row) { return row ? row_id(GTK_WIDGET(row)) : NULL; }
static gboolean row_is_block(GtkWidget *row) { return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "programme-item-block")); }
/* A planned jingle/sweeper sub-row (1.1) of a content: display only. */
static gboolean row_is_overlay(GtkWidget *row) { return g_object_get_data(G_OBJECT(row), "programme-overlay-title") != NULL; }
static const gchar *row_event_id(GtkWidget *row) { return g_object_get_data(G_OBJECT(row), "programme-event-id"); }
/* The id a body click selects: the owner content for an overlay row. */
static const gchar *row_select_id(GtkWidget *row)
{
    const gchar *select = g_object_get_data(G_OBJECT(row), "programme-select-id");
    return select ? select : row_id(row);
}
/* Jingle/sweeper events attach below the preceding main event (1.1);
 * advertisement events are main rows whose contents become their sub-rows. */
static gboolean kind_attaches(const gchar *kind) { return g_str_equal(kind, "jingle") || g_str_equal(kind, "sweeper"); }
static gboolean kind_is_parent(const gchar *kind) { return !kind_attaches(kind) && !g_str_equal(kind, "advertisement"); }
static gboolean kind_is_live(const gchar *kind) { return g_str_equal(kind, "live"); }
static const gchar *kind_label(const gchar *kind)
{ return kind_is_live(kind) && !radios_programme_kind_valid(kind) ? "Canlı" : radios_programme_kind_label(kind); }
static const gchar *kind_class(const gchar *kind)
{ return kind_attaches(kind) ? "programme-kind-jingle" : g_str_equal(kind, "advertisement") ? "programme-kind-advertisement" : NULL; }
static JsonObject *block_live(JsonObject *block)
{
    JsonNode *node = block ? json_object_get_member(block, "live") : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
/* Finished by the flow's own COMPLETE receipt: only then does the event leave
 * the active list (PDF s.11). An independent event run is an additional
 * source; its completion stays a "Tamamlandı" badge on the row and never
 * advances or shortens the flow's list. */
static gboolean block_done(Queue *q, const gchar *id)
{
    return g_hash_table_contains(q->completed, id);
}
static gboolean blank(const gchar *value)
{ for (; value && *value; ++value) if (!g_ascii_isspace(*value)) return FALSE; return TRUE; }

/* ---- Rows ------------------------------------------------------------- */
static void queue_rebuild(Queue *q);
static void select_id(Queue *q, const gchar *id, gboolean notify);
static void selection_clear(Queue *q, GtkWidget *clicked);
static gboolean open_selected(gpointer data);

static void open_schedule(Queue *q)
{
    if (!q->expand_idle) q->expand_idle = g_idle_add(open_selected, q);
}

static void state_apply(GtkWidget *label, const gchar *state, const gchar *value)
{
    static const gchar *const states[] = {"ld-state--ready", "ld-state--missing", "ld-state--pending", "ld-state--playing", "ld-state--paused", "ld-state--completed"};
    for (guint i = 0; i < G_N_ELEMENTS(states); ++i) remove_class(label, states[i]);
    gchar *style = g_strconcat("ld-state--", state, NULL); add_class(label, style); g_free(style);
    gtk_label_set_text(GTK_LABEL(label), value);
}
static void leaf_state(Queue *q, JsonObject *leaf, GtkWidget *label)
{
    const gchar *id = text(leaf, "id");
    if (q->playing && g_str_equal(q->playing, id)) state_apply(label, q->paused ? "paused" : "playing", q->paused ? "Ⅱ Duraklatıldı" : "▶ Çalıyor");
    else if (g_hash_table_contains(q->completed_leaves, id)) state_apply(label, "completed", "✓ Tamamlandı");
    else if (leaf_bound(leaf)) state_apply(label, "ready", "✓ Hazır");
    else state_apply(label, "missing", "✕ Dosya eksik");
}
static void block_state(RadioSProgrammeBlock *block, GtkWidget *label)
{
    gboolean missing = FALSE;
    for (guint i = 0; i < block->leaves->len; ++i) {
        JsonObject *leaf = g_ptr_array_index(block->leaves, i);
        missing |= !leaf_bound(leaf);
    }
    if (!block->leaves->len) state_apply(label, "pending", "○ Kontrol bekliyor");
    else if (missing) state_apply(label, "missing", "✕ Dosya eksik");
    else state_apply(label, "ready", "✓ Hazır");
}

static void action_clicked(GtkButton *button, gpointer data)
{
    Queue *q = data;
    gchar *id = g_strdup(g_object_get_data(G_OBJECT(button), "programme-item-id"));
    /* L acts on the row's event, also from a content row of that event. */
    gchar *target = g_strdup(g_object_get_data(G_OBJECT(button), "programme-live-id"));
    RadioSProgrammeRowAction action = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "programme-row-action"));
    gboolean block = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "programme-item-block"));
    select_id(q, id, TRUE);
    if (action == RADIOS_PROGRAMME_ROW_WAVEFORM && block) {
        open_schedule(q);
    } else if (q->hooks.action) q->hooks.action(q->hooks.data, target ? target : id, action);
    g_free(id); g_free(target);
}
/* PDF s.8: a second click on the selected content row clears the selection.
 * An event row's click opens the event and an overlay row's click selects its
 * song, so neither of them toggles. */
static gboolean row_toggles(Queue *q, GtkWidget *row)
{
    const gchar *id = row ? row_id(row) : NULL;
    return q->selected && id && !row_is_block(row) && !row_is_overlay(row) && g_str_equal(id, q->selected);
}
static void number_clicked(GtkButton *button, gpointer data)
{
    Queue *q = data;
    if (row_toggles(q, gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_LIST_BOX_ROW))) { selection_clear(q, NULL); return; }
    gchar *id = g_strdup(g_object_get_data(G_OBJECT(button), "programme-item-id"));
    select_id(q, id, TRUE);
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "programme-item-block"))) {
        open_schedule(q);
    }
    g_free(id);
}

static void row_event_after(GtkWidget *handle, GdkEvent *event, gpointer data)
{
    Queue *q = data;
    /* A double click never clears the selection: its second release follows
     * a multi-press. */
    if (event->type == GDK_BUTTON_PRESS || event->type == GDK_2BUTTON_PRESS || event->type == GDK_3BUTTON_PRESS) {
        if (event->button.button == 1) q->multi_click = event->type != GDK_BUTTON_PRESS;
        return;
    }
    if (event->type != GDK_BUTTON_RELEASE || event->button.button != 1 || q->drag_id) return;
    GtkWidget *row = g_object_get_data(G_OBJECT(handle), "programme-row");
    if (!q->multi_click && row_toggles(q, row)) { selection_clear(q, row); return; }
    gchar *id = g_strdup(row_select_id(row));
    select_id(q, id, TRUE);
    if (row_is_block(row)) open_schedule(q);
    g_free(id);
}

/* ---- Context menu: Özellikler (plus İçeriği göster on an event row) ----- */
static void menu_properties(GtkMenuItem *item, gpointer data)
{
    Queue *q = data; gchar *id = g_strdup(row_select_id(g_object_get_data(G_OBJECT(item), "programme-row")));
    select_id(q, id, TRUE);
    if (q->hooks.action) q->hooks.action(q->hooks.data, id, RADIOS_PROGRAMME_ROW_PROPERTIES);
    g_free(id);
}
static void menu_contents(GtkMenuItem *item, gpointer data)
{
    Queue *q = data; gchar *id = g_strdup(row_select_id(g_object_get_data(G_OBJECT(item), "programme-row")));
    select_id(q, id, TRUE); open_schedule(q); g_free(id);
}
static void row_menu_release(GtkWidget *row, gpointer data)
{
    (void)data; GtkWidget *menu = g_object_get_data(G_OBJECT(row), "programme-row-menu");
    if (!menu) return;
    g_object_set_data(G_OBJECT(row), "programme-row-menu", NULL);
    gtk_widget_destroy(menu); g_object_unref(menu);
}
static void menu_item(Queue *q, GtkWidget *menu, GtkWidget *row, const gchar *label, const gchar *name, GCallback callback)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label); gtk_widget_set_name(item, name);
    g_object_set_data(G_OBJECT(item), "programme-row", row);
    g_signal_connect(item, "activate", callback, q);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}
static GtkWidget *row_menu(Queue *q, GtkWidget *row)
{
    GtkWidget *menu = g_object_get_data(G_OBJECT(row), "programme-row-menu");
    if (menu) return menu;
    menu = g_object_ref_sink(gtk_menu_new()); gtk_widget_set_name(menu, "programme-row-menu");
    menu_item(q, menu, row, "Özellikler", "programme-menu-properties", G_CALLBACK(menu_properties));
    if (row_is_block(row) && !GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "programme-row-live")))
        menu_item(q, menu, row, "İçeriği göster", "programme-menu-contents", G_CALLBACK(menu_contents));
    gtk_widget_show_all(menu);
    gtk_menu_attach_to_widget(GTK_MENU(menu), row, NULL);
    g_object_set_data(G_OBJECT(row), "programme-row-menu", menu);
    return menu;
}
static gboolean row_button_press(GtkWidget *handle, GdkEventButton *event, gpointer data)
{
    Queue *q = data;
    if (event->type != GDK_BUTTON_PRESS || event->button != 3 || q->drag_id) return FALSE;
    GtkWidget *row = g_object_get_data(G_OBJECT(handle), "programme-row");
    gchar *id = g_strdup(row_select_id(row)); select_id(q, id, TRUE); g_free(id);
    gtk_menu_popup_at_pointer(GTK_MENU(row_menu(q, row)), (GdkEvent *)event);
    return TRUE;
}

/* ---- R: push-to-talk. Press and release are separate actions; a held R
 * also releases when the pointer leaves it, when it turns unavailable or
 * when its row is rebuilt. It never emits PLAY. --------------------------- */
static void talk_end(Queue *q, gboolean notify)
{
    GtkWidget *button = q->talk_button;
    if (!button) return;
    g_object_remove_weak_pointer(G_OBJECT(button), (gpointer *)&q->talk_button); q->talk_button = NULL;
    remove_class(button, "is-held");
    gchar *id = g_strdup(g_object_get_data(G_OBJECT(button), "programme-live-id"));
    if (notify && id && q->hooks.action) q->hooks.action(q->hooks.data, id, RADIOS_PROGRAMME_ROW_TALK_RELEASE);
    g_free(id);
    /* Rows whose rebuild waited for this release follow right after it. */
    if (q->rebuild_pending && !q->destroyed && !q->rebuild_idle) q->rebuild_idle = g_idle_add(rebuild_idle_cb, q);
}
static void talk_begin(Queue *q, GtkWidget *button)
{
    if (q->talk_button || !gtk_widget_get_sensitive(button)) return;
    q->talk_button = button; g_object_add_weak_pointer(G_OBJECT(button), (gpointer *)&q->talk_button);
    add_class(button, "is-held");
    gchar *id = g_strdup(g_object_get_data(G_OBJECT(button), "programme-live-id"));
    if (id && q->hooks.action) q->hooks.action(q->hooks.data, id, RADIOS_PROGRAMME_ROW_TALK_PRESS);
    g_free(id);
}
static gboolean talk_key(const GdkEventKey *key)
{ return key->keyval == GDK_KEY_space || key->keyval == GDK_KEY_Return || key->keyval == GDK_KEY_KP_Enter; }
static gboolean talk_button_event(GtkWidget *button, GdkEvent *event, gpointer data)
{
    Queue *q = data;
    switch (event->type) {
    case GDK_BUTTON_PRESS: if (event->button.button == 1) talk_begin(q, button); break;
    case GDK_BUTTON_RELEASE: if (event->button.button == 1 && q->talk_button == button) talk_end(q, TRUE); break;
    case GDK_LEAVE_NOTIFY:
        if (event->crossing.mode == GDK_CROSSING_NORMAL && event->crossing.detail != GDK_NOTIFY_INFERIOR && q->talk_button == button)
            talk_end(q, TRUE);
        break;
    case GDK_KEY_PRESS: if (talk_key(&event->key)) { talk_begin(q, button); return TRUE; } break;
    case GDK_KEY_RELEASE: if (talk_key(&event->key)) { if (q->talk_button == button) talk_end(q, TRUE); return TRUE; } break;
    default: break;
    }
    return FALSE;
}

static void row_drag_begin(GtkWidget *handle, GdkDragContext *context, gpointer data)
{
    (void)context; Queue *q = data; GtkWidget *row = g_object_get_data(G_OBJECT(handle), "programme-row");
    if (q->expand_idle) { g_source_remove(q->expand_idle); q->expand_idle = 0; }
    g_free(q->drag_id); q->drag_id = g_strdup(row_id(row)); q->drag_block = row_is_block(row);
}
static void row_drag_get(GtkWidget *handle, GdkDragContext *context, GtkSelectionData *selection, guint info, guint time, gpointer data)
{
    (void)context; (void)info; (void)time; (void)data;
    GtkWidget *row = g_object_get_data(G_OBJECT(handle), "programme-row");
    radios_drag_set_payload(selection, row_id(row));
}
static void mark_clear(Queue *q)
{
    if (q->mark) { remove_class(q->mark, q->mark_class); g_object_remove_weak_pointer(G_OBJECT(q->mark), (gpointer *)&q->mark); q->mark = NULL; }
    remove_class(q->root, "is-drop-target");
}
static void row_drag_end(GtkWidget *handle, GdkDragContext *context, gpointer data)
{
    (void)handle; (void)context; Queue *q = data;
    g_clear_pointer(&q->drag_id, g_free); mark_clear(q);
}

static GtkWidget *action_button(Queue *q, GtkWidget *box, const gchar *symbol, const gchar *tip, const gchar *id,
    gboolean block, RadioSProgrammeRowAction action, gboolean sensitive)
{
    GtkWidget *button = gtk_button_new_with_label(symbol);
    gtk_widget_set_tooltip_text(button, tip); gtk_widget_set_sensitive(button, sensitive);
    g_object_set_data_full(G_OBJECT(button), "programme-item-id", g_strdup(id), g_free);
    g_object_set_data(G_OBJECT(button), "programme-item-block", GINT_TO_POINTER(block));
    g_object_set_data(G_OBJECT(button), "programme-row-action", GUINT_TO_POINTER(action));
    static const gchar *const names[] = {"play", "waveform", "properties", "remove", "live", "talk", "talk"};
    add_class(button, names[action]);
    gchar *name = g_strconcat("programme-row-", names[action], NULL); gtk_widget_set_name(button, name); g_free(name);
    if (action == RADIOS_PROGRAMME_ROW_TALK_PRESS) {
        static const gchar *const signals[] = {"button-press-event", "button-release-event", "leave-notify-event", "key-press-event", "key-release-event"};
        for (guint i = 0; i < G_N_ELEMENTS(signals); ++i) g_signal_connect(button, signals[i], G_CALLBACK(talk_button_event), q);
    } else g_signal_connect(button, "clicked", G_CALLBACK(action_clicked), q);
    pack(box, button, FALSE); return button;
}

typedef struct {
    const gchar *id, *number, *time, *eyebrow, *title, *meta, *kind, *event_id;
    const gchar *select_id;     /* selected by a body click when not the id (overlay rows) */
    const gchar *overlay_title; /* set for a planned jingle/sweeper sub-row */
    gchar *duration;
    gboolean block, playable, waveform, sub, last, live;
    guint block_index;
    gint leaf_index;
} RowSpec;

/* PDF s.13: "Her alt satır girintili olmalı; ana satıra dirsekli dal
 * çizgisiyle bağlanmalıdır". The branch is drawn over the whole row, edge to
 * edge, so a main row's stub and its sub-rows join into one unbroken line: a
 * sub-row's trunk runs from its top edge to the elbow at its number (└) or on
 * to its bottom edge when more sub-rows follow (├); a main row with sub-rows
 * continues the trunk below its own number. The trunk lies in the middle of
 * the indent left of a sub-row's number and takes the kind accent of the
 * sub-rows (blue jingle/sweeper, purple advertisement) as in the PDF. */
typedef enum { BRANCH_NONE, BRANCH_MAIN, BRANCH_MIDDLE, BRANCH_LAST } BranchKind;
static guint32 branch_rgb(const gchar *kind)
{ return kind && kind_attaches(kind) ? 0x2f6f9f : kind && g_str_equal(kind, "advertisement") ? 0x6d4fa8 : 0x737373; }
static gboolean branch_draw(GtkWidget *row, cairo_t *cr, gpointer data)
{
    (void)data;
    BranchKind kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "programme-branch"));
    GtkWidget *number = g_object_get_data(G_OBJECT(row), "programme-number-button");
    GtkWidget *indent = kind == BRANCH_MAIN ? number : g_object_get_data(G_OBJECT(row), "programme-branch-area");
    gint x = 0, y = 0, indent_x = 0, indent_y = 0;
    if (kind == BRANCH_NONE || !number || !indent || !gtk_widget_translate_coordinates(number, row, 0, 0, &x, &y) ||
        !gtk_widget_translate_coordinates(indent, row, 0, 0, &indent_x, &indent_y)) return FALSE;
    guint32 rgb = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "programme-branch-rgb"));
    gint height = gtk_widget_get_allocated_height(number), bottom = gtk_widget_get_allocated_height(row);
    gdouble trunk = indent_x + BRANCH_WIDTH / 2 + 0.5, elbow = y + height / 2 + 0.5;
    cairo_save(cr);
    cairo_set_source_rgb(cr, ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0, (rgb & 0xff) / 255.0);
    cairo_set_line_width(cr, 1);
    if (kind == BRANCH_MAIN) {
        cairo_move_to(cr, trunk, y + height); cairo_line_to(cr, trunk, bottom);
    } else {
        cairo_move_to(cr, trunk, 0); cairo_line_to(cr, trunk, kind == BRANCH_LAST ? elbow + 0.5 : bottom);
        cairo_move_to(cr, trunk - 0.5, elbow); cairo_line_to(cr, x, elbow);
    }
    cairo_stroke(cr);
    cairo_restore(cr);
    return FALSE;
}
static void branch_set(GtkWidget *row, BranchKind kind, guint32 rgb)
{
    if (!g_object_get_data(G_OBJECT(row), "programme-branch")) g_signal_connect_after(row, "draw", G_CALLBACK(branch_draw), NULL);
    g_object_set_data(G_OBJECT(row), "programme-branch", GINT_TO_POINTER(kind));
    g_object_set_data(G_OBJECT(row), "programme-branch-rgb", GUINT_TO_POINTER(rgb));
}

static GtkWidget *row_new(Queue *q, const RowSpec *spec)
{
    gboolean overlay = spec->overlay_title != NULL;
    const gchar *select = spec->select_id ? spec->select_id : spec->id;
    GtkWidget *row = gtk_list_box_row_new();
    add_class(row, "ld-row"); add_class(row, spec->block ? "programme-block-row" : "programme-child-row");
    if (spec->sub) add_class(row, "programme-sub-row");
    if (spec->kind && kind_class(spec->kind)) add_class(row, kind_class(spec->kind));
    g_object_set_data_full(G_OBJECT(row), "programme-item-id", g_strdup(spec->id), g_free);
    g_object_set_data_full(G_OBJECT(row), "programme-row-key",
        overlay ? g_strdup_printf("%s\n%s", spec->id, spec->overlay_title) : g_strdup(spec->id), g_free);
    g_object_set_data_full(G_OBJECT(row), "programme-row-number", g_strdup(spec->number), g_free);
    g_object_set_data_full(G_OBJECT(row), "programme-event-id", g_strdup(spec->event_id), g_free);
    if (spec->select_id) g_object_set_data_full(G_OBJECT(row), "programme-select-id", g_strdup(spec->select_id), g_free);
    if (overlay) g_object_set_data_full(G_OBJECT(row), "programme-overlay-title", g_strdup(spec->overlay_title), g_free);
    g_object_set_data(G_OBJECT(row), "programme-row-level", GINT_TO_POINTER((spec->sub ? RADIOS_PROGRAMME_ROW_SUB : RADIOS_PROGRAMME_ROW_MAIN) + 1));
    g_object_set_data(G_OBJECT(row), "programme-row-live", GINT_TO_POINTER(spec->live));
    g_object_set_data(G_OBJECT(row), "programme-item-block", GINT_TO_POINTER(spec->block));
    g_object_set_data(G_OBJECT(row), "programme-block-index", GUINT_TO_POINTER(spec->block_index));
    g_object_set_data(G_OBJECT(row), "programme-leaf-index", GINT_TO_POINTER(spec->leaf_index + 1));
    if (overlay) { gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE); gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE); }
    GtkWidget *handle = gtk_event_box_new(); gtk_event_box_set_visible_window(GTK_EVENT_BOX(handle), FALSE);
    g_object_set_data(G_OBJECT(handle), "programme-row", row);
    g_signal_connect(handle, "event-after", G_CALLBACK(row_event_after), q);
    g_signal_connect(handle, "button-press-event", G_CALLBACK(row_button_press), q);
    g_signal_connect(row, "destroy", G_CALLBACK(row_menu_release), NULL);
    if (!overlay) {
        gtk_drag_source_set(handle, GDK_BUTTON1_MASK, &targets[0], 1, GDK_ACTION_MOVE);
        g_signal_connect(handle, "drag-begin", G_CALLBACK(row_drag_begin), q);
        g_signal_connect(handle, "drag-data-get", G_CALLBACK(row_drag_get), q);
        g_signal_connect(handle, "drag-end", G_CALLBACK(row_drag_end), q);
    }
    gtk_container_add(GTK_CONTAINER(row), handle);
    GtkWidget *cells = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 0, "programme-row");
    gtk_container_add(GTK_CONTAINER(handle), cells);

    GtkWidget *number_cell = radios_ui_box(GTK_ORIENTATION_VERTICAL, 3, "broadcast-queue-number");
    gtk_widget_set_size_request(number_cell, NUMBER_WIDTH, -1);
    GtkWidget *number_button = gtk_button_new();
    add_class(number_button, spec->block ? "programme-block-number" : "programme-leaf-number");
    GtkWidget *index = radios_ui_label(spec->number, "broadcast-queue-index");
    gtk_label_set_xalign(GTK_LABEL(index), 0.5f); gtk_container_add(GTK_CONTAINER(number_button), index);
    g_object_set_data_full(G_OBJECT(number_button), "programme-item-id", g_strdup(select), g_free);
    g_object_set_data(G_OBJECT(number_button), "programme-item-block", GINT_TO_POINTER(spec->block));
    g_signal_connect(number_button, "clicked", G_CALLBACK(number_clicked), q);
    g_object_set_data(G_OBJECT(row), "programme-number-button", number_button);
    if (spec->sub) {
        /* Indented small number; the row draws its branch line through the
         * indent (branch_draw). */
        GtkWidget *branch_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 0, "programme-branch");
        GtkWidget *branch = gtk_drawing_area_new(); gtk_widget_set_name(branch, "programme-row-branch");
        gtk_widget_set_size_request(branch, BRANCH_WIDTH, -1);
        pack(branch_row, branch, FALSE); pack(branch_row, number_button, TRUE);
        pack(number_cell, branch_row, FALSE);
        g_object_set_data(G_OBJECT(row), "programme-branch-area", branch);
        branch_set(row, spec->last ? BRANCH_LAST : BRANCH_MIDDLE, branch_rgb(spec->kind));
    } else pack(number_cell, number_button, FALSE);
    GtkWidget *time = radios_ui_label(spec->time, "broadcast-queue-time"); gtk_label_set_xalign(GTK_LABEL(time), 0.5f);
    pack(number_cell, time, FALSE);
    pack(cells, number_cell, FALSE);

    GtkWidget *content = radios_ui_box(GTK_ORIENTATION_VERTICAL, 1, "programme-row-content");
    gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
    pack(content, line(spec->eyebrow, "broadcast-queue-artist"), FALSE);
    GtkWidget *title = line(spec->title, "ld-item-title"); gtk_widget_set_tooltip_text(title, spec->title);
    pack(content, title, FALSE);
    GtkWidget *meta = line(spec->meta, "ld-item-meta");
    gtk_widget_set_name(meta, "programme-row-meta");
    gtk_widget_set_tooltip_text(meta, spec->meta);
    g_object_set_data(G_OBJECT(row), "programme-meta-label", meta);
    pack(content, meta, FALSE);
    if (spec->block) {
        GtkWidget *runtime = line("", "programme-row-runtime");
        gtk_widget_set_name(runtime, "programme-row-runtime");
        gtk_widget_set_no_show_all(runtime, TRUE);
        g_object_set_data(G_OBJECT(row), "programme-runtime-label", runtime);
        pack(content, runtime, FALSE);
    }
    pack(cells, content, TRUE);

    GtkWidget *duration = radios_ui_label(spec->duration, "ld-num broadcast-queue-duration");
    gtk_label_set_xalign(GTK_LABEL(duration), 1.0f); gtk_widget_set_size_request(duration, DURATION_WIDTH, -1);
    pack(cells, duration, FALSE);

    GtkWidget *state_cell = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "broadcast-queue-state");
    gtk_widget_set_size_request(state_cell, CONTROLS_WIDTH, -1); gtk_widget_set_valign(state_cell, GTK_ALIGN_CENTER);
    GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, "broadcast-row-actions");
    GtkWidget *play = action_button(q, actions, "S", "Başlat", spec->id, spec->block, RADIOS_PROGRAMME_ROW_PLAY, spec->playable && !overlay && !spec->live);
    action_button(q, actions, "V", spec->block ? "İçeriği göster" : "Cue ve geçiş", spec->id, spec->block, RADIOS_PROGRAMME_ROW_WAVEFORM,
        spec->waveform && !overlay && !spec->live);
    /* Ö has no button (Özellikler stays in the context menu). L and R act
     * on the row's event; R becomes pressable only for the on-air event. */
    /* PDF s.15: R takes the former Ö place; L sits next to R. */
    GtkWidget *talk = action_button(q, actions, "R", "Bas-konuş: basılı tutarken mikrofon yayında", select, spec->block, RADIOS_PROGRAMME_ROW_TALK_PRESS, FALSE);
    g_object_set_data_full(G_OBJECT(talk), "programme-live-id", g_strdup(spec->event_id), g_free);
    GtkWidget *live = action_button(q, actions, "L", "Canlı yayın (etikete göre)", select, spec->block, RADIOS_PROGRAMME_ROW_LIVE, TRUE);
    g_object_set_data_full(G_OBJECT(live), "programme-live-id", g_strdup(spec->event_id), g_free);
    action_button(q, actions, "×", "Yayın sırasından kaldır", spec->id, spec->block, RADIOS_PROGRAMME_ROW_REMOVE, !overlay);
    pack(state_cell, actions, FALSE);
    GtkWidget *state = line("", "ld-state");
    pack(state_cell, state, FALSE);
    /* Which player(s) the event or content sounds on. A live step sounds on
     * no player (microphone and channels), so its line stays empty. */
    GtkWidget *player = line("", "ld-row-player");
    gtk_widget_set_name(player, "programme-row-player");
    pack(state_cell, player, FALSE);
    if (!overlay && !spec->live) g_object_set_data(G_OBJECT(row), "programme-player-label", player);
    pack(cells, state_cell, FALSE);
    g_object_set_data(G_OBJECT(row), "programme-state-label", state);
    g_object_set_data(G_OBJECT(row), "programme-play-button", play);
    g_object_set_data(G_OBJECT(row), "programme-live-button", live);
    g_object_set_data(G_OBJECT(row), "programme-talk-button", talk);
    row_style_attach(row, q);
    gtk_style_context_add_provider(gtk_widget_get_style_context(row),
        GTK_STYLE_PROVIDER(q->progress_style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 21);
    return row;
}

static GtkWidget *note_row(const gchar *message, const gchar *style, const gchar *block_id, guint block_index)
{
    GtkWidget *row = gtk_list_box_row_new(); add_class(row, style);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE); gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget *label = radios_ui_label(message, "ld-empty-cell"); gtk_label_set_xalign(GTK_LABEL(label), 0.5f);
    gtk_container_add(GTK_CONTAINER(row), label);
    g_object_set_data_full(G_OBJECT(row), "programme-row-key", g_strdup(message), g_free);
    if (block_id) {
        g_object_set_data(G_OBJECT(row), "programme-empty-block", GINT_TO_POINTER(1));
        g_object_set_data(G_OBJECT(row), "programme-block-index", GUINT_TO_POINTER(block_index));
    }
    return row;
}

static void progress_apply(Queue *q)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(q->list));
    for (GList *item = rows; item; item = item->next) {
        GtkWidget *row = item->data; const gchar *id = row_id(row);
        if (!id) continue;
        gboolean upcoming = !row_is_block(row) && q->next && g_str_equal(id, q->next);
        gboolean completed = row_is_block(row) && g_hash_table_contains(q->completed, id);
        set_class(row, "is-upcoming", upcoming);
        set_class(row, "is-upcoming-light", upcoming && q->progress_grey > 127);
        set_class(row, "is-upcoming-dark", upcoming && q->progress_grey <= 127);
        set_class(row, "is-event-completed", completed);
    }
    g_list_free(rows);
}

static void playing_apply(Queue *q)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(q->list));
    guint playing_block = G_MAXUINT; gint playing_leaf = -1;
    RadioSProgrammeTree *running = playback_tree(q); const gchar *playing_event = NULL;
    if (running && q->playing && radios_programme_tree_find(running, q->playing, &playing_block, &playing_leaf) && playing_leaf >= 0)
        playing_event = text(((RadioSProgrammeBlock *)g_ptr_array_index(running->blocks, playing_block))->block, "id");
    for (GList *item = rows; item; item = item->next) {
        GtkWidget *row = item->data; const gchar *id = row_id(row);
        if (!id || row_is_overlay(row)) continue;
        guint block = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "programme-block-index"));
        gint leaf = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "programme-leaf-index")) - 1;
        gboolean playing = playing_leaf >= 0 && (leaf < 0 ? playing_event && g_str_equal(id, playing_event) :
            q->playing && g_str_equal(id, q->playing));
        /* The live step the flow waits at reads as the sounding row. */
        gboolean step = leaf < 0 && q->live_step && g_str_equal(id, q->live_step);
        set_class(row, "is-playing", playing || step);
        GtkWidget *play = g_object_get_data(G_OBJECT(row), "programme-play-button");
        set_class(play, "is-pressed", (playing && !q->paused) || step);
        GtkWidget *state = g_object_get_data(G_OBJECT(row), "programme-state-label");
        if (!q->tree || block >= q->tree->blocks->len) continue;
        RadioSProgrammeBlock *owner = g_ptr_array_index(q->tree->blocks, block);
        if (leaf >= 0 && (guint)leaf < owner->leaves->len) leaf_state(q, g_ptr_array_index(owner->leaves, (guint)leaf), state);
        else if (leaf < 0 && playing)
            state_apply(state, q->paused ? "paused" : "playing", q->paused ? "Ⅱ Duraklatıldı" : "▶ Çalıyor");
        else if (leaf < 0 && g_hash_table_contains(q->completed, id)) state_apply(state, "completed", "✓ Tamamlandı");
        else if (leaf < 0 && kind_is_live(text(owner->block, "kind"))) {
            gboolean on_air = step || (q->live_event && q->live_active && g_str_equal(q->live_event, id));
            state_apply(state, on_air ? "playing" : "ready", on_air ? "● Yayında" : "✓ Hazır");
        }
        else if (leaf < 0) block_state(owner, state);
    }
    g_list_free(rows);
    progress_apply(q);
    event_runs_apply(q);
    overlay_apply(q);
}

/* The accepted plan's player, then the content's own planned player, then
 * the queue's own player; 0 when automation will choose. */
static guint leaf_planned_player(Queue *q, JsonObject *leaf)
{
    const gchar *id = text(leaf, "id"); guint block = 0; gint index = -1;
    if (q->run_tree && *id && radios_programme_tree_find(q->run_tree, id, &block, &index) && index >= 0) {
        JsonObject *accepted = g_ptr_array_index(((RadioSProgrammeBlock *)g_ptr_array_index(q->run_tree->blocks, block))->leaves, (guint)index);
        gint64 planned = number(accepted, "player");
        if (planned >= 1 && planned <= 32) return (guint)planned;
    }
    gint64 planned = number(leaf, "player");
    if (planned >= 1 && planned <= 32) return (guint)planned;
    return q->default_player;
}
static gchar *players_text(Queue *q, guint32 mask)
{
    if (!mask) return g_strdup("Oynatıcı · otomatik");
    GString *value = g_string_new(NULL); guint shown = 0, total = 0;
    for (guint player = 1; player <= 32; ++player) {
        if (!(mask & (1u << (player - 1)))) continue;
        if (++total > 3) continue;
        if (shown++) g_string_append(value, ", ");
        const gchar *name = q->player_names[player - 1];
        if (name && *name) g_string_append(value, name); else g_string_append_printf(value, "Oynatıcı %u", player);
    }
    if (total > shown) g_string_append_printf(value, " +%u", total - shown);
    return g_string_free(value, FALSE);
}
static void players_apply(Queue *q)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(q->list));
    for (GList *item = rows; item; item = item->next) {
        GtkWidget *row = item->data, *label = g_object_get_data(G_OBJECT(row), "programme-player-label");
        if (!label || !q->tree) continue;
        guint block = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "programme-block-index"));
        gint leaf = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "programme-leaf-index")) - 1;
        if (block >= q->tree->blocks->len) continue;
        RadioSProgrammeBlock *owner = g_ptr_array_index(q->tree->blocks, block); guint32 actual = 0, planned = 0;
        for (guint i = 0; i < owner->leaves->len; ++i) {
            if (leaf >= 0 && (guint)leaf != i) continue;
            JsonObject *content = g_ptr_array_index(owner->leaves, i); const gchar *id = text(content, "id");
            guint sounded = *id ? GPOINTER_TO_UINT(g_hash_table_lookup(q->players, id)) : 0, player = leaf_planned_player(q, content);
            if (sounded) actual |= 1u << (sounded - 1);
            else if (player) planned |= 1u << (player - 1);
        }
        if (!owner->leaves->len && q->default_player) planned = 1u << (q->default_player - 1);
        /* An independent event run sounds the whole event on its own player. */
        JsonObject *run = leaf < 0 ? g_hash_table_lookup(q->event_runs, row_id(row)) : NULL;
        if (!run && leaf < 0) run = g_hash_table_lookup(q->event_run_completed, row_id(row));
        gint64 source = run ? number(run, "source") : -1;
        if (run && source >= 0 && source < 32) { actual |= 1u << source; planned = 0; }
        gchar *value = players_text(q, actual | planned);
        if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), value)) {
            gtk_label_set_text(GTK_LABEL(label), value); gtk_widget_set_tooltip_text(label, value);
        }
        g_free(value);
    }
    g_list_free(rows);
}

/* Independent event playback is an additional source; it must never replace
 * this queue's normal playing/next IDs or invent a normal COMPLETE receipt. */
static gboolean event_flag(JsonObject *run, const gchar *key)
{
    JsonNode *node = run ? json_object_get_member(run, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}
static void event_runs_apply(Queue *q)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(q->list));
    for (GList *item = rows; item; item = item->next) {
        GtkWidget *row = item->data, *label = g_object_get_data(G_OBJECT(row), "programme-runtime-label");
        if (!label) continue;
        JsonObject *run = g_hash_table_lookup(q->event_runs, row_id(row));
        if (!run) run = g_hash_table_lookup(q->event_run_completed, row_id(row));
        gboolean completed = run && event_flag(run, "completed");
        set_class(row, "is-event-running", run && !completed);
        set_class(row, "is-event-run-completed", completed);
        gtk_widget_set_visible(label, run != NULL);
        if (!run) { gtk_label_set_text(GTK_LABEL(label), ""); continue; }
        gint64 source = number(run, "source"), state = number(run, "state");
        const gchar *status = completed ? "Tamamlandı" : state == RADIOS_PLAYOUT_PAUSED ? "Duraklatıldı" :
            state == RADIOS_PLAYOUT_PLAYING || state == RADIOS_PLAYOUT_OVERLAP ? "Çalıyor" :
            state == RADIOS_PLAYOUT_WAITING || state == RADIOS_PLAYOUT_MANUAL_WAIT ? "Bekliyor" : "Hazırlanıyor";
        gchar *player = *text(run, "source_name") ? g_strdup(text(run, "source_name")) : g_strdup_printf("Oynatıcı %" G_GINT64_FORMAT, source + 1);
        gint64 cue = MAX(G_GINT64_CONSTANT(0), number(run, "cue_in_ms"));
        gint64 position = MAX(G_GINT64_CONSTANT(0), number(run, "position_ms") - cue);
        gint64 duration = MAX(G_GINT64_CONSTANT(0), number(run, "duration_ms") - cue);
        gchar *clock = duration && !completed ? g_strdup_printf(" · %" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT " / %" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
            position / 60000, position / 1000 % 60, duration / 60000, duration / 1000 % 60) : g_strdup("");
        gchar *value = g_strdup_printf("Yarı AutoDJ · %s · %s%s%s", player, status,
            completed && event_flag(run, "continuing") ? " · Küme geçişi hazırlanıyor" : "", clock);
        gtk_label_set_text(GTK_LABEL(label), value); gtk_widget_set_tooltip_text(label, value);
        g_free(value); g_free(clock); g_free(player);
    }
    g_list_free(rows);
    /* An event run also names the player its event sounds on. */
    players_apply(q);
}

/* Hints come from the accepted plan. Do not infer an overlay from an
 * automation rule: its player or vocal window may have made it unavailable. */
static JsonObject *hint_leaf(Queue *q, JsonObject *leaf)
{
    if (!q->run_tree) return q->hide_hints ? NULL : leaf;
    guint block = 0; gint index = -1;
    if (!radios_programme_tree_find(q->run_tree, text(leaf, "id"), &block, &index) || index < 0) return NULL;
    RadioSProgrammeBlock *owner = g_ptr_array_index(q->run_tree->blocks, block);
    return g_ptr_array_index(owner->leaves, (guint)index);
}
/* The titled planned jingles/sweepers of a music content: its 1.1 sub-rows
 * in the opened event. The objects are borrowed from the plan. */
/* A planned hint is placed over, before or after its song; any other value
 * is not a planned jingle (programme_queue.h) and makes no 1.1 row. */
static gboolean hint_placed(JsonObject *hint)
{
    const gchar *placement = text(hint, "placement");
    return g_str_equal(placement, "overlay") || g_str_equal(placement, "before") || g_str_equal(placement, "after");
}
static GPtrArray *leaf_hints(Queue *q, JsonObject *leaf)
{
    GPtrArray *hints = g_ptr_array_new();
    JsonObject *planned = g_str_equal(text(leaf, "kind"), "music") ? hint_leaf(q, leaf) : NULL;
    JsonNode *node = planned ? json_object_get_member(planned, "planned_jingles") : NULL;
    if (node && JSON_NODE_HOLDS_ARRAY(node)) {
        JsonArray *array = json_node_get_array(node);
        for (guint i = 0; i < json_array_get_length(array); ++i) {
            JsonNode *hint = json_array_get_element(array, i);
            if (hint && JSON_NODE_HOLDS_OBJECT(hint) && !blank(text(json_node_get_object(hint), "title")) && hint_placed(json_node_get_object(hint)))
                g_ptr_array_add(hints, json_node_get_object(hint));
        }
    } else if (planned) {
        node = json_object_get_member(planned, "planned_jingle");
        if (node && JSON_NODE_HOLDS_OBJECT(node) && !blank(text(json_node_get_object(node), "title")) && hint_placed(json_node_get_object(node)))
            g_ptr_array_add(hints, json_node_get_object(node));
    }
    return hints;
}
static gchar *hint_title(JsonObject *hint) { return g_strstrip(g_strdup(text(hint, "title"))); }
static const gchar *hint_kind(JsonObject *hint) { return radios_programme_kind_valid(text(hint, "kind")) ? text(hint, "kind") : "jingle"; }
static gboolean hint_done(Queue *q, JsonObject *leaf, JsonObject *hint, guint index)
{
    gchar *id = radios_programme_hint_id(leaf, hint, index);
    gboolean done = g_hash_table_contains(q->completed_overlays, id) || g_hash_table_contains(q->completed_leaves, id);
    g_free(id); return done;
}
static gboolean hint_sounding(Queue *q, JsonObject *leaf, JsonObject *hint)
{
    if (*text(hint, "entry_id") && q->playing && g_str_equal(q->playing, text(hint, "entry_id"))) return TRUE;
    gchar *key = g_strdup_printf("%s\n%s", text(leaf, "id"), text(hint, "title"));
    guint state = GPOINTER_TO_UINT(g_hash_table_lookup(q->overlays, text(hint, "id")));
    if (!state) state = GPOINTER_TO_UINT(g_hash_table_lookup(q->overlays, key));
    g_free(key);
    return state == RADIOS_PLAYOUT_CONTENT_PLAYING + 1 || state == RADIOS_PLAYOUT_CONTENT_PAUSED + 1;
}
/* "Araya sweeper" / "Üzerine jingle": the relation under a 1.1 row. */
static gchar *hint_relation(JsonObject *hint)
{
    const gchar *placement = text(hint, "placement");
    const gchar *position = g_str_equal(placement, "overlay") ? "Üzerine" :
        g_str_equal(placement, "before") ? "Önce" :
        g_str_equal(placement, "after") ? "Araya" : NULL;
    gchar *label = g_utf8_strdown(radios_programme_kind_label(hint_kind(hint)), -1);
    gchar *relation = position ? g_strdup_printf("%s %s", position, label) : g_strdup(radios_programme_kind_label(hint_kind(hint)));
    g_free(label); return relation;
}
static gchar *leaf_meta_text(Queue *q, JsonObject *leaf)
{
    (void)q; /* planned jingles are the content's own sub-rows, not meta text */
    GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
    const gchar *kind = text(leaf, "kind"), *version = text(leaf, "version_label");
    if (*text(leaf, "album")) g_ptr_array_add(parts, g_strdup(text(leaf, "album")));
    g_ptr_array_add(parts, g_strdup(radios_programme_kind_label(kind)));
    if (*version && !g_str_equal(version, "Taslak")) g_ptr_array_add(parts, g_strdup(version));
    g_ptr_array_add(parts, NULL);
    gchar *result = g_strjoinv(" · ", (gchar **)parts->pdata);
    g_ptr_array_unref(parts);
    return result;
}

/* Refresh accepted hints without rebuilding rows or losing the operator's
 * current event, selection, scroll position or drag operation. */
static void meta_apply(Queue *q)
{
    if (!q->tree) return;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(q->list));
    for (GList *item = rows; item; item = item->next) {
        GtkWidget *row = item->data;
        if (row_is_block(row) || !row_id(row) || row_is_overlay(row)) continue;
        guint block = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "programme-block-index"));
        gint index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "programme-leaf-index")) - 1;
        if (block >= q->tree->blocks->len || index < 0) continue;
        RadioSProgrammeBlock *owner = g_ptr_array_index(q->tree->blocks, block);
        if ((guint)index >= owner->leaves->len) continue;
        GtkWidget *label = g_object_get_data(G_OBJECT(row), "programme-meta-label");
        gchar *value = leaf_meta_text(q, g_ptr_array_index(owner->leaves, (guint)index));
        if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), value)) {
            gtk_label_set_text(GTK_LABEL(label), value);
            gtk_widget_set_tooltip_text(label, value);
        }
        g_free(value);
    }
    g_list_free(rows);
}

/* ---- Display window --------------------------------------------------- */
/* The rows a view shows, in order. Only these rows are built, so a long
 * programme stays cheap; each row keeps its real tree indices for drops. */
typedef struct {
    guint block; gint leaf, overlay;    /* overlay: index into the content's planned hints */
    gboolean sub, last;                 /* a 1.1 row; the last one of its main row */
    guint main_number, sub_number;
    const gchar *note;                  /* a note row instead of an item */
} RowPlan;

static void plan_add(GArray *plan, guint block, gint leaf, gint overlay, gboolean sub, guint main_number, guint sub_number)
{
    RowPlan item = {block, leaf, overlay, sub, sub, main_number, sub_number, NULL};
    if (sub && plan->len) {
        RowPlan *previous = &g_array_index(plan, RowPlan, plan->len - 1);
        if (previous->sub) previous->last = FALSE;
    }
    g_array_append_val(plan, item);
}
static gboolean leaf_in_transition(Queue *q, const gchar *id)
{
    return (q->transition_source && g_str_equal(q->transition_source, id)) ||
        (q->transition_target && g_str_equal(q->transition_target, id));
}
/* The earliest position a limited view must keep: the transition's rows, the
 * sounding content and (event view) the live step on air. Block index, or
 * leaf index inside block_filter. */
static gint plan_anchor(Queue *q, guint block_filter, gboolean leaf_level)
{
    const gchar *ids[] = {q->transition_source, q->transition_target, q->playing};
    gint anchor = -1;
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) {
        guint block = 0; gint leaf = -1;
        if (!ids[i] || !radios_programme_tree_find(q->tree, ids[i], &block, &leaf) || leaf < 0) continue;
        if (leaf_level && block != block_filter) continue;
        gint candidate = leaf_level ? leaf : (gint)block;
        if (anchor < 0 || candidate < anchor) anchor = candidate;
    }
    guint step = 0; gint step_leaf = 0;
    if (!leaf_level && q->live_step && radios_programme_tree_find(q->tree, q->live_step, &step, &step_leaf) && step_leaf < 0 &&
        (anchor < 0 || (gint)step < anchor)) anchor = (gint)step;
    return anchor;
}
static GArray *plan_events(Queue *q)
{
    GArray *plan = g_array_new(FALSE, FALSE, sizeof(RowPlan));
    guint count = q->tree->blocks->len, *mains = g_new(guint, count);
    for (guint i = 0; i < count; ++i) {
        const gchar *kind = text(((RadioSProgrammeBlock *)g_ptr_array_index(q->tree->blocks, i))->block, "kind");
        mains[i] = i;
        if (i && kind_attaches(kind) &&
            kind_is_parent(text(((RadioSProgrammeBlock *)g_ptr_array_index(q->tree->blocks, mains[i - 1]))->block, "kind")))
            mains[i] = mains[i - 1];
    }
    gboolean limited = q->event_show > 0;
    gint anchor = limited ? plan_anchor(q, 0, FALSE) : -1;
    guint transition[2] = {G_MAXUINT, G_MAXUINT}; const gchar *ids[] = {q->transition_source, q->transition_target};
    for (guint k = 0; k < 2; ++k) {
        guint block = 0; gint leaf = -1;
        if (ids[k] && radios_programme_tree_find(q->tree, ids[k], &block, &leaf) && leaf >= 0) transition[k] = block;
    }
    guint shown = 0;
    for (guint start = anchor >= 0 ? mains[anchor] : 0; start < count;) {
        guint end = start + 1; gboolean forced = FALSE, done = TRUE;
        while (end < count && mains[end] == start) ++end;
        for (guint b = start; b < end; ++b) {
            forced |= b == transition[0] || b == transition[1];
            done &= block_done(q, text(((RadioSProgrammeBlock *)g_ptr_array_index(q->tree->blocks, b))->block, "id"));
        }
        if (limited && !forced && (done || shown >= q->event_show)) { start = end; continue; }
        ++shown; plan_add(plan, start, -1, -1, FALSE, shown, 0);
        guint sub = 0; RadioSProgrammeBlock *main = g_ptr_array_index(q->tree->blocks, start);
        for (guint b = start + 1; b < end; ++b) {
            const gchar *id = text(((RadioSProgrammeBlock *)g_ptr_array_index(q->tree->blocks, b))->block, "id");
            if (!limited || b == transition[0] || b == transition[1] || !block_done(q, id)) plan_add(plan, b, -1, -1, TRUE, shown, ++sub);
        }
        /* A voice is the same accepted content in both views, including a
         * music overlay that has no independent programme block. */
        for (guint j = 0; j < main->leaves->len; ++j) {
            JsonObject *leaf = g_ptr_array_index(main->leaves, j); GPtrArray *hints = leaf_hints(q, leaf);
            for (guint h = 0; h < hints->len; ++h) {
                JsonObject *hint = g_ptr_array_index(hints, h);
                if (!g_str_equal(text(hint, "placement"), "overlay") || hint_done(q, leaf, hint, h)) continue;
                plan_add(plan, start, (gint)j, (gint)h, TRUE, shown, ++sub);
            }
            g_ptr_array_unref(hints);
        }
        if (g_str_equal(text(main->block, "kind"), "advertisement"))
            for (guint j = 0; j < main->leaves->len; ++j) {
                const gchar *id = text(g_ptr_array_index(main->leaves, j), "id");
                if (!limited || leaf_in_transition(q, id) || !g_hash_table_contains(q->completed_leaves, id))
                    plan_add(plan, start, (gint)j, -1, TRUE, shown, ++sub);
            }
        start = end;
    }
    g_free(mains);
    return plan;
}
static GArray *plan_contents(Queue *q, guint opened)
{
    GArray *plan = g_array_new(FALSE, FALSE, sizeof(RowPlan));
    RadioSProgrammeBlock *block = g_ptr_array_index(q->tree->blocks, opened);
    if (!block->leaves->len) {
        RowPlan note = {opened, -1, -1, FALSE, FALSE, 1, 0, "Boş event"};
        if (kind_is_live(text(block->block, "kind"))) note.note = NULL; /* the live step is its own row */
        g_array_append_val(plan, note);
        return plan;
    }
    gboolean limited = q->content_show > 0;
    gint anchor = limited ? plan_anchor(q, opened, TRUE) : -1;
    guint shown = 0, main_number = 0;
    for (guint j = anchor > 0 ? (guint)anchor : 0; j < block->leaves->len; ++j) {
        JsonObject *leaf = g_ptr_array_index(block->leaves, j); const gchar *id = text(leaf, "id");
        if (limited && !leaf_in_transition(q, id) && (g_hash_table_contains(q->completed_leaves, id) || shown >= q->content_show)) continue;
        ++shown; ++main_number; plan_add(plan, opened, (gint)j, -1, FALSE, main_number, 0);
        GPtrArray *hints = leaf_hints(q, leaf);
        guint sub = 0;
        for (guint i = 0; i < hints->len; ++i) {
            JsonObject *hint = g_ptr_array_index(hints, i);
            if (hint_done(q, leaf, hint, i) || (limited && shown >= q->content_show && !hint_sounding(q, leaf, hint))) continue;
            ++shown; plan_add(plan, opened, (gint)j, (gint)i, TRUE, main_number, ++sub);
        }
        g_ptr_array_unref(hints);
    }
    return plan;
}
/* NULL when the programme has no events (the list shows "Event yok"). */
static GArray *plan_rows(Queue *q, guint *opened_block, gboolean *contents)
{
    guint block = 0; gint leaf = -1;
    *contents = q->tree && q->opened && radios_programme_tree_find(q->tree, q->opened, &block, &leaf) && leaf < 0;
    *opened_block = block;
    if (!q->tree || !q->tree->blocks->len) return NULL;
    return *contents ? plan_contents(q, block) : plan_events(q);
}
static gchar *plan_key(Queue *q, const RowPlan *item)
{
    if (item->note) return g_strdup(item->note);
    RadioSProgrammeBlock *block = g_ptr_array_index(q->tree->blocks, item->block);
    if (item->leaf < 0) return g_strdup(text(block->block, "id"));
    JsonObject *leaf = g_ptr_array_index(block->leaves, (guint)item->leaf);
    if (item->overlay < 0) return g_strdup(text(leaf, "id"));
    GPtrArray *hints = leaf_hints(q, leaf);
    JsonObject *hint = (guint)item->overlay < hints->len ? g_ptr_array_index(hints, (guint)item->overlay) : NULL;
    gchar *title = hint ? hint_title(hint) : g_strdup("");
    gchar *id = radios_programme_hint_id(leaf, hint, (guint)item->overlay);
    gchar *key = g_strdup_printf("%s\n%s", id, title);
    g_free(id); g_free(title); g_ptr_array_unref(hints);
    return key;
}
/* Rebuilds only when the visible rows would differ; tick updates stay cheap. */
static gboolean window_refresh(Queue *q)
{
    guint opened = 0; gboolean contents = FALSE;
    GArray *plan = plan_rows(q, &opened, &contents);
    GList *rows = gtk_container_get_children(GTK_CONTAINER(q->list)), *item = rows;
    gboolean same = g_list_length(rows) == (plan ? plan->len : 1);
    for (guint i = 0; same && plan && i < plan->len; ++i, item = item->next) {
        gchar *key = plan_key(q, &g_array_index(plan, RowPlan, i));
        same = !g_strcmp0(g_object_get_data(G_OBJECT(item->data), "programme-row-key"), key);
        g_free(key);
    }
    g_list_free(rows); if (plan) g_array_unref(plan);
    if (same) return FALSE;
    queue_rebuild(q);
    return TRUE;
}
/* A live row's subtitle: the run-time label of its channels when the page has
 * set one (radios_programme_queue_set_live_label), else the event's own. */
static gchar *live_meta(Queue *q, const gchar *id, JsonObject *step)
{
    const gchar *label = id ? g_hash_table_lookup(q->live_labels, id) : NULL;
    if (!label) label = text(step, "label");
    const gchar *name = g_str_equal(label, "dj") ? "DJ" : g_str_equal(label, "talk") ? "Konuşma" : g_str_equal(label, "music") ? "Müzik" : NULL;
    return name ? g_strdup_printf("%s · %s", kind_label("live"), name) : g_strdup(kind_label("live"));
}

static void destroy_child(GtkWidget *child, gpointer data) { (void)data; gtk_widget_destroy(child); }
/* Whether the keyboard focus of the queue's window is inside the queue. */
static gboolean focus_inside(Queue *q)
{
    GtkWidget *toplevel = gtk_widget_get_toplevel(q->root);
    GtkWidget *focus = GTK_IS_WINDOW(toplevel) ? gtk_window_get_focus(GTK_WINDOW(toplevel)) : NULL;
    return focus && (focus == q->root || gtk_widget_is_ancestor(focus, q->root));
}
static gboolean rebuild_idle_cb(gpointer data)
{
    Queue *q = data; q->rebuild_idle = 0;
    if (!q->destroyed && q->rebuild_pending && !q->talk_button) queue_rebuild(q);
    return G_SOURCE_REMOVE;
}
static void queue_rebuild(Queue *q)
{
    /* A held R keeps its row and pointer grab: a list change (a finished
     * content leaving, a refreshed programme) waits for the release, so it
     * never cuts a push-to-talk. The state passes still run on the rows. */
    if (q->talk_button && !q->destroyed) {
        q->rebuild_pending = TRUE;
        playing_apply(q); transition_apply(q); live_apply(q); ending_apply(q); overlay_apply(q);
        return;
    }
    q->rebuild_pending = FALSE;
    gboolean focused = focus_inside(q);
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(q->scroll));
    gdouble scroll = gtk_adjustment_get_value(adjustment);
    q->rendering = TRUE; mark_clear(q);
    talk_end(q, TRUE); /* a held R cannot outlive its row */
    gtk_container_foreach(GTK_CONTAINER(q->list), destroy_child, NULL);
    GtkWidget *selected = NULL, *main_row = NULL;
    guint opened_block = 0; gboolean contents = FALSE;
    GArray *plan = plan_rows(q, &opened_block, &contents);
    gtk_widget_set_visible(q->navigation, contents);
    gtk_label_set_text(GTK_LABEL(q->column), contents ? "Parça" : "Event");
    set_class(q->root, "programme-content-view", contents);
    if (contents) {
        RadioSProgrammeBlock *block = g_ptr_array_index(q->tree->blocks, opened_block);
        gtk_label_set_text(GTK_LABEL(q->title), text(block->block, "title"));
    }
    if (!plan) gtk_container_add(GTK_CONTAINER(q->list), note_row("Event yok", "programme-empty-row", NULL, 0));
    for (guint n = 0; plan && n < plan->len; ++n) {
        const RowPlan *item = &g_array_index(plan, RowPlan, n);
        RadioSProgrammeBlock *block = g_ptr_array_index(q->tree->blocks, item->block);
        const gchar *id = text(block->block, "id"), *kind = text(block->block, "kind");
        if (item->note) { gtk_container_add(GTK_CONTAINER(q->list), note_row(item->note, "programme-empty-block-row", id, item->block)); continue; }
        gchar *position = item->sub ? g_strdup_printf("%u.%u", item->main_number, item->sub_number) : g_strdup_printf("%u", item->main_number);
        GtkWidget *row = NULL; const gchar *match = NULL;
        if (item->leaf < 0) {
            gboolean live = kind_is_live(kind); JsonObject *step = block_live(block->block); gint64 live_ms = number(step, "duration_ms");
            gchar *eyebrow = g_strdup_printf("Event · %s", kind_label(kind));
            gchar *meta = live ? live_meta(q, id, step) : g_strdup_printf("%s · %u parça / içerik", kind_label(kind), block->leaves->len);
            RowSpec spec = {.id = id, .number = position, .time = *text(block->block, "start_time") ? text(block->block, "start_time") : "—:—",
                .eyebrow = eyebrow, .title = text(block->block, "title"), .meta = meta, .kind = kind, .event_id = id,
                .duration = live ? duration_text(live_ms, live_ms > 0) : duration_text(radios_programme_block_duration(block), block->leaves->len > 0),
                .block = TRUE, .playable = block->leaves->len > 0 && !live, .waveform = !live, .sub = item->sub, .last = item->last, .live = live,
                .block_index = item->block, .leaf_index = -1};
            row = row_new(q, &spec); match = id;
            g_free(eyebrow); g_free(meta); g_free(spec.duration);
        } else if (item->overlay < 0) {
            JsonObject *leaf = g_ptr_array_index(block->leaves, (guint)item->leaf);
            const gchar *leaf_kind = text(leaf, "kind");
            gchar *leaf_meta = leaf_meta_text(q, leaf);
            gint64 milliseconds = number(leaf, "duration_ms");
            RowSpec spec = {.id = text(leaf, "id"), .number = position, .time = *text(leaf, "start_time") ? text(leaf, "start_time") : "—:—",
                .eyebrow = *text(leaf, "artist") ? text(leaf, "artist") : kind_label(leaf_kind), .title = text(leaf, "title"), .meta = leaf_meta,
                .kind = leaf_kind, .event_id = id, .duration = duration_text(milliseconds, milliseconds > 0),
                .block = FALSE, .playable = leaf_bound(leaf), .waveform = leaf_bound(leaf), .sub = item->sub, .last = item->last,
                .block_index = item->block, .leaf_index = item->leaf};
            row = row_new(q, &spec); match = text(leaf, "id");
            g_free(leaf_meta); g_free(spec.duration);
        } else {
            JsonObject *leaf = g_ptr_array_index(block->leaves, (guint)item->leaf);
            GPtrArray *hints = leaf_hints(q, leaf);
            if ((guint)item->overlay < hints->len) {
                JsonObject *hint = g_ptr_array_index(hints, (guint)item->overlay);
                gchar *overlay_id = radios_programme_hint_id(leaf, hint, (guint)item->overlay);
                gchar *title = hint_title(hint), *relation = hint_relation(hint);
                gint64 milliseconds = number(hint, "duration_ms");
                RowSpec spec = {.id = overlay_id, .number = position, .time = "—:—", .eyebrow = kind_label(hint_kind(hint)), .title = title,
                    .meta = relation, .kind = hint_kind(hint), .event_id = id, .select_id = text(leaf, "id"), .overlay_title = title,
                    .duration = duration_text(milliseconds, milliseconds > 0), .sub = TRUE, .last = item->last,
                    .block_index = item->block, .leaf_index = item->leaf};
                row = row_new(q, &spec);
                g_free(overlay_id); g_free(title); g_free(relation); g_free(spec.duration);
            }
            g_ptr_array_unref(hints);
        }
        if (row) {
            gtk_container_add(GTK_CONTAINER(q->list), row);
            if (match && q->selected && g_str_equal(q->selected, match)) selected = row;
            /* A main row followed by sub-rows carries the top of their branch. */
            if (!item->sub) main_row = row;
            else if (main_row && !g_object_get_data(G_OBJECT(main_row), "programme-branch"))
                branch_set(main_row, BRANCH_MAIN, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "programme-branch-rgb")));
        }
        g_free(position);
    }
    if (plan) g_array_unref(plan);
    gtk_widget_show_all(q->list);
    if (selected) { gtk_list_box_select_row(GTK_LIST_BOX(q->list), GTK_LIST_BOX_ROW(selected)); add_class(selected, "is-selected"); }
    else gtk_list_box_unselect_all(GTK_LIST_BOX(q->list));
    playing_apply(q); transition_apply(q); live_apply(q); ending_apply(q);
    /* Keyboard focus that was inside the queue stays there when its row or
     * the back button went away, so Esc keeps clearing the selection. */
    if (focused && !focus_inside(q)) {
        GtkWidget *target = selected ? selected : GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(q->list), 0));
        if (target) gtk_widget_grab_focus(target);
    }
    gtk_adjustment_set_value(adjustment, scroll);
    q->rendering = FALSE;
}

/* ---- Tick-sized state passes (proportional to the visible rows) ----------- */
static void overlay_apply(Queue *q)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(q->list));
    for (GList *item = rows; item; item = item->next) {
        GtkWidget *row = item->data; const gchar *title = g_object_get_data(G_OBJECT(row), "programme-overlay-title");
        if (!title) continue;
        gchar *key = g_strdup_printf("%s\n%s", row_select_id(row), title);
        guint state = GPOINTER_TO_UINT(g_hash_table_lookup(q->overlays, row_id(row)));
        if (!state) state = GPOINTER_TO_UINT(g_hash_table_lookup(q->overlays, key));
        if (!state) state = GPOINTER_TO_UINT(g_hash_table_lookup(q->overlays, row_select_id(row)));
        g_free(key);
        GtkWidget *label = g_object_get_data(G_OBJECT(row), "programme-state-label");
        switch (state ? (RadioSPlayoutContentState)(state - 1) : RADIOS_PLAYOUT_CONTENT_NONE) {
        case RADIOS_PLAYOUT_CONTENT_READY: state_apply(label, "ready", "✓ Hazır"); break;
        case RADIOS_PLAYOUT_CONTENT_PLAYING: state_apply(label, "playing", "▶ Çalıyor"); break;
        case RADIOS_PLAYOUT_CONTENT_PAUSED: state_apply(label, "paused", "Ⅱ Duraklatıldı"); break;
        case RADIOS_PLAYOUT_CONTENT_WAITING: state_apply(label, "pending", "Bekliyor"); break;
        default: state_apply(label, "pending", "○ Planlandı"); break;
        }
        set_class(row, "is-playing", state == RADIOS_PLAYOUT_CONTENT_PLAYING + 1);
    }
    g_list_free(rows);
}
/* S reads G (green) on the transition's source and target rows; in the event
 * view on the rows of their events. */
static void transition_apply(Queue *q)
{
    guint blocks[2] = {G_MAXUINT, G_MAXUINT}; const gchar *ids[] = {q->transition_source, q->transition_target};
    for (guint k = 0; k < 2; ++k) {
        guint block = 0; gint leaf = -1;
        if (ids[k] && q->tree && radios_programme_tree_find(q->tree, ids[k], &block, &leaf) && leaf >= 0) blocks[k] = block;
    }
    GList *rows = gtk_container_get_children(GTK_CONTAINER(q->list));
    for (GList *item = rows; item; item = item->next) {
        GtkWidget *row = item->data; const gchar *id = row_id(row);
        if (!id || row_is_overlay(row)) continue;
        guint block = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "programme-block-index"));
        gint leaf = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "programme-leaf-index")) - 1;
        gboolean on = leaf >= 0 ? leaf_in_transition(q, id) : (block == blocks[0] || block == blocks[1]);
        GtkWidget *play = g_object_get_data(G_OBJECT(row), "programme-play-button");
        if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(play)), on ? "G" : "S")) gtk_button_set_label(GTK_BUTTON(play), on ? "G" : "S");
        set_class(play, "is-transition", on);
    }
    g_list_free(rows);
}
static void live_apply(Queue *q)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(q->list));
    for (GList *item = rows; item; item = item->next) {
        GtkWidget *row = item->data, *live = g_object_get_data(G_OBJECT(row), "programme-live-button");
        GtkWidget *talk = g_object_get_data(G_OBJECT(row), "programme-talk-button");
        const gchar *event = row_event_id(row);
        if (!live || !talk || !event) continue;
        gboolean mine = q->live_event && g_str_equal(event, q->live_event);
        set_class(live, "is-pressed", mine && q->live_active);
        set_class(talk, "is-pressed", mine && q->talk_held);
        gboolean available = q->talk_event && g_str_equal(event, q->talk_event);
        if (!available && q->talk_button == talk) talk_end(q, TRUE);
        gtk_widget_set_sensitive(talk, available);
    }
    g_list_free(rows);
}
/* Only the ending event's own row blinks (and the opened event's title bar
 * when that event is open); its sub-rows and other rows stay steady. */
static void ending_apply(Queue *q)
{
    gboolean on = q->ending_event && q->ending_timer && q->ending_phase;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(q->list));
    for (GList *item = rows; item; item = item->next) {
        GtkWidget *row = item->data; const gchar *id = row_id(row);
        set_class(row, "is-ending", on && id && row_is_block(row) && g_str_equal(id, q->ending_event));
    }
    g_list_free(rows);
    set_class(q->navigation, "is-ending", on && q->opened && g_str_equal(q->opened, q->ending_event));
}
static gboolean ending_tick(gpointer data)
{
    Queue *q = data; q->ending_phase = !q->ending_phase; ending_apply(q);
    return G_SOURCE_CONTINUE;
}
static void ending_clear(Queue *q)
{
    if (q->ending_timer) { g_source_remove(q->ending_timer); q->ending_timer = 0; }
    g_clear_pointer(&q->ending_event, g_free); q->ending_phase = FALSE;
    if (!q->destroyed) ending_apply(q);
}

static void select_id(Queue *q, const gchar *id, gboolean notify)
{
    if (g_strcmp0(q->selected, id)) { g_free(q->selected); q->selected = g_strdup(id); }
    GList *rows = gtk_container_get_children(GTK_CONTAINER(q->list));
    q->rendering = TRUE;
    for (GList *item = rows; item; item = item->next) {
        gboolean match = id && row_id(item->data) && g_str_equal(row_id(item->data), id);
        set_class(item->data, "is-selected", match);
        if (match) gtk_list_box_select_row(GTK_LIST_BOX(q->list), item->data);
    }
    q->rendering = FALSE; g_list_free(rows);
    if (notify && q->hooks.selected) q->hooks.selected(q->hooks.data, id);
}
static gboolean deselect_idle_cb(gpointer data)
{
    Queue *q = data; q->deselect_idle = 0; q->deselect_row = NULL;
    return G_SOURCE_REMOVE;
}
/* PDF s.8: the header counter shows the selected item's duration only while
 * something is selected. Clearing the selection tells the page with NULL, and
 * the page shows the sounding content's remaining time again. When a click on
 * the selected row cleared it, the list box selects that row again as the
 * same release reaches it; row_selected/row_activated drop that until the
 * click is over. */
static void selection_clear(Queue *q, GtkWidget *clicked)
{
    gboolean notify = q->selected != NULL, rendering = q->rendering;
    select_id(q, NULL, FALSE);
    q->rendering = TRUE; gtk_list_box_unselect_all(GTK_LIST_BOX(q->list)); q->rendering = rendering;
    if (clicked) {
        q->deselect_row = clicked;
        if (!q->deselect_idle) q->deselect_idle = g_idle_add(deselect_idle_cb, q);
    }
    if (notify && q->hooks.selected) q->hooks.selected(q->hooks.data, NULL);
}

static gboolean open_selected(gpointer data)
{
    Queue *q = data; q->expand_idle = 0;
    if (q->selected && q->tree) {
        guint block = 0; gint leaf = -1;
        if (radios_programme_tree_find(q->tree, q->selected, &block, &leaf) && leaf < 0) {
            g_free(q->opened); q->opened = g_strdup(q->selected);
            GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(q->scroll));
            gtk_adjustment_set_value(adjustment, 0);
            queue_rebuild(q);
        }
    }
    return G_SOURCE_REMOVE;
}
static void row_selected(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    Queue *q = data;
    if (q->rendering || q->destroyed) return;
    if (row && GTK_WIDGET(row) == q->deselect_row) {
        q->rendering = TRUE; gtk_list_box_unselect_row(list, row); q->rendering = FALSE;
        return;
    }
    /* The list box's own unselect (Ctrl+Space, Ctrl+Shift+A) clears it too. */
    if (!row) { selection_clear(q, NULL); return; }
    if (!row_id(GTK_WIDGET(row))) return;
    gchar *id = g_strdup(row_id(GTK_WIDGET(row)));
    select_id(q, id, TRUE);
    g_free(id);
}

static void row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    (void)list; Queue *q = data;
    if (q->rendering || !row || !row_id(GTK_WIDGET(row)) || GTK_WIDGET(row) == q->deselect_row) return;
    gchar *id = g_strdup(row_id(GTK_WIDGET(row)));
    select_id(q, id, TRUE);
    if (row_is_block(GTK_WIDGET(row)) && !q->drag_id) open_schedule(q);
    g_free(id);
}

static void overview_clicked(GtkButton *button, gpointer data)
{
    (void)button; Queue *q = data;
    if (q->expand_idle) { g_source_remove(q->expand_idle); q->expand_idle = 0; }
    gchar *id = g_strdup(q->opened);
    g_clear_pointer(&q->opened, g_free);
    g_free(q->selected); q->selected = g_strdup(id);
    queue_rebuild(q);
    select_id(q, id, TRUE);
    g_free(id);
}
/* Esc with the keyboard focus anywhere in the queue clears the selection. */
static gboolean queue_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)widget; Queue *q = data;
    if (event->keyval != GDK_KEY_Escape || (event->state & gtk_accelerator_get_default_mod_mask()) || !q->selected) return FALSE;
    selection_clear(q, NULL);
    return TRUE;
}
/* A click on the list's empty area (below the rows, or on a note row) clears
 * the selection; clicks on rows arrive here from their own windows. */
static gboolean list_button_release(GtkWidget *list, GdkEventButton *event, gpointer data)
{
    Queue *q = data;
    if (event->button != 1 || q->drag_id || gtk_get_event_widget((GdkEvent *)event) != list) return FALSE;
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), (gint)event->y);
    if (!row || !row_id(GTK_WIDGET(row))) selection_clear(q, NULL);
    return FALSE;
}

/* ---- Drop target ------------------------------------------------------- */
static RadioSDragInfo target_info(GdkAtom target)
{
    for (guint i = 0; i < G_N_ELEMENTS(targets); ++i)
        if (target == gdk_atom_intern_static_string(targets[i].target)) return (RadioSDragInfo)targets[i].info;
    return 0;
}
static RadioSProgrammePlacement placement_at(Queue *q, RadioSDragInfo info, gint y, GtkWidget **mark, const gchar **style)
{
    guint blocks = q->tree ? q->tree->blocks->len : 0;
    RadioSProgrammePlacement placement = {-1, blocks};
    guint opened_block = 0; gint opened_leaf = -1;
    gboolean opened = q->tree && q->opened &&
        radios_programme_tree_find(q->tree, q->opened, &opened_block, &opened_leaf) && opened_leaf < 0;
    gboolean bundle = info == RADIOS_DRAG_INFO_PROGRAMME || info == RADIOS_DRAG_INFO_PLAYLIST;
    gboolean block_level = (bundle && !opened) || (info == RADIOS_DRAG_INFO_ROW && q->drag_block);
    if (!block_level && opened) {
        placement.block = (gint)opened_block;
        placement.position = ((RadioSProgrammeBlock *)g_ptr_array_index(q->tree->blocks, opened_block))->leaves->len;
    }
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(q->list), y);
    *mark = NULL; *style = NULL;
    if (!row || !q->tree) return placement;
    gboolean empty = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "programme-empty-block"));
    if (!row_id(GTK_WIDGET(row)) && !empty) return placement;
    guint block = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "programme-block-index"));
    gint leaf = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "programme-leaf-index")) - 1;
    if (block >= blocks) return placement;
    GtkAllocation allocation; gtk_widget_get_allocation(GTK_WIDGET(row), &allocation);
    gboolean before = y < allocation.y + allocation.height / 2;
    *mark = GTK_WIDGET(row);
    if (block_level) {
        if (bundle) before = FALSE;
        placement.block = -1; placement.position = block + (before ? 0 : 1);
        *style = before ? "is-drop-before" : "is-drop-after";
    } else if (empty || leaf < 0) {
        placement.block = (gint)block;
        placement.position = ((RadioSProgrammeBlock *)g_ptr_array_index(q->tree->blocks, block))->leaves->len;
        *style = "is-drop-into";
    } else {
        placement.block = (gint)block; placement.position = (guint)leaf + (before ? 0 : 1);
        *style = before ? "is-drop-before" : "is-drop-after";
    }
    return placement;
}
static void autoscroll(Queue *q, gint y)
{
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(q->scroll));
    gdouble value = gtk_adjustment_get_value(adjustment), page = gtk_adjustment_get_page_size(adjustment);
    if (y < value + 28) gtk_adjustment_set_value(adjustment, value - 18);
    else if (y > value + page - 28) gtk_adjustment_set_value(adjustment, value + 18);
}
static gboolean drag_motion(GtkWidget *list, GdkDragContext *context, gint x, gint y, guint time, gpointer data)
{
    (void)x; Queue *q = data;
    GdkAtom target = gtk_drag_dest_find_target(list, context, NULL);
    RadioSDragInfo info = target != GDK_NONE ? target_info(target) : 0;
    if (!info) { gdk_drag_status(context, 0, time); return FALSE; }
    GtkWidget *mark; const gchar *style;
    placement_at(q, info, y, &mark, &style);
    if (mark != q->mark || style != q->mark_class) {
        mark_clear(q);
        if (mark) { q->mark = mark; q->mark_class = style; add_class(mark, style); g_object_add_weak_pointer(G_OBJECT(mark), (gpointer *)&q->mark); }
    }
    set_class(q->root, "is-drop-target", mark == NULL);
    autoscroll(q, y);
    gdk_drag_status(context, info == RADIOS_DRAG_INFO_ROW ? GDK_ACTION_MOVE : GDK_ACTION_COPY, time);
    return TRUE;
}
static void drag_leave(GtkWidget *list, GdkDragContext *context, guint time, gpointer data)
{ (void)list; (void)context; (void)time; mark_clear(data); }
static gboolean drag_drop(GtkWidget *list, GdkDragContext *context, gint x, gint y, guint time, gpointer data)
{
    (void)x; (void)y; (void)data;
    GdkAtom target = gtk_drag_dest_find_target(list, context, NULL);
    if (target == GDK_NONE) return FALSE;
    gtk_drag_get_data(list, context, target, time);
    return TRUE;
}
static void drag_received(GtkWidget *list, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection,
    guint info, guint time, gpointer data)
{
    (void)list; (void)x; Queue *q = data; GtkWidget *mark; const gchar *style;
    RadioSProgrammePlacement placement = placement_at(q, (RadioSDragInfo)info, y, &mark, &style);
    mark_clear(q);
    gchar *payload = radios_drag_payload(selection);
    gboolean accepted = payload && q->hooks.drop && q->hooks.drop(q->hooks.data, (RadioSDragInfo)info, payload, placement);
    g_free(payload);
    gtk_drag_finish(context, accepted, FALSE, time);
    g_clear_pointer(&q->drag_id, g_free);
}

/* ---- Construction ------------------------------------------------------ */
static void queue_free(gpointer data)
{
    Queue *q = data;
    if (q->expand_idle) g_source_remove(q->expand_idle);
    if (q->ending_timer) g_source_remove(q->ending_timer);
    if (q->rebuild_idle) g_source_remove(q->rebuild_idle);
    if (q->deselect_idle) g_source_remove(q->deselect_idle);
    if (q->talk_button) g_object_remove_weak_pointer(G_OBJECT(q->talk_button), (gpointer *)&q->talk_button);
    if (q->config) json_object_unref(q->config);
    radios_programme_tree_free(q->tree); radios_programme_tree_free(q->run_tree);
    g_hash_table_unref(q->event_runs); g_hash_table_unref(q->event_run_completed);
    g_hash_table_unref(q->completed); g_object_unref(q->row_style); g_object_unref(q->progress_style);
    g_hash_table_unref(q->completed_leaves); g_hash_table_unref(q->overlays); g_hash_table_unref(q->completed_overlays);
    g_hash_table_unref(q->players); for (guint i = 0; i < G_N_ELEMENTS(q->player_names); ++i) g_free(q->player_names[i]);
    g_free(q->transition_source); g_free(q->transition_target); g_free(q->ending_event); g_free(q->live_event); g_free(q->talk_event);
    g_free(q->live_step); g_hash_table_unref(q->live_labels);
    g_free(q->opened); g_free(q->selected); g_free(q->playing); g_free(q->next); g_free(q->drag_id); g_free(q);
}
static void queue_destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; Queue *q = data; q->destroyed = TRUE;
    if (q->expand_idle) { g_source_remove(q->expand_idle); q->expand_idle = 0; }
    if (q->ending_timer) { g_source_remove(q->ending_timer); q->ending_timer = 0; }
    talk_end(q, FALSE);
    if (q->rebuild_idle) { g_source_remove(q->rebuild_idle); q->rebuild_idle = 0; }
    if (q->deselect_idle) { g_source_remove(q->deselect_idle); q->deselect_idle = 0; }
    q->deselect_row = NULL;
}
static GtkWidget *head_cell(GtkWidget *head, const gchar *value, gint width, gfloat align, gboolean expand)
{
    GtkWidget *label = radios_ui_label(value, NULL); gtk_label_set_xalign(GTK_LABEL(label), align);
    if (width > 0) gtk_widget_set_size_request(label, width, -1);
    pack(head, label, expand); return label;
}
GtkWidget *radios_programme_queue_new(const RadioSProgrammeQueueHooks *hooks)
{
    Queue *q = g_new0(Queue, 1); q->hooks = *hooks;
    q->completed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    q->players = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    q->event_runs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_object_unref);
    q->event_run_completed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_object_unref);
    q->completed_leaves = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    q->overlays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    q->completed_overlays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    q->live_labels = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    q->row_style = gtk_css_provider_new(); q->progress_style = gtk_css_provider_new();
    const gchar *css =
        /* Five controls share the column; sub-rows indent with a small number;
         * jingle/sweeper blue and advertisement purple mark the kind only. */
        ".broadcast-row-actions button { min-width: 20px; padding: 0 3px; }"
        ".broadcast-row-actions button.play.is-transition { border-color: #3fa65b; color: #8ee6a0; background-image: none; background-color: #12301a; }"
        ".broadcast-row-actions button.live.is-pressed { border-color: #d1b06b; color: #ffce69; background-image: none; background-color: #302616; }"
        ".broadcast-row-actions button.talk.is-pressed, .broadcast-row-actions button.talk.is-held {"
        " border-color: #e06c6c; color: #ff9c9c; background-image: none; background-color: #3a1616; }"
        "row.programme-sub-row .programme-row-content { margin-left: 10px; }"
        "row.programme-sub-row button.programme-block-number { min-height: 26px; background-image: none; background-color: #262626; }"
        "row.programme-sub-row button.programme-block-number label { font-size: 15px; }"
        "row.programme-sub-row .ld-item-title { font-size: 15px; font-weight: 500; }"
        "row.ld-row.programme-kind-jingle label.broadcast-queue-artist { color: #5fb8ff; }"
        "row.ld-row.programme-kind-advertisement label.broadcast-queue-artist { color: #b48cff; }"
        "row.programme-kind-jingle button.programme-block-number, row.programme-kind-jingle button.programme-leaf-number { border-color: #2f6f9f; }"
        "row.programme-kind-advertisement button.programme-block-number, row.programme-kind-advertisement button.programme-leaf-number { border-color: #6d4fa8; }"
        ".ld-row.is-ending { background-image: none; background-color: #4d3511; }"
        ".programme-queue-navigation.is-ending { background-color: #4d3511; }"
        ".ld-row.is-upcoming { background-image: none; box-shadow: none; transition: none; }"
        ".ld-row.is-upcoming box, .ld-row.is-upcoming eventbox, .ld-row.is-upcoming label {"
        " background-image: none; background-color: transparent; }"
        ".ld-row.is-upcoming-dark label { color: #fff; }"
        ".ld-row.is-upcoming-light label { color: #111; }"
        /* The kind accent outranks the next row's black/white text in every
         * state (next, selected, playing); on the light phase it darkens so
         * it stays readable on the brightening row. */
        "row.ld-row.is-upcoming-light.programme-kind-jingle label.broadcast-queue-artist { color: #164b80; }"
        "row.ld-row.is-upcoming-light.programme-kind-advertisement label.broadcast-queue-artist { color: #4c2a8f; }"
        ".ld-row.is-upcoming button {"
        " background-image: linear-gradient(to bottom, #333, #1f1f1f); color: #ededed; border-color: #555; }"
        ".ld-row.is-upcoming button label, .ld-row.is-upcoming-light button label { color: #ededed; }"
        ".ld-state--completed { color: #8b969e; }"
        ".ld-row-player { color: #9fb7c6; font-size: 11px; }";
    gtk_css_provider_load_from_data(q->row_style, css, -1, NULL);
    gtk_css_provider_load_from_data(q->progress_style,
        ".ld-row.is-upcoming { background-image: none; background-color: #000; transition: none; }", -1, NULL);
    q->root = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, "ld-table-wrap programme-queue-list");
    g_signal_connect(q->root, "destroy", G_CALLBACK(queue_destroyed), q);
    g_signal_connect(q->root, "key-press-event", G_CALLBACK(queue_key_press), q);
    gtk_widget_set_name(q->root, "programme-queue-table");
    q->navigation = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "programme-queue-navigation");
    GtkWidget *back = radios_ui_button("‹ Eventler"); gtk_widget_set_name(back, "programme-queue-back");
    g_signal_connect(back, "clicked", G_CALLBACK(overview_clicked), q);
    pack(q->navigation, back, FALSE);
    q->title = line("", "ld-item-title"); pack(q->navigation, q->title, TRUE);
    gtk_style_context_add_provider(gtk_widget_get_style_context(q->navigation),
        GTK_STYLE_PROVIDER(q->row_style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 20);
    gtk_widget_show_all(q->navigation); gtk_widget_set_no_show_all(q->navigation, TRUE);
    pack(q->root, q->navigation, FALSE);
    GtkWidget *head = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 0, "programme-queue-head");
    head_cell(head, "No", NUMBER_WIDTH, 0.5f, FALSE); q->column = head_cell(head, "Event", -1, 0, TRUE);
    head_cell(head, "Süre", DURATION_WIDTH, 1.0f, FALSE); head_cell(head, "Kontroller", CONTROLS_WIDTH, 0, FALSE);
    pack(q->root, head, FALSE);
    q->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(q->scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(q->scroll, TRUE); gtk_widget_set_size_request(q->scroll, -1, 160);
    q->list = gtk_list_box_new(); add_class(q->list, "ld-table");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(q->list), GTK_SELECTION_SINGLE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(q->list), TRUE);
    g_signal_connect(q->list, "row-selected", G_CALLBACK(row_selected), q);
    g_signal_connect(q->list, "row-activated", G_CALLBACK(row_activated), q);
    g_signal_connect(q->list, "button-release-event", G_CALLBACK(list_button_release), q);
    gtk_drag_dest_set(q->list, 0, targets, G_N_ELEMENTS(targets), GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(q->list, "drag-motion", G_CALLBACK(drag_motion), q);
    g_signal_connect(q->list, "drag-leave", G_CALLBACK(drag_leave), q);
    g_signal_connect(q->list, "drag-drop", G_CALLBACK(drag_drop), q);
    g_signal_connect(q->list, "drag-data-received", G_CALLBACK(drag_received), q);
    gtk_container_add(GTK_CONTAINER(q->scroll), q->list);
    pack(q->root, q->scroll, TRUE);
    g_object_set_data(G_OBJECT(q->root), "list-box", q->list);
    g_object_set_data_full(G_OBJECT(q->root), "radios-programme-queue", q, queue_free);
    queue_rebuild(q);
    return q->root;
}

void radios_programme_queue_render(GtkWidget *queue, JsonObject *config, const gchar *selected)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    q->hide_hints = FALSE;
    if (q->config) json_object_unref(q->config);
    q->config = config ? json_object_ref(config) : NULL;
    radios_programme_tree_free(q->tree); q->tree = config ? radios_programme_tree_new(config) : NULL;
    /* Removed events cannot retain completion state if the draft is rebuilt. */
    GHashTableIter completed; gpointer completed_id;
    g_hash_table_iter_init(&completed, q->completed);
    while (g_hash_table_iter_next(&completed, &completed_id, NULL)) {
        gint leaf = -1;
        if (!q->tree || !radios_programme_tree_find(q->tree, completed_id, NULL, &leaf) || leaf >= 0)
            g_hash_table_iter_remove(&completed);
    }
    g_hash_table_iter_init(&completed, q->completed_leaves);
    while (g_hash_table_iter_next(&completed, &completed_id, NULL)) {
        gint leaf = -1;
        if (!q->tree || !radios_programme_tree_find(q->tree, completed_id, NULL, &leaf) || leaf < 0)
            g_hash_table_iter_remove(&completed);
    }
    GHashTable *runtime_tables[] = {q->event_runs, q->event_run_completed};
    for (guint i = 0; i < G_N_ELEMENTS(runtime_tables); ++i) {
        g_hash_table_iter_init(&completed, runtime_tables[i]);
        while (g_hash_table_iter_next(&completed, &completed_id, NULL)) {
            gint leaf = -1;
            if (!q->tree || !radios_programme_tree_find(q->tree, completed_id, NULL, &leaf) || leaf >= 0)
                g_hash_table_iter_remove(&completed);
        }
    }
    if (g_strcmp0(q->selected, selected)) { g_free(q->selected); q->selected = g_strdup(selected); }
    /* A selected content keeps its event's flat list visible. */
    guint block = 0; gint leaf = -1;
    if (q->tree && selected && radios_programme_tree_find(q->tree, selected, &block, &leaf) && leaf >= 0) {
        g_free(q->opened); q->opened = g_strdup(text(((RadioSProgrammeBlock *)g_ptr_array_index(q->tree->blocks, block))->block, "id"));
    }
    if (q->opened && (!q->tree || !radios_programme_tree_find(q->tree, q->opened, NULL, NULL)))
        g_clear_pointer(&q->opened, g_free);
    queue_rebuild(q);
}

void radios_programme_queue_set_playing(GtkWidget *queue, const gchar *leaf, gboolean paused)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    if (!g_strcmp0(q->playing, leaf) && q->paused == paused) return;
    RadioSProgrammeTree *running = playback_tree(q);
    if (g_strcmp0(q->playing, leaf) && leaf && running) {
        guint block = 0; gint index = -1;
        if (radios_programme_tree_find(running, leaf, &block, &index) && index >= 0) {
            RadioSProgrammeBlock *owner = g_ptr_array_index(running->blocks, block);
            g_hash_table_remove(q->completed, text(owner->block, "id"));
            g_hash_table_remove(q->completed_leaves, leaf); /* a replay */
        }
    }
    g_free(q->playing); q->playing = g_strdup(leaf); q->paused = paused;
    if (!window_refresh(q)) playing_apply(q);
}

void radios_programme_queue_set_progress(GtkWidget *queue, const gchar *playing,
    const gchar *next, guint64 position_ms, guint64 duration_ms, gboolean paused)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    gboolean changed = g_strcmp0(q->playing, playing) || g_strcmp0(q->next, next);
    radios_programme_queue_set_playing(queue, playing, paused);
    gint playing_leaf = -1, next_leaf = -1; RadioSProgrammeTree *running = playback_tree(q);
    gboolean valid = running && playing && next && g_strcmp0(playing, next) &&
        radios_programme_tree_find(running, playing, NULL, &playing_leaf) && playing_leaf >= 0 &&
        radios_programme_tree_find(running, next, NULL, &next_leaf) && next_leaf >= 0;
    const gchar *upcoming = valid ? next : NULL;
    if (g_strcmp0(q->next, upcoming)) { g_free(q->next); q->next = g_strdup(upcoming); }
    if (!paused || changed) { q->position_ms = position_ms; q->duration_ms = duration_ms; }
    gdouble progress = q->duration_ms ? MIN(1.0, (gdouble)q->position_ms / (gdouble)q->duration_ms) : 0;
    guint grey = q->next ? (guint)(255.0 * progress * progress + 0.5) : 0;
    if (grey != q->progress_grey) {
        q->progress_grey = grey;
        gchar *css = g_strdup_printf(".ld-row.is-upcoming { background-image: none; background-color: rgb(%u,%u,%u); transition: none; }", grey, grey, grey);
        gtk_css_provider_load_from_data(q->progress_style, css, -1, NULL); g_free(css);
    }
    progress_apply(q);
}

void radios_programme_queue_complete(GtkWidget *queue, const gchar *leaf)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed || !q->tree || !leaf) return;
    guint block = 0; gint index = -1;
    RadioSProgrammeTree *running = playback_tree(q);
    if (!running || !radios_programme_tree_find(running, leaf, &block, &index) || index < 0) return;
    RadioSProgrammeBlock *owner = g_ptr_array_index(running->blocks, block);
    JsonObject *finished = g_ptr_array_index(owner->leaves, (guint)index);
    if (!leaf_bound(finished)) return;
    /* The content itself is finished: it leaves a limited content list, and
     * a transition it was the source of ends (the target returns to S). */
    gboolean changed = !g_hash_table_contains(q->completed_leaves, leaf);
    g_hash_table_add(q->completed_leaves, g_strdup(leaf));
    if (q->transition_source && g_str_equal(q->transition_source, leaf)) {
        g_clear_pointer(&q->transition_source, g_free); g_clear_pointer(&q->transition_target, g_free); changed = TRUE;
    }
    /* The mixed flow skips drafts without audio. Completing its last audible
     * leaf therefore also ends an event with a silent metadata-only tail. */
    gboolean last = TRUE;
    for (guint i = (guint)index + 1; i < owner->leaves->len; ++i) {
        JsonObject *after = g_ptr_array_index(owner->leaves, i);
        if (leaf_bound(after)) { last = FALSE; break; }
    }
    const gchar *id = text(owner->block, "id"); gint current_leaf = -1;
    if (last && *id && radios_programme_tree_find(q->tree, id, NULL, &current_leaf) && current_leaf < 0 &&
        !g_hash_table_contains(q->completed, id)) {
        g_hash_table_add(q->completed, g_strdup(id)); changed = TRUE;
        /* COMPLETE is a terminal receipt, not an acknowledgement warning. Keep
         * a stable finished state and stop indicating this leaf as sounding. A
         * later event may already be playing when this receipt is delivered. */
        if (!g_strcmp0(q->playing, leaf)) {
            g_clear_pointer(&q->playing, g_free);
            g_clear_pointer(&q->next, g_free);
            q->paused = FALSE;
        }
        if (q->ending_event && g_str_equal(q->ending_event, id)) ending_clear(q);
    }
    if (!changed) return;
    if (!window_refresh(q)) { playing_apply(q); transition_apply(q); }
}

void radios_programme_queue_set_run_config(GtkWidget *queue, JsonObject *accepted_config)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    RadioSProgrammeTree *frozen = NULL;
    if (accepted_config) {
        JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, accepted_config);
        gchar *serialized = json_to_string(node, FALSE); JsonParser *parser = json_parser_new();
        if (json_parser_load_from_data(parser, serialized, -1, NULL))
            frozen = radios_programme_tree_new(json_node_get_object(json_parser_get_root(parser)));
        g_object_unref(parser); g_free(serialized); json_node_free(node);
    }
    if (q->run_tree && !frozen) q->hide_hints = TRUE;
    else if (frozen) q->hide_hints = FALSE;
    radios_programme_tree_free(q->run_tree); q->run_tree = frozen;
    if (!window_refresh(q)) { meta_apply(q); playing_apply(q); }
}

static JsonObject *event_run_copy(JsonObject *source)
{
    JsonObject *copy = json_object_new();
    const gchar *fields[] = {"origin_block_id", "source", "state", "position_ms", "duration_ms", "cue_in_ms",
        "source_name", "completed", "continuing"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) {
        JsonNode *value = json_object_get_member(source, fields[i]);
        if (value && JSON_NODE_HOLDS_VALUE(value)) json_object_set_member(copy, fields[i], radios_programme_clone(value));
    }
    return copy;
}
void radios_programme_queue_set_event_runs(GtkWidget *queue, JsonArray *runs)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    g_hash_table_remove_all(q->event_runs);
    for (guint i = 0; runs && i < json_array_get_length(runs) && i < 32; ++i) {
        JsonNode *node = json_array_get_element(runs, i);
        if (!node || !JSON_NODE_HOLDS_OBJECT(node)) continue;
        JsonObject *run = json_node_get_object(node);
        const gchar *origin = text(run, "origin_block_id"); gint leaf = -1;
        if (!g_uuid_string_is_valid(origin) || number(run, "source") < 0 || number(run, "source") >= 32 ||
            !q->tree || !radios_programme_tree_find(q->tree, origin, NULL, &leaf) || leaf >= 0) continue;
        if (!event_flag(run, "completed")) g_hash_table_remove(q->event_run_completed, origin);
        g_hash_table_replace(q->event_runs, g_strdup(origin), event_run_copy(run));
    }
    if (!window_refresh(q)) event_runs_apply(q);
}
void radios_programme_queue_complete_event_run(GtkWidget *queue, const gchar *origin, guint source)
{
    Queue *q = queue_get(queue); gint leaf = -1;
    if (!q || q->destroyed || source >= 32 || !origin || !q->tree ||
        !radios_programme_tree_find(q->tree, origin, NULL, &leaf) || leaf >= 0) return;
    JsonObject *active = g_hash_table_lookup(q->event_runs, origin);
    if (active && (number(active, "source") != source || !event_flag(active, "completed"))) return;
    if (g_hash_table_size(q->event_run_completed) >= 64 && !g_hash_table_contains(q->event_run_completed, origin)) {
        GHashTableIter iter; gpointer key, value; const gchar *oldest = NULL; gint64 serial = G_MAXINT64;
        g_hash_table_iter_init(&iter, q->event_run_completed);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            gint64 candidate = number(value, "completed_serial");
            if (candidate < serial) { serial = candidate; oldest = key; }
        }
        if (oldest) g_hash_table_remove(q->event_run_completed, oldest);
    }
    JsonObject *run = active ? radios_programme_clone_object(active) : json_object_new();
    json_object_set_string_member(run, "origin_block_id", origin); json_object_set_int_member(run, "source", source);
    json_object_set_int_member(run, "state", RADIOS_PLAYOUT_FINISHED); json_object_set_boolean_member(run, "completed", TRUE);
    json_object_set_boolean_member(run, "continuing", FALSE);
    json_object_set_int_member(run, "completed_serial", (gint64)++q->event_completion_serial);
    g_hash_table_replace(q->event_run_completed, g_strdup(origin), run);
    if (!window_refresh(q)) event_runs_apply(q);
}

void radios_programme_queue_reset_progress(GtkWidget *queue)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    g_hash_table_remove_all(q->completed); g_hash_table_remove_all(q->players);
    g_hash_table_remove_all(q->event_runs); g_hash_table_remove_all(q->event_run_completed);
    g_hash_table_remove_all(q->completed_leaves); g_hash_table_remove_all(q->overlays); g_hash_table_remove_all(q->completed_overlays);
    g_clear_pointer(&q->transition_source, g_free); g_clear_pointer(&q->transition_target, g_free);
    ending_clear(q);
    q->hide_hints = TRUE;
    radios_programme_tree_free(q->run_tree); q->run_tree = NULL;
    g_clear_pointer(&q->playing, g_free); g_clear_pointer(&q->next, g_free);
    q->paused = FALSE; q->position_ms = q->duration_ms = 0; q->progress_grey = 0;
    gtk_css_provider_load_from_data(q->progress_style,
        ".ld-row.is-upcoming { background-image: none; background-color: #000; transition: none; }", -1, NULL);
    if (!window_refresh(q)) { meta_apply(q); playing_apply(q); transition_apply(q); }
}

void radios_programme_queue_set_player(GtkWidget *queue, const gchar *leaf, guint player)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed || !leaf || !*leaf || player < 1 || player > 32) return;
    if (GPOINTER_TO_UINT(g_hash_table_lookup(q->players, leaf)) == player) return;
    /* Leaf ids are unique per selection; a long run must not grow forever. */
    if (g_hash_table_size(q->players) >= 4096) g_hash_table_remove_all(q->players);
    g_hash_table_replace(q->players, g_strdup(leaf), GUINT_TO_POINTER(player));
    players_apply(q);
}

void radios_programme_queue_set_player_context(GtkWidget *queue, guint default_player, const gchar *const *names)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    q->default_player = default_player <= 32 ? default_player : 0;
    for (guint i = 0; i < G_N_ELEMENTS(q->player_names); ++i) {
        g_free(q->player_names[i]); q->player_names[i] = names && names[i] ? g_strdup(names[i]) : NULL;
    }
    players_apply(q);
}

void radios_programme_queue_set_expanded(GtkWidget *queue, const gchar *block, gboolean expanded)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed || !block) return;
    if (expanded) {
        if (!g_strcmp0(q->opened, block)) return;
        g_free(q->opened); q->opened = g_strdup(block);
    } else {
        if (g_strcmp0(q->opened, block)) return;
        g_clear_pointer(&q->opened, g_free);
    }
    queue_rebuild(q);
}

/* ---- Kılavuz 2026-10-18 · yayın sırası görünümü --------------------------- */
void radios_programme_queue_set_window(GtkWidget *queue, guint event_show, guint content_show)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    if (q->event_show == event_show && q->content_show == content_show) return;
    q->event_show = event_show; q->content_show = content_show;
    window_refresh(q);
}
void radios_programme_queue_set_transition(GtkWidget *queue, const gchar *source_leaf, const gchar *target_leaf)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    if (source_leaf && !*source_leaf) source_leaf = NULL;
    if (target_leaf && !*target_leaf) target_leaf = NULL;
    if (!g_strcmp0(q->transition_source, source_leaf) && !g_strcmp0(q->transition_target, target_leaf)) return;
    g_free(q->transition_source); q->transition_source = g_strdup(source_leaf);
    g_free(q->transition_target); q->transition_target = g_strdup(target_leaf);
    if (!window_refresh(q)) transition_apply(q);
}
void radios_programme_queue_set_ending(GtkWidget *queue, const gchar *event_id, gboolean approaching)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    if (!approaching) {
        /* FALSE for another event leaves the current warning alone. */
        if (!event_id || !*event_id || !g_strcmp0(q->ending_event, event_id)) ending_clear(q);
        return;
    }
    if (!event_id || !*event_id) return;
    if (g_strcmp0(q->ending_event, event_id)) { g_free(q->ending_event); q->ending_event = g_strdup(event_id); }
    if (!q->ending_timer) { q->ending_timer = g_timeout_add(500, ending_tick, q); q->ending_phase = TRUE; }
    ending_apply(q);
}
void radios_programme_queue_set_live(GtkWidget *queue, const gchar *event_id, gboolean live_active, gboolean talk_held)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    if (event_id && !*event_id) event_id = NULL;
    if (!event_id || (!live_active && !talk_held)) {
        if (event_id && g_strcmp0(q->live_event, event_id)) return; /* another event's idle state */
        g_clear_pointer(&q->live_event, g_free); q->live_active = q->talk_held = FALSE;
    } else {
        if (g_strcmp0(q->live_event, event_id)) { g_free(q->live_event); q->live_event = g_strdup(event_id); }
        q->live_active = live_active; q->talk_held = talk_held;
    }
    live_apply(q); playing_apply(q);
}
void radios_programme_queue_set_talk_available(GtkWidget *queue, const gchar *event_id)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    if (event_id && !*event_id) event_id = NULL;
    if (!g_strcmp0(q->talk_event, event_id)) return;
    g_free(q->talk_event); q->talk_event = g_strdup(event_id);
    live_apply(q);
}
void radios_programme_queue_set_overlay(GtkWidget *queue, const gchar *leaf_id, const gchar *title, RadioSPlayoutContentState state)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed || !leaf_id || !*leaf_id) return;
    gchar *clean = title ? g_strstrip(g_strdup(title)) : NULL;
    gchar *key = clean && *clean ? g_strdup_printf("%s\n%s", leaf_id, clean) : g_strdup(leaf_id);
    g_free(clean);
    guint block = 0; gint index = -1;
    JsonObject *leaf = q->tree && radios_programme_tree_find(q->tree, leaf_id, &block, &index) && index >= 0 ?
        g_ptr_array_index(((RadioSProgrammeBlock *)g_ptr_array_index(q->tree->blocks, block))->leaves, (guint)index) : NULL;
    GPtrArray *hints = leaf ? leaf_hints(q, leaf) : NULL;
    for (guint i = 0; hints && i < hints->len; ++i) {
        JsonObject *hint = g_ptr_array_index(hints, i);
        if (title && g_strcmp0(title, text(hint, "title"))) continue;
        gchar *titled = g_strdup_printf("%s\n%s", leaf_id, text(hint, "title")), *id = radios_programme_hint_id(leaf, hint, i);
        guint previous = GPOINTER_TO_UINT(g_hash_table_lookup(q->overlays, titled));
        /* Only a sub-row that really sounded is finished (PDF s.13); a stop
         * or skip before it played keeps it, and a re-play revives it. */
        if (state == RADIOS_PLAYOUT_CONTENT_NONE && g_hash_table_remove(q->overlays, titled) &&
            (previous == RADIOS_PLAYOUT_CONTENT_PLAYING + 1 || previous == RADIOS_PLAYOUT_CONTENT_PAUSED + 1))
            g_hash_table_add(q->completed_overlays, g_strdup(id));
        else if (state != RADIOS_PLAYOUT_CONTENT_NONE) g_hash_table_remove(q->completed_overlays, id);
        g_free(titled); g_free(id);
    }
    if (hints) g_ptr_array_unref(hints);
    if (state == RADIOS_PLAYOUT_CONTENT_NONE) g_hash_table_remove(q->overlays, key);
    else {
        if (g_hash_table_size(q->overlays) >= 4096 && !g_hash_table_contains(q->overlays, key)) g_hash_table_remove_all(q->overlays);
        g_hash_table_replace(q->overlays, g_strdup(key), GUINT_TO_POINTER((guint)state + 1));
    }
    g_free(key);
    if (!window_refresh(q)) overlay_apply(q);
}
void radios_programme_queue_set_overlay_identity(GtkWidget *queue, const gchar *leaf_id,
    const gchar *overlay_id, const gchar *title, RadioSPlayoutContentState state)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    if (!overlay_id || !*overlay_id) { radios_programme_queue_set_overlay(queue, leaf_id, title, state); return; }
    if (state == RADIOS_PLAYOUT_CONTENT_NONE) {
        guint previous = GPOINTER_TO_UINT(g_hash_table_lookup(q->overlays, overlay_id));
        if (g_hash_table_remove(q->overlays, overlay_id) &&
            (previous == RADIOS_PLAYOUT_CONTENT_PLAYING + 1 || previous == RADIOS_PLAYOUT_CONTENT_PAUSED + 1))
            g_hash_table_add(q->completed_overlays, g_strdup(overlay_id));
    } else {
        g_hash_table_remove(q->completed_overlays, overlay_id);
        g_hash_table_replace(q->overlays, g_strdup(overlay_id), GUINT_TO_POINTER((guint)state + 1));
    }
    if (!window_refresh(q)) overlay_apply(q);
}
/* PDF s.8: when the flow reaches a live step the event list shows it as the
 * current row; s.11: a finished step leaves the active list. */
void radios_programme_queue_set_live_step(GtkWidget *queue, const gchar *event_id)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed) return;
    if (event_id && !*event_id) event_id = NULL;
    if (!g_strcmp0(q->live_step, event_id)) return;
    g_free(q->live_step); q->live_step = g_strdup(event_id);
    /* Reaching the step again is a replay: it is no longer finished. */
    if (event_id) g_hash_table_remove(q->completed, event_id);
    if (!window_refresh(q)) playing_apply(q);
}
void radios_programme_queue_complete_live_step(GtkWidget *queue, const gchar *event_id)
{
    Queue *q = queue_get(queue); gint leaf = -1;
    if (!q || q->destroyed || !event_id || !*event_id || !q->tree ||
        !radios_programme_tree_find(q->tree, event_id, NULL, &leaf) || leaf >= 0) return;
    gboolean changed = !g_hash_table_contains(q->completed, event_id);
    if (changed) g_hash_table_add(q->completed, g_strdup(event_id));
    if (!g_strcmp0(q->live_step, event_id)) { g_clear_pointer(&q->live_step, g_free); changed = TRUE; }
    if (q->ending_event && g_str_equal(q->ending_event, event_id)) ending_clear(q);
    if (changed && !window_refresh(q)) playing_apply(q);
}
/* Live rows' subtitles in place: only a text that changes is set. */
static void live_meta_apply(Queue *q)
{
    if (!q->tree) return;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(q->list));
    for (GList *item = rows; item; item = item->next) {
        GtkWidget *row = item->data, *label = g_object_get_data(G_OBJECT(row), "programme-meta-label");
        guint block = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "programme-block-index"));
        if (!label || !row_is_block(row) || block >= q->tree->blocks->len) continue;
        RadioSProgrammeBlock *owner = g_ptr_array_index(q->tree->blocks, block);
        if (!kind_is_live(text(owner->block, "kind")) || g_strcmp0(text(owner->block, "id"), row_id(row))) continue;
        gchar *value = live_meta(q, row_id(row), block_live(owner->block));
        if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), value)) {
            gtk_label_set_text(GTK_LABEL(label), value); gtk_widget_set_tooltip_text(label, value);
        }
        g_free(value);
    }
    g_list_free(rows);
}
void radios_programme_queue_set_live_label(GtkWidget *queue, const gchar *event_id, const gchar *label)
{
    Queue *q = queue_get(queue); if (!q || q->destroyed || !event_id || !*event_id) return;
    if (label && !*label) label = NULL;
    if (!g_strcmp0(g_hash_table_lookup(q->live_labels, event_id), label)) return;
    if (!label) g_hash_table_remove(q->live_labels, event_id);
    else {
        /* Bounded like the other run-time tables. */
        if (g_hash_table_size(q->live_labels) >= 256 && !g_hash_table_contains(q->live_labels, event_id))
            g_hash_table_remove_all(q->live_labels);
        g_hash_table_replace(q->live_labels, g_strdup(event_id), g_strdup(label));
    }
    live_meta_apply(q);
}
RadioSProgrammeRowLevel radios_programme_queue_row_level(GtkListBoxRow *row)
{
    gint level = row ? GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "programme-row-level")) : 0;
    return level ? (RadioSProgrammeRowLevel)(level - 1) : RADIOS_PROGRAMME_ROW_NOTE;
}
const gchar *radios_programme_queue_row_number(GtkListBoxRow *row)
{ return row ? g_object_get_data(G_OBJECT(row), "programme-row-number") : NULL; }
