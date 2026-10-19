#include "radios/rotation_editor.h"
#include "radios/schema_settings_ui.h"
#include "radios/probability_editor.h"
#include "radios/autodj_automation_ui.h"
#include "radios/transition_distribution.h"
#include "radios/audio_ui.h"
#include "radios/data.h"
#include "radios/widgets.h"
#include <stdlib.h>
#include <string.h>

enum { GAP_TRACK, GAP_ARTIST, GAP_ALBUM, GAP_COUNT };
enum { MODE_EOF, MODE_CROSSFADE, MODE_CUE, MODE_BEATMATCH, MODE_COUNT };
enum { SOURCE_ID, SOURCE_NAME, SOURCE_FREQUENCY, SOURCE_COUNT, SOURCE_ORDER, SOURCE_WEIGHT, SOURCE_SHARE, SOURCE_WINDOW, SOURCE_INTERVAL, SOURCE_KIND, SOURCE_TRACKS, SOURCE_COLUMNS };
enum { SONG_ID, SONG_NUMBER, SONG_TITLE, SONG_ARTIST, SONG_ALBUM, SONG_DURATION, SONG_WEIGHT, SONG_SHARE, SONG_WINDOW, SONG_MAX, SONG_ORIGINAL, SONG_COLUMNS };
static const gchar *const gap_keys[] = {"track_gap_minutes", "artist_gap_minutes", "album_gap_minutes"};
static const gchar *const gap_ids[] = {"track-gap", "artist-gap", "album-gap"};
/* Rotation-wide AutoDJ policy: one boundary rule and one channel set per kind. */
#define AUTODJ_KIND_COUNT 4
#define AUTODJ_PLAYER_COUNT 32
static const gchar *const autodj_kind_ids[] = {"music", "advertisement", "jingle", "sweeper"};
static const gchar *const autodj_kind_labels[] = {"Müzik", "Reklam", "Jingle", "Sweeper"};
static const gint autodj_default_modes[] = {MODE_BEATMATCH, MODE_EOF, MODE_CROSSFADE, MODE_CROSSFADE};
static const gint autodj_default_overlaps[] = {8000, 0, 1000, 500};

typedef struct {
    RadioSViewContext *ctx;
    RadioSRotationEditorHooks hooks;
    GtkWidget *root, *name, *total, *pages, *status, *heading, *global_settings, *content_warning, *library_toolbar, *library_footer, *name_row;
    GtkWidget *selection_mode, *global_gaps[GAP_COUNT];
    GtkWidget *playlists_combo, *add, *sources, *songs, *back, *up, *down, *remove;
    GtkWidget *frequency, *count, *weight, *start, *end, *interval, *track_order;
    GtkWidget *random, *advert_random, *sweeper_random, *media_kind, *override, *override_body;
    GtkWidget *source_gaps[GAP_COUNT];
    GtkWidget *source_properties, *source_summary, *song_properties, *song_summary;
    GtkWidget *song_weight, *song_order, *song_frequency, *song_max, *song_start, *song_end, *song_interval;
    GtkWidget *source_menu, *routing_dialog, *create_dialog, *probability_dialog, *autodj_policy_page;
    GtkWidget *autodj_enabled, *autodj_body, *autodj_modes[AUTODJ_KIND_COUNT], *autodj_overlaps[AUTODJ_KIND_COUNT];
    GtkWidget *autodj_sync_percents[AUTODJ_KIND_COUNT];
    GtkWidget *autodj_players[AUTODJ_KIND_COUNT][AUTODJ_PLAYER_COUNT];
    GtkWidget *autodj_player_buttons[AUTODJ_KIND_COUNT], *autodj_popovers[AUTODJ_KIND_COUNT];
    GtkWidget *autodj_player_lists[AUTODJ_KIND_COUNT]; /* popover content: cluster headers + members */
    gboolean autodj_grouping; /* a cluster header is setting its members */
    GtkWidget *schema_page, *schema_body, *step_dialog, *name_caption;
    JsonArray *channels; /* defined mixer strips [{number, name, label, source}] from the parent */
    JsonArray *schemas;
    GtkWidget *soft_end, *relax_gaps, *excluded_kinds[AUTODJ_KIND_COUNT];
    GtkWidget *automation_page, *automation_body, *autodj_preset_notes[AUTODJ_KIND_COUNT];
    GtkListStore *source_store, *song_store;
    JsonNode *config;
    JsonArray *playlists;
    RadioSDataClient *playlist_client, *opened_client;
    GHashTable *draft_fields;
    gchar *selected, *selected_song;
    gboolean updating, closed, valid;
} RotationEditor;

static void render(RotationEditor *editor, gboolean preserve_page);
static void render_source(RotationEditor *editor);
static void sources_render(RotationEditor *editor);
static void songs_render(RotationEditor *editor);
static void changed(GtkWidget *widget, gpointer data);
static void draft_capture(RotationEditor *editor);
static void draft_restore(RotationEditor *editor);
static JsonObject *copy_object(JsonObject *object);
static GtkWidget *probability_widget_new(RotationEditor *editor, gboolean source, gboolean songs);
static void source_autodj(GtkMenuItem *item, gpointer data);
static void step_edit(GtkButton *button, gpointer data);
static void step_add(GtkButton *button, gpointer data);
static void schema_refresh(RotationEditor *editor);
static JsonArray *channel_list(RotationEditor *editor);

static JsonObject *object_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static gboolean bool_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}
static JsonNode *copy_node(JsonNode *node)
{
    if (!node) return NULL;
    gchar *text = json_to_string(node, FALSE);
    JsonParser *parser = json_parser_new();
    gboolean loaded = json_parser_load_from_data(parser, text, -1, NULL);
    g_free(text);
    JsonNode *copy = loaded ? json_parser_steal_root(parser) : NULL;
    g_object_unref(parser); return copy;
}
static RotationEditor *editor_get(GtkWidget *widget)
{ return widget ? g_object_get_data(G_OBJECT(widget), "radios-rotation-editor") : NULL; }
static JsonObject *config_object(RotationEditor *editor)
{ return editor->config && JSON_NODE_HOLDS_OBJECT(editor->config) ? json_node_get_object(editor->config) : NULL; }
static JsonArray *source_array(RotationEditor *editor)
{ return array_member(config_object(editor), "sources"); }
static JsonObject *selected_source(RotationEditor *editor, guint *index)
{
    JsonArray *sources = source_array(editor);
    for (guint i = 0; sources && editor->selected && i < json_array_get_length(sources); ++i) {
        JsonNode *node = json_array_get_element(sources, i);
        if (JSON_NODE_HOLDS_OBJECT(node) && !g_strcmp0(radios_json_string(json_node_get_object(node), "id", ""), editor->selected)) {
            if (index) *index = i;
            return json_node_get_object(node);
        }
    }
    return NULL;
}
static gboolean catalog_current(RotationEditor *editor)
{
    RadioSDataClient *current = radios_data_client_acquire(editor->ctx);
    gboolean same = editor->playlists && (!editor->ctx->data || (editor->playlist_client && radios_data_client_same_identity(current, editor->playlist_client)));
    if (current) radios_data_client_unref(current);
    return same;
}
static JsonObject *playlist_by_id(RotationEditor *editor, const gchar *id)
{
    if (!catalog_current(editor)) return NULL;
    for (guint i = 0; editor->playlists && i < json_array_get_length(editor->playlists); ++i) {
        JsonNode *node = json_array_get_element(editor->playlists, i);
        if (JSON_NODE_HOLDS_OBJECT(node) && !g_strcmp0(radios_json_string(json_node_get_object(node), "_id", ""), id))
            return json_node_get_object(node);
    }
    return NULL;
}

static const gchar *playlist_kind_label(const gchar *purpose)
{
    const gchar *ids[] = {"music", "advert", "advertisement", "jingle", "sweeper", "announcement", "programme", "other"};
    const gchar *titles[] = {"Müzik", "Reklam", "Reklam", "Jingle", "Sweeper", "Anons", "Program", "Diğer"};
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i)
        if (!g_strcmp0(purpose, ids[i])) return titles[i];
    return "Karışık";
}

static const gchar *track_id(JsonObject *track)
{ return radios_json_string(track, "_id", radios_json_string(track, "entry_id", "")); }
static JsonObject *song_rule(JsonObject *source, const gchar *id)
{
    JsonArray *rules = array_member(source, "track_rules");
    for (guint i = 0; id && rules && i < json_array_get_length(rules); ++i) {
        JsonObject *rule = json_array_get_object_element(rules, i);
        if (!g_strcmp0(radios_json_string(rule, "entry_id", ""), id)) return rule;
    }
    return NULL;
}
static JsonObject *selected_song(RotationEditor *editor, guint *position)
{
    JsonObject *source = selected_source(editor, NULL);
    JsonArray *tracks = array_member(playlist_by_id(editor, radios_json_string(source, "playlist_id", "")), "tracks");
    for (guint i = 0; editor->selected_song && tracks && i < json_array_get_length(tracks); ++i) {
        JsonObject *track = json_array_get_object_element(tracks, i);
        if (!g_strcmp0(track_id(track), editor->selected_song)) { if (position) *position = i; return track; }
    }
    return NULL;
}
static const gchar *source_order(JsonObject *source)
{
    const gchar *mode = radios_json_string(source, "track_selection_mode", "");
    return !g_strcmp0(mode, "weighted") ? "Ağırlıklı" : !g_strcmp0(mode, "linear") ? "Doğrusal" :
        bool_member(source, "track_random") ? "Karışık" : "Doğrusal";
}
/* One weighting unit (radyo.pdf 3): equal active weights play the playlists
 * in turn, different weights draw by share. The hidden legacy selector
 * follows the weights whenever a weight is edited. */
static void selection_mode_sync(RotationEditor *editor)
{
    JsonObject *settings = object_member(config_object(editor), "settings"); JsonArray *sources = source_array(editor);
    gint64 first = -1; gboolean differ = FALSE;
    for (guint i = 0; sources && i < json_array_get_length(sources); ++i) {
        gint64 weight = radios_json_int(json_array_get_object_element(sources, i), "weight", 100);
        if (!weight) continue;
        if (first < 0) first = weight; else differ |= weight != first;
    }
    if (!settings) return;
    json_object_set_string_member(settings, "selection_mode", differ ? "weighted" : "linear");
    gboolean updating = editor->updating; editor->updating = TRUE;
    gtk_combo_box_set_active(GTK_COMBO_BOX(editor->selection_mode), differ ? 1 : 0); editor->updating = updating;
}
static gchar *percent_label(gdouble value)
{ return value == (gdouble)(gint64)value ? g_strdup_printf("%%%.0f", value) : g_strdup_printf("%%%.1f", value); }
/* Pay column in planner precedence: a percentage from the probability window
 * wins; NULL means no percentages at this scope, so weights decide. */
static gchar *policy_share(JsonObject *policy, const gchar *scope, const gchar *id)
{
    JsonArray *children = array_member(policy, "children"); JsonObject *rest = NULL; gboolean scoped = FALSE;
    for (guint i = 0; children && i < json_array_get_length(children); ++i) {
        JsonObject *group = json_array_get_object_element(children, i);
        if (!group || g_strcmp0(radios_json_string(group, "scope", ""), scope)) continue;
        JsonArray *ids = array_member(group, "ids"); JsonNode *probability = json_object_get_member(group, "probability");
        gboolean defined = probability && JSON_NODE_HOLDS_VALUE(probability); scoped = TRUE;
        if (!ids || !json_array_get_length(ids)) { rest = group; continue; }
        for (guint j = 0; j < json_array_get_length(ids); ++j) {
            const gchar *member = json_array_get_string_element(ids, j);
            if (member && id && !g_ascii_strcasecmp(member, id)) {
                if (!defined) return g_strdup("Kalan pay");
                gchar *share = percent_label(json_node_get_double(probability)), *text = g_strdup_printf("%s olasılık", share); g_free(share); return text;
            }
        }
    }
    JsonNode *probability = rest ? json_object_get_member(rest, "probability") : NULL;
    if (probability && JSON_NODE_HOLDS_VALUE(probability)) {
        gchar *share = percent_label(json_node_get_double(probability)), *text = g_strdup_printf("Kalanlar %s", share); g_free(share); return text;
    }
    return scoped ? g_strdup("Kalan pay") : NULL;
}
static gchar *clock_window(gint64 start, gint64 end)
{ return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT "–%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT,
    start/60,start%60,end/60,end%60); }
static void source_summary_render(RotationEditor *editor)
{
    JsonObject *source = selected_source(editor, NULL);
    gtk_widget_set_sensitive(editor->source_properties, source != NULL);
    if (!source) { gtk_label_set_text(GTK_LABEL(editor->source_summary), ""); return; }
    JsonObject *playlist = playlist_by_id(editor, radios_json_string(source, "playlist_id", ""));
    JsonArray *tracks = array_member(playlist, "tracks");
    gchar *text = g_strdup_printf("%s · %s · %u parça",
        radios_json_string(playlist, "name", "Kullanılamayan playlist"),
        playlist_kind_label(radios_json_string(playlist, "purpose", "music")),
        tracks ? json_array_get_length(tracks) : 0);
    gtk_label_set_text(GTK_LABEL(editor->source_summary), text); g_free(text);
}

/* The planner reads a rotation's stored transition choices as its fallback;
 * the default ends each track at its end (parça sonu). The editor no longer
 * shows this array: transitions are set in the probability window and on the
 * AutoDJ page, and stored choices stay as they are. */
static JsonArray *default_transitions(void)
{
    JsonArray *transitions = json_array_new();
    for (guint i = 0; i < MODE_CUE + 1; ++i) {
        JsonObject *transition = json_object_new();
        json_object_set_int_member(transition, "mode", i);
        json_object_set_int_member(transition, "weight", i == MODE_EOF ? 100 : 0);
        json_object_set_int_member(transition, "overlap_ms", i == MODE_EOF ? 0 : 3000);
        json_array_add_object_element(transitions, transition);
    }
    return transitions;
}
static JsonObject *default_settings(void)
{
    JsonObject *settings = json_object_new();
    json_object_set_string_member(settings, "selection_mode", "linear");
    for (guint i = 0; i < GAP_COUNT; ++i) json_object_set_int_member(settings, gap_keys[i], 0);
    json_object_set_array_member(settings, "transitions", default_transitions()); return settings;
}
static JsonNode *default_config(void)
{
    JsonObject *config = json_object_new(); gchar *id = g_uuid_string_random();
    json_object_set_string_member(config, "id", id); g_free(id);
    json_object_set_string_member(config, "name", "Yeni rotasyon");
    json_object_set_array_member(config, "sources", json_array_new());
    json_object_set_object_member(config, "settings", default_settings());
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, config); return node;
}
static GtkWidget *named(GtkWidget *widget, const gchar *suffix)
{
    gchar *name = g_strconcat("rotation-editor-", suffix, NULL);
    gtk_widget_set_name(widget, name); g_free(name); return widget;
}
static GtkWidget *label(const gchar *text, const gchar *class_name)
{ return radios_ui_label(text, class_name); }
static GtkWidget *spin(RotationEditor *editor, const gchar *suffix, gint minimum, gint maximum)
{
    GtkWidget *widget = named(gtk_spin_button_new_with_range(minimum, maximum, 1), suffix);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(widget), TRUE);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(widget), 5);
    g_signal_connect(widget, "changed", G_CALLBACK(changed), editor);
    g_signal_connect(widget, "value-changed", G_CALLBACK(changed), editor); return widget;
}
static gint spin_read(GtkWidget *widget)
{ return (gint)g_ascii_strtoll(gtk_entry_get_text(GTK_ENTRY(widget)), NULL, 10); }
static gboolean spin_valid(GtkWidget *widget, gint minimum, gint maximum)
{
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(widget));
    if (!*text) return FALSE;
    for (const gchar *c = text; *c; ++c) if (!g_ascii_isdigit(*c)) return FALSE;
    if (strlen(text) > 6) return FALSE;
    gint64 value = g_ascii_strtoll(text, NULL, 10);
    return value >= minimum && value <= maximum;
}
static void grid_field(GtkWidget *grid, const gchar *text, GtkWidget *widget, gint column, gint row)
{
    gtk_grid_attach(GTK_GRID(grid), label(text, "re-caption"), column * 2, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), widget, column * 2 + 1, row, 1, 1);
}
/* Grid and tab captions must not wrap: a wrapping label reports a one
 * character minimum width, so a tight grid or a notebook tab (which is
 * allocated its minimum) breaks the text one letter per line. */
static GtkWidget *plain(const gchar *text, const gchar *class_name)
{
    GtkWidget *widget = radios_ui_label(text, class_name);
    gtk_label_set_line_wrap(GTK_LABEL(widget), FALSE); return widget;
}
/* Kılavuz §03 "Kümeli ve kümesiz seçim": clustered players are chosen under
 * their cluster (the cluster's own check selects every member), independent
 * players one by one under "Kümesizler"; a saved player that is not present
 * now stays listed while selected, so a choice is never lost silently. The
 * member checks keep their names and the saved autodj_players format. */
static GtkWidget *autodj_card(RotationEditor *editor, guint deck)
{
    gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
    GtkWidget *card = editor->ctx->parent ? g_object_get_data(G_OBJECT(editor->ctx->parent), key) : NULL; g_free(key);
    return card && !gtk_widget_in_destruction(card) ? card : NULL;
}
static void autodj_toggled(GtkWidget *widget, gpointer data);
static void autodj_cluster_toggled(GtkToggleButton *header, gpointer data)
{
    RotationEditor *editor = data; if (editor->updating || editor->autodj_grouping) return;
    guint kind = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(header), "autodj-kind"));
    const gchar *cluster = g_object_get_data(G_OBJECT(header), "autodj-cluster-id");
    gboolean active = gtk_toggle_button_get_active(header) || gtk_toggle_button_get_inconsistent(header);
    GtkWidget *member = NULL; editor->autodj_grouping = TRUE;
    for (guint deck = 0; kind < AUTODJ_KIND_COUNT && deck < AUTODJ_PLAYER_COUNT; ++deck) {
        GtkWidget *card = autodj_card(editor, deck); const gchar *id = card ? g_object_get_data(G_OBJECT(card), "radios-player-cluster-id") : NULL;
        if (!card || !id || g_strcmp0(id, cluster)) continue;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->autodj_players[kind][deck]), active); member = editor->autodj_players[kind][deck];
    }
    editor->autodj_grouping = FALSE;
    if (member) autodj_toggled(member, editor); /* one change for the whole cluster */
}
static GtkWidget *autodj_group_header(RotationEditor *editor, guint kind, guint index, const gchar *cluster, const gchar *title)
{
    GtkWidget *header;
    if (cluster) {
        header = gtk_check_button_new_with_label(title);
        gchar *suffix = g_strdup_printf("autodj-%s-cluster-%u", autodj_kind_ids[kind], index); named(header, suffix); g_free(suffix);
        g_object_set_data(G_OBJECT(header), "autodj-kind", GUINT_TO_POINTER(kind));
        g_object_set_data_full(G_OBJECT(header), "autodj-cluster-id", g_strdup(cluster), g_free);
        g_signal_connect(header, "toggled", G_CALLBACK(autodj_cluster_toggled), editor);
    } else header = plain(title, "re-caption");
    g_object_set_data(G_OBJECT(header), "autodj-group-header", GINT_TO_POINTER(1));
    gtk_box_pack_start(GTK_BOX(editor->autodj_player_lists[kind]), header, FALSE, FALSE, 0); gtk_widget_show(header);
    return header;
}
static void autodj_players_render(RotationEditor *editor)
{
    GtkWidget *cards[AUTODJ_PLAYER_COUNT]; gboolean any_card = FALSE;
    for (guint deck = 0; deck < AUTODJ_PLAYER_COUNT; ++deck) any_card |= (cards[deck] = autodj_card(editor, deck)) != NULL;
    gboolean grouping = editor->autodj_grouping; editor->autodj_grouping = TRUE;
    for (guint kind = 0; kind < AUTODJ_KIND_COUNT; ++kind) {
        GtkWidget *list = editor->autodj_player_lists[kind]; guint selected = 0, position = 0, clusters = 0;
        if (!list) continue;
        GList *children = gtk_container_get_children(GTK_CONTAINER(list));
        for (GList *it = children; it; it = it->next) if (g_object_get_data(G_OBJECT(it->data), "autodj-group-header")) gtk_widget_destroy(it->data);
        g_list_free(children);
        gboolean placed[AUTODJ_PLAYER_COUNT] = {FALSE};
        for (guint deck = 0; deck < AUTODJ_PLAYER_COUNT; ++deck) selected += editor->autodj_players[kind][deck] &&
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->autodj_players[kind][deck]));
        /* Clusters in the order of their first player, then Kümesizler, then saved-but-absent players. */
        for (guint pass = 0; pass < 3; ++pass)
            for (guint first = 0; first < AUTODJ_PLAYER_COUNT; ++first) {
                if (placed[first] || !editor->autodj_players[kind][first]) continue;
                const gchar *cluster = cards[first] ? g_object_get_data(G_OBJECT(cards[first]), "radios-player-cluster-id") : NULL;
                gboolean clustered = cards[first] && cluster && *cluster, independent = cards[first] && !clustered;
                gboolean absent = !cards[first] && (!any_card || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->autodj_players[kind][first])));
                if ((pass == 0 && !clustered) || (pass == 1 && !independent) || (pass == 2 && !absent)) continue;
                GtkWidget *header = NULL; guint members = 0, active = 0;
                if (pass == 0) {
                    const gchar *name = g_object_get_data(G_OBJECT(cards[first]), "radios-player-cluster-name");
                    header = autodj_group_header(editor, kind, clusters++, cluster, name && *name ? name : "Küme");
                } else if (any_card) header = autodj_group_header(editor, kind, 0, NULL, pass == 1 ? "Kümesizler" : "Kayıtlı · şu an kapalı");
                if (header) gtk_box_reorder_child(GTK_BOX(list), header, (gint)position++);
                for (guint deck = first; deck < AUTODJ_PLAYER_COUNT; ++deck) {
                    GtkWidget *check = editor->autodj_players[kind][deck]; if (!check || placed[deck]) continue;
                    const gchar *id = cards[deck] ? g_object_get_data(G_OBJECT(cards[deck]), "radios-player-cluster-id") : NULL;
                    gboolean member = pass == 0 ? cards[deck] && id && !g_strcmp0(id, cluster) :
                        pass == 1 ? cards[deck] && !(id && *id) :
                        !cards[deck] && (!any_card || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check)));
                    if (!member) continue;
                    placed[deck] = TRUE; ++members; active += gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check));
                    const gchar *name = cards[deck] ? g_object_get_data(G_OBJECT(cards[deck]), "radios-player-name") : NULL;
                    gchar *caption = name && *name ? g_strdup(name) : g_strdup_printf("Oynatıcı %u", deck + 1);
                    gtk_button_set_label(GTK_BUTTON(check), caption); g_free(caption);
                    gtk_widget_set_margin_start(check, header ? 18 : 0);
                    gtk_widget_set_no_show_all(check, FALSE); gtk_widget_show(check);
                    gtk_box_reorder_child(GTK_BOX(list), check, (gint)position++);
                }
                if (pass == 0 && header) {
                    gtk_toggle_button_set_inconsistent(GTK_TOGGLE_BUTTON(header), active && active < members);
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(header), members && active == members);
                }
                if (pass != 0) break; /* one header for all independent / absent players */
            }
        for (guint deck = 0; deck < AUTODJ_PLAYER_COUNT; ++deck)
            if (editor->autodj_players[kind][deck] && !placed[deck]) {
                gtk_widget_set_no_show_all(editor->autodj_players[kind][deck], TRUE); gtk_widget_hide(editor->autodj_players[kind][deck]);
            }
        if (editor->autodj_player_buttons[kind]) {
            gchar *caption = g_strdup_printf("%u oynatıcı seçili ▾", selected);
            gtk_button_set_label(GTK_BUTTON(editor->autodj_player_buttons[kind]), caption); g_free(caption);
        }
    }
    editor->autodj_grouping = grouping;
}
static void autodj_popover_shown(GtkWidget *popover, gpointer data)
{ (void)popover; autodj_players_render(data); }
/* Kind distributions (settings.transitions per kind, kılavuz 18.10) replace
 * the kind rule's transition fields once the data service announces them;
 * decided when the AutoDJ page is built, at run time. */
static gboolean kind_distributions(RotationEditor *editor)
{ return editor->automation_page && g_object_get_data(G_OBJECT(editor->automation_page), "radios-kind-distributions") != NULL; }
static void autodj_sensitivity(RotationEditor *editor)
{
    gboolean enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->autodj_enabled));
    gtk_widget_set_sensitive(editor->autodj_body, enabled);
    for (guint kind = 0; kind < AUTODJ_KIND_COUNT; ++kind) {
        const gchar *mode = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->autodj_modes[kind]));
        /* A kind served by designed presets takes its transition type, length
         * and beat sync from the AutoDJ automation page; only its channels
         * stay here, so the same setting never lives in two places. With kind
         * distributions the row's "Geçişler" holds them for every kind. */
        gboolean distributions = kind_distributions(editor);
        gboolean preset = distributions || (editor->automation_body && radios_autodj_automation_kind_uses_presets(editor->automation_body, autodj_kind_ids[kind]));
        GtkWidget *rest = g_object_get_data(G_OBJECT(editor->autodj_sync_percents[kind]), "radios-autodj-sync-rest");
        GtkWidget *fallbacks[] = {editor->autodj_modes[kind], editor->autodj_overlaps[kind], editor->autodj_sync_percents[kind], rest};
        for (guint i = 0; i < G_N_ELEMENTS(fallbacks); ++i) {
            if (!fallbacks[i]) continue;
            gtk_widget_set_no_show_all(fallbacks[i], preset);
            gtk_widget_set_visible(fallbacks[i], !preset);
        }
        gtk_widget_set_sensitive(editor->autodj_modes[kind], !preset);
        gtk_widget_set_sensitive(editor->autodj_overlaps[kind], !preset && enabled && mode && !g_str_equal(mode, "0"));
        gtk_widget_set_sensitive(editor->autodj_sync_percents[kind], !preset && enabled && mode && g_str_equal(mode, "3"));
        /* Kalan pay: the share SYNC leaves crossfades over the same length. */
        if (rest) {
            gint percent = spin_valid(editor->autodj_sync_percents[kind], 0, 100) ? spin_read(editor->autodj_sync_percents[kind]) : 100;
            gchar *text = mode && g_str_equal(mode, "3") && percent < 100 ? g_strdup_printf("%%%d Crossfade", 100 - percent) : g_strdup("");
            gtk_label_set_text(GTK_LABEL(rest), text); g_free(text);
        }
        if (editor->autodj_preset_notes[kind])
            gtk_label_set_text(GTK_LABEL(editor->autodj_preset_notes[kind]), preset && !distributions ? "Ön ayar kullanılıyor (AutoDJ otomasyonu)" : "");
    }
}
static void autodj_toggled(GtkWidget *widget, gpointer data)
{ RotationEditor *editor = data; if (editor->autodj_grouping) return; autodj_sensitivity(editor); changed(widget, data); autodj_players_render(editor); }
static void autodj_rest_changed(GtkWidget *widget, gpointer data)
{ (void)widget; autodj_sensitivity(data); }
static GtkWidget *autodj_page(RotationEditor *editor)
{
    GtkWidget *body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL);
    gtk_container_set_border_width(GTK_CONTAINER(body), 8);
    editor->autodj_enabled = named(gtk_check_button_new_with_label("AutoDJ geçişlerini içerik türüne göre bu ayarlarla yönet"), "autodj-policy-enabled");
    g_signal_connect(editor->autodj_enabled, "toggled", G_CALLBACK(autodj_toggled), editor);
    gtk_box_pack_start(GTK_BOX(body), editor->autodj_enabled, FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new(); editor->autodj_body = grid;
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6); gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    const gchar *columns[] = {"İçerik", "Geçiş", "Süre (ms)", "SYNC olasılığı (%)", "Kalan pay", "Kanallar (oynatıcılar)"};
    /* With kind distributions the transition columns stay empty (hidden). */
    for (guint i = 0; i < G_N_ELEMENTS(columns); ++i)
        if (!kind_distributions(editor) || i == 0 || i == G_N_ELEMENTS(columns) - 1)
            gtk_grid_attach(GTK_GRID(grid), plain(columns[i], "re-caption"), (gint)i, 0, 1, 1);
    const gchar *modes[] = {"Parça sonunda kes", "Ses kısarak geçiş (crossfade)", "Cue noktasından geçiş", "SYNC beatmatch (BPM ve vuruş eşleştirerek)"};
    for (guint kind = 0; kind < AUTODJ_KIND_COUNT; ++kind) {
        gint row = (gint)kind + 1;
        gtk_grid_attach(GTK_GRID(grid), plain(autodj_kind_labels[kind], NULL), 0, row, 1, 1);
        gchar *id = g_strdup_printf("autodj-%s-preset-note", autodj_kind_ids[kind]);
        editor->autodj_preset_notes[kind] = named(plain("", "re-caption"), id); g_free(id);
        gtk_grid_attach(GTK_GRID(grid), editor->autodj_preset_notes[kind], 6, row, 1, 1);
        id = g_strdup_printf("autodj-%s-mode", autodj_kind_ids[kind]);
        editor->autodj_modes[kind] = named(gtk_combo_box_text_new(), id); g_free(id);
        for (guint mode = 0; mode < MODE_COUNT; ++mode) {
            gchar *value = g_strdup_printf("%u", mode);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->autodj_modes[kind]), value, modes[mode]); g_free(value);
        }
        g_signal_connect(editor->autodj_modes[kind], "changed", G_CALLBACK(autodj_toggled), editor);
        gtk_grid_attach(GTK_GRID(grid), editor->autodj_modes[kind], 1, row, 1, 1);
        id = g_strdup_printf("autodj-%s-overlap", autodj_kind_ids[kind]);
        editor->autodj_overlaps[kind] = spin(editor, id, 0, 30000); g_free(id);
        gtk_grid_attach(GTK_GRID(grid), editor->autodj_overlaps[kind], 2, row, 1, 1);
        id = g_strdup_printf("autodj-%s-sync-percent", autodj_kind_ids[kind]);
        editor->autodj_sync_percents[kind] = spin(editor, id, 0, 100); g_free(id);
        /* Default before the editor is complete: no stored-config validation or save may run. */
        { gboolean updating = editor->updating; editor->updating = TRUE;
          gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->autodj_sync_percents[kind]), 100);
          editor->updating = updating; }
        gtk_widget_set_tooltip_text(editor->autodj_sync_percents[kind],
            "SYNC beatmatch seçiliyken bu türdeki geçişlerin yüzde kaçının SYNC beatmatch ile yapılacağı; kalanı aynı sürede ses kısarak geçiştir.");
        gtk_grid_attach(GTK_GRID(grid), editor->autodj_sync_percents[kind], 3, row, 1, 1);
        id = g_strdup_printf("autodj-%s-sync-rest", autodj_kind_ids[kind]);
        GtkWidget *rest = named(plain("", "re-caption"), id); g_free(id);
        g_object_set_data(G_OBJECT(editor->autodj_sync_percents[kind]), "radios-autodj-sync-rest", rest);
        g_signal_connect(editor->autodj_sync_percents[kind], "changed", G_CALLBACK(autodj_rest_changed), editor);
        g_signal_connect(editor->autodj_sync_percents[kind], "value-changed", G_CALLBACK(autodj_rest_changed), editor);
        gtk_grid_attach(GTK_GRID(grid), rest, 4, row, 1, 1);
        GtkWidget *menu = gtk_menu_button_new(); editor->autodj_player_buttons[kind] = menu;
        gtk_button_set_label(GTK_BUTTON(menu), "Oynatıcıları seç…");
        GtkWidget *popover = gtk_popover_new(menu); editor->autodj_popovers[kind] = popover;
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), popover);
        GtkWidget *players = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, NULL); editor->autodj_player_lists[kind] = players;
        g_signal_connect(popover, "show", G_CALLBACK(autodj_popover_shown), editor);
        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_widget_set_size_request(scroll, 280, 220);
        gtk_container_add(GTK_CONTAINER(scroll), players); gtk_container_add(GTK_CONTAINER(popover), scroll);
        for (guint player = 0; player < AUTODJ_PLAYER_COUNT; ++player) {
            gchar *text = g_strdup_printf("Oynatıcı %u", player + 1);
            id = g_strdup_printf("autodj-%s-player-%u", autodj_kind_ids[kind], player + 1);
            editor->autodj_players[kind][player] = named(gtk_check_button_new_with_label(text), id); g_free(id); g_free(text);
            g_signal_connect(editor->autodj_players[kind][player], "toggled", G_CALLBACK(autodj_toggled), editor);
            gtk_box_pack_start(GTK_BOX(players), editor->autodj_players[kind][player], FALSE, FALSE, 0);
        }
        gtk_widget_show_all(scroll);
        gtk_grid_attach(GTK_GRID(grid), menu, 5, row, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
    return body;
}
static void autodj_read(RotationEditor *editor, JsonObject *settings)
{
    if (!editor->autodj_enabled || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->autodj_enabled))) return;
    JsonObject *policy = json_object_new(), *channels = json_object_new();
    JsonObject *previous = object_member(settings, "autodj_players");
    for (guint kind = 0; kind < AUTODJ_KIND_COUNT; ++kind) {
        const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->autodj_modes[kind]));
        gint mode = id ? atoi(id) : MODE_EOF; JsonObject *choice = json_object_new(); JsonArray *decks = json_array_new();
        json_object_set_int_member(choice, "mode", mode);
        json_object_set_int_member(choice, "overlap_ms", mode == MODE_EOF ? 0 : spin_read(editor->autodj_overlaps[kind]));
        if (mode == MODE_BEATMATCH && spin_read(editor->autodj_sync_percents[kind]) < 100)
            json_object_set_int_member(choice, "sync_percent", spin_read(editor->autodj_sync_percents[kind]));
        json_object_set_object_member(policy, autodj_kind_ids[kind], choice);
        guint32 seen = 0; JsonArray *saved = array_member(previous, autodj_kind_ids[kind]);
        for (guint i = 0; saved && i < json_array_get_length(saved); ++i) {
            gint64 player = json_array_get_int_element(saved, i) - 1;
            if (player >= 0 && player < AUTODJ_PLAYER_COUNT && !(seen & ((guint32)1 << player)) &&
                gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->autodj_players[kind][player]))) {
                json_array_add_int_element(decks, player + 1); seen |= (guint32)1 << player;
            }
        }
        for (guint player = 0; player < AUTODJ_PLAYER_COUNT; ++player)
            if (!(seen & ((guint32)1 << player)) && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->autodj_players[kind][player]))) json_array_add_int_element(decks, player + 1);
        json_object_set_array_member(channels, autodj_kind_ids[kind], decks);
    }
    /* With kind distributions the transitions live in settings.transitions. */
    if (kind_distributions(editor)) json_object_unref(policy);
    else json_object_set_object_member(settings, "kind_transitions", policy);
    json_object_set_object_member(settings, "autodj_players", channels);
}
static void autodj_render(RotationEditor *editor, JsonObject *settings)
{
    if (!editor->autodj_enabled) return;
    JsonObject *policy = object_member(settings, "kind_transitions"), *channels = object_member(settings, "autodj_players");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->autodj_enabled), policy != NULL || (kind_distributions(editor) && channels != NULL));
    for (guint kind = 0; kind < AUTODJ_KIND_COUNT; ++kind) {
        JsonObject *choice = object_member(policy, autodj_kind_ids[kind]);
        gint64 mode = choice ? radios_json_int(choice, "mode", autodj_default_modes[kind]) : autodj_default_modes[kind];
        gint64 overlap = choice ? radios_json_int(choice, "overlap_ms", autodj_default_overlaps[kind]) : autodj_default_overlaps[kind];
        gchar *id = g_strdup_printf("%" G_GINT64_FORMAT, mode);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->autodj_modes[kind]), id); g_free(id);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->autodj_overlaps[kind]), mode == MODE_EOF ? autodj_default_overlaps[kind] : overlap);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->autodj_sync_percents[kind]),
            choice && mode == MODE_BEATMATCH ? radios_json_int(choice, "sync_percent", 100) : 100);
        JsonArray *decks = array_member(channels, autodj_kind_ids[kind]);
        guint32 defaults = 0; guint found = 0; const gchar *first_group = NULL;
        for (guint player = 0; !decks && player < AUTODJ_PLAYER_COUNT; ++player) {
            gchar *key = g_strdup_printf("radios-deck-owner-%u", player);
            GtkWidget *card = editor->ctx->parent ? g_object_get_data(G_OBJECT(editor->ctx->parent), key) : NULL; g_free(key);
            if (!card || gtk_widget_in_destruction(card)) continue;
            const gchar *group = g_object_get_data(G_OBJECT(card), "radios-player-cluster-id");
            if (!found) first_group = group;
            if (found < 2 && !g_strcmp0(group, first_group)) { defaults |= (guint32)1 << player; ++found; }
        }
        if (!decks && !found) defaults = 3;
        for (guint player = 0; player < AUTODJ_PLAYER_COUNT; ++player) {
            gboolean on = !decks && (defaults & ((guint32)1 << player));
            for (guint j = 0; decks && j < json_array_get_length(decks); ++j)
                on |= json_array_get_int_element(decks, j) == (gint64)player + 1;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->autodj_players[kind][player]), on);
        }
    }
    autodj_sensitivity(editor); autodj_players_render(editor);
}
static gboolean autodj_valid(RotationEditor *editor)
{
    if (!editor->autodj_enabled || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->autodj_enabled))) return TRUE;
    for (guint kind = 0; kind < AUTODJ_KIND_COUNT; ++kind) {
        const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->autodj_modes[kind]));
        gint mode = id ? atoi(id) : MODE_EOF; gboolean any = FALSE;
        for (guint player = 0; player < AUTODJ_PLAYER_COUNT; ++player) any |= gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->autodj_players[kind][player]));
        gchar *message = NULL;
        if (!any) message = g_strdup_printf("AutoDJ: %s için en az bir kanal (oynatıcı) seçin.", autodj_kind_labels[kind]);
        else if (mode != MODE_EOF && !spin_valid(editor->autodj_overlaps[kind], 0, 30000))
            message = g_strdup_printf("AutoDJ: %s geçiş süresi 0–30000 ms olmalı.", autodj_kind_labels[kind]);
        else if (mode == MODE_BEATMATCH && !spin_read(editor->autodj_overlaps[kind]))
            message = g_strdup_printf("AutoDJ: %s için SYNC beatmatch süresi sıfırdan büyük olmalı.", autodj_kind_labels[kind]);
        else if (mode == MODE_BEATMATCH && !spin_valid(editor->autodj_sync_percents[kind], 0, 100))
            message = g_strdup_printf("AutoDJ: %s için SYNC olasılığı 0–100 olmalı.", autodj_kind_labels[kind]);
        if (message) { gtk_label_set_text(GTK_LABEL(editor->status), message); g_free(message); return FALSE; }
    }
    return TRUE;
}
/* Repeat limits only. Transitions have one home each: the probability window
 * ("Olasılık ve geçişler…") for music-to-music draws and the AutoDJ page for
 * content kinds and channels. */
static GtkWidget *settings_grid(RotationEditor *editor, gboolean source)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6); gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 8);
    GtkWidget **gaps = source ? editor->source_gaps : editor->global_gaps;
    const gchar *captions[] = {"Şarkı tekrarı (dk)", "Sanatçı tekrarı (dk)", "Albüm tekrarı (dk)"};
    for (guint i = 0; i < GAP_COUNT; ++i) {
        gchar *id = g_strdup_printf("%s%s", source ? "source-" : "", gap_ids[i]);
        gaps[i] = spin(editor, id, 0, 10080); g_free(id);
        grid_field(grid, captions[i], gaps[i], (gint)i % 2, (gint)i / 2);
    }
    if (source) return grid;
    /* Optional block-end and gap fallbacks: written only once changed. */
    editor->soft_end = spin(editor, "soft-end-seconds", 0, 600);
    gtk_widget_set_tooltip_text(editor->soft_end, "Kuşak bitişinde son şarkının en çok kaç saniye taşabileceği. 0: kuşak sonunda kesin geçiş.");
    grid_field(grid, "Kuşak sonu taşma (sn)", editor->soft_end, 1, 1);
    editor->relax_gaps = named(gtk_check_button_new_with_label("Kurallar tükenince tekrar aralıklarını gevşet"), "relax-gaps");
    gtk_widget_set_tooltip_text(editor->relax_gaps, "Hiçbir şarkı tekrar kurallarına uymuyorsa sessiz kalmak yerine aralıkları gevşeterek en uygun şarkıyı çalar.");
    g_signal_connect(editor->relax_gaps, "toggled", G_CALLBACK(changed), editor);
    gtk_grid_attach(GTK_GRID(grid), editor->relax_gaps, 0, 2, 4, 1);
    /* Content kinds the rotation (schema) leaves out: a checked kind never
     * plays from it, and without jingles its in-song jingle layers stop too. */
    GtkWidget *kinds = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    for (guint kind = 0; kind < AUTODJ_KIND_COUNT; ++kind) {
        gchar *id = g_strdup_printf("excluded-kind-%s", autodj_kind_ids[kind]);
        editor->excluded_kinds[kind] = named(gtk_check_button_new_with_label(autodj_kind_labels[kind]), id); g_free(id);
        gtk_widget_set_tooltip_text(editor->excluded_kinds[kind], "İşaretli tür bu rotasyonda/şemada hiç çalmaz.");
        g_signal_connect(editor->excluded_kinds[kind], "toggled", G_CALLBACK(changed), editor);
        gtk_box_pack_start(GTK_BOX(kinds), editor->excluded_kinds[kind], FALSE, FALSE, 0);
    }
    gtk_grid_attach(GTK_GRID(grid), label("Olmayan türler", "re-caption"), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), kinds, 1, 3, 3, 1);
    return grid;
}
static JsonObject *settings_read(RotationEditor *editor, gboolean source)
{
    JsonObject *target = source ? selected_source(editor, NULL) : config_object(editor);
    JsonObject *previous = object_member(target, "settings");
    JsonObject *settings = previous ? copy_object(previous) : json_object_new();
    GtkWidget **gaps = source ? editor->source_gaps : editor->global_gaps;
    if (!source) json_object_set_string_member(settings, "selection_mode",
        gtk_combo_box_get_active(GTK_COMBO_BOX(editor->selection_mode)) == 1 ? "weighted" : "linear");
    for (guint i = 0; i < GAP_COUNT; ++i)
        json_object_set_int_member(settings, gap_keys[i], spin_read(gaps[i]));
    /* Stored transition choices are kept as they are: a rotation always carries
     * the array the planner expects; a playlist override never adds one. */
    if (!source && !array_member(settings, "transitions"))
        json_object_set_array_member(settings, "transitions", default_transitions());
    if (!source) autodj_read(editor, settings);
    if (!source && editor->automation_body) radios_autodj_automation_read(editor->automation_body, settings);
    return settings;
}
static gboolean automation_valid(RotationEditor *editor)
{
    gchar *message = NULL;
    if (!editor->automation_body || radios_autodj_automation_valid(editor->automation_body, &message)) return TRUE;
    if (editor->status) gtk_label_set_text(GTK_LABEL(editor->status), message);
    g_free(message); return FALSE;
}
static void settings_render(RotationEditor *editor, JsonObject *settings, gboolean source)
{
    /* The automation page renders first: the kind policy greys out kinds it serves. */
    if (!source && editor->automation_body) radios_autodj_automation_render(editor->automation_body, settings);
    if (!source) autodj_render(editor, settings);
    GtkWidget **gaps = source ? editor->source_gaps : editor->global_gaps;
    if (!source) gtk_combo_box_set_active(GTK_COMBO_BOX(editor->selection_mode),
        !g_strcmp0(radios_json_string(settings, "selection_mode", "linear"), "weighted") ? 1 : 0);
    if (!source) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->soft_end), radios_json_int(settings, "soft_end_seconds", 0));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->relax_gaps), bool_member(settings, "relax_gaps"));
        JsonArray *excluded = array_member(settings, "excluded_kinds");
        for (guint kind = 0; kind < AUTODJ_KIND_COUNT; ++kind) {
            gboolean active = FALSE;
            for (guint i = 0; excluded && i < json_array_get_length(excluded); ++i) {
                JsonNode *node = json_array_get_element(excluded, i);
                active |= JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING && g_str_equal(json_node_get_string(node), autodj_kind_ids[kind]);
            }
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->excluded_kinds[kind]), active);
        }
    }
    for (guint i = 0; i < GAP_COUNT; ++i)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(gaps[i]), radios_json_int(settings, gap_keys[i], 0));
}
static gboolean time_parse(GtkWidget *entry, gboolean ending, gint *minute)
{
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (strlen(text) != 5 || text[2] != ':' || !g_ascii_isdigit(text[0]) || !g_ascii_isdigit(text[1]) ||
        !g_ascii_isdigit(text[3]) || !g_ascii_isdigit(text[4])) return FALSE;
    gint hour = (text[0] - '0') * 10 + text[1] - '0', min = (text[3] - '0') * 10 + text[4] - '0';
    if (min > 59 || hour > (ending ? 24 : 23) || (hour == 24 && min != 0)) return FALSE;
    *minute = hour * 60 + min; return TRUE;
}
static void time_set(GtkWidget *entry, gint64 minute)
{
    gchar *text = g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, minute / 60, minute % 60);
    gtk_entry_set_text(GTK_ENTRY(entry), text); g_free(text);
}
/* The status label is created last; widgets built before it may already
 * emit change signals, so a missing label is simply skipped. */
static void status(RotationEditor *editor, const gchar *text)
{ if (editor->status) gtk_label_set_text(GTK_LABEL(editor->status), text ? text : ""); }
static gboolean settings_valid(RotationEditor *editor, gboolean source)
{
    GtkWidget **gaps = source ? editor->source_gaps : editor->global_gaps;
    for (guint i = 0; i < GAP_COUNT; ++i) if (!spin_valid(gaps[i], 0, 10080)) {
        status(editor, "Tekrar aralıkları 0–10080 arasında tam dakika olmalı; geçersiz değer kaydedilmedi."); return FALSE;
    }
    return source || (autodj_valid(editor) && automation_valid(editor));
}

static void song_properties_render(RotationEditor *editor)
{
    guint position = 0; JsonObject *track = selected_song(editor,&position);
    gtk_widget_set_sensitive(editor->song_properties, track != NULL);
    if (!track) { gtk_label_set_text(GTK_LABEL(editor->song_summary), ""); return; }
    JsonObject *rule = song_rule(selected_source(editor,NULL),editor->selected_song);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->song_weight),radios_json_int(rule,"weight",100));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->song_order),radios_json_int(rule,"order",position)+1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->song_frequency),radios_json_int(rule,"frequency",1));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->song_max),radios_json_int(rule,"max_plays",0));
    time_set(editor->song_start,radios_json_int(rule,"start_minute",0)); time_set(editor->song_end,radios_json_int(rule,"end_minute",1440));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->song_interval),radios_json_int(rule,"interval_minutes",0));
    gchar *window = clock_window(radios_json_int(rule,"start_minute",0),radios_json_int(rule,"end_minute",1440));
    gchar *cap = radios_json_int(rule,"max_plays",0) ? g_strdup_printf("en fazla %" G_GINT64_FORMAT " kez",radios_json_int(rule,"max_plays",0)) : g_strdup("adet sınırı yok");
    gchar *text = g_strdup_printf("%s\n%s (takvim saati) · her %" G_GINT64_FORMAT " playlist turunda · %s\nEn az %" G_GINT64_FORMAT " dk aralık; playlistin sanatçı/albüm tekrar kuralları da uygulanır.",
        radios_json_string(track,"title","Şarkı"),window,radios_json_int(rule,"frequency",1),cap,radios_json_int(rule,"interval_minutes",0));
    gtk_label_set_text(GTK_LABEL(editor->song_summary),text); g_free(text); g_free(window); g_free(cap);
}
static void song_selected(GtkTreeSelection *selection,gpointer data)
{
    RotationEditor *editor=data; if (editor->updating || editor->closed) return;
    draft_capture(editor);
    GtkTreeModel *model; GtkTreeIter iter; g_clear_pointer(&editor->selected_song,g_free);
    if (gtk_tree_selection_get_selected(selection,&model,&iter)) gtk_tree_model_get(model,&iter,SONG_ID,&editor->selected_song,-1);
    editor->updating=TRUE; song_properties_render(editor); draft_restore(editor); editor->updating=FALSE;
}
static gboolean song_rule_store(RotationEditor *editor, GtkWidget *widget)
{
    guint ordinal = 0;
    JsonObject *track = selected_song(editor, &ordinal), *source = selected_source(editor, NULL);
    if (!track || !source) return FALSE;
    gint begin = 0, end = 0;
    gboolean clock = widget == editor->song_start || widget == editor->song_end;
    if (clock && (!time_parse(editor->song_start, FALSE, &begin) ||
        !time_parse(editor->song_end, TRUE, &end) || begin >= end)) {
        status(editor, "Şarkının saat aralığını tamamlayın; diğer alanlar düzenlenebilir."); return FALSE;
    }
    GtkWidget *fields[] = {editor->song_weight, editor->song_order, editor->song_frequency, editor->song_max, editor->song_interval};
    const gchar *keys[] = {"weight", "order", "frequency", "max_plays", "interval_minutes"};
    const gint minimum[] = {0, 1, 1, 0, 0}, maximum[] = {100, 5000, 100, 200, 10080};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) if (widget == fields[i] && !spin_valid(widget, minimum[i], maximum[i])) {
        status(editor, "Şarkı kuralı için geçerli aralıkta bir tam sayı yazın."); return FALSE;
    }
    JsonArray *rules = array_member(source, "track_rules");
    JsonObject *rule = song_rule(source, editor->selected_song);
    if (!rule && rules && json_array_get_length(rules) >= 500) {
        status(editor, "Bir playlistte en fazla 500 özel şarkı kuralı tanımlanabilir."); return FALSE;
    }
    if (!rules) { rules = json_array_new(); json_object_set_array_member(source, "track_rules", rules); }
    if (!rule) {
        rule = json_object_new(); json_object_set_string_member(rule, "entry_id", track_id(track));
        json_object_set_int_member(rule, "weight", 100);
        json_object_set_int_member(rule, "order", ordinal);
        json_object_set_int_member(rule, "frequency", 1);
        json_object_set_int_member(rule, "max_plays", 0);
        json_object_set_int_member(rule, "start_minute", 0);
        json_object_set_int_member(rule, "end_minute", 1440);
        json_object_set_int_member(rule, "interval_minutes", 0);
        json_array_add_object_element(rules, rule);
    }
    if (clock) { json_object_set_int_member(rule, "start_minute", begin); json_object_set_int_member(rule, "end_minute", end); }
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) if (widget == fields[i])
        json_object_set_int_member(rule, keys[i], spin_read(widget) - (i == 1 ? 1 : 0));
    return TRUE;
}
static gint song_sort(GtkTreeModel *model,GtkTreeIter *a,GtkTreeIter *b,gpointer data)
{
    (void)data; gint order_a,order_b,ordinal_a,ordinal_b;
    gtk_tree_model_get(model,a,SONG_NUMBER,&order_a,SONG_ORIGINAL,&ordinal_a,-1);
    gtk_tree_model_get(model,b,SONG_NUMBER,&order_b,SONG_ORIGINAL,&ordinal_b,-1);
    return order_a!=order_b ? (order_a>order_b ? 1:-1) : (ordinal_a>ordinal_b)-(ordinal_a<ordinal_b);
}

static gboolean validate_stored(RotationEditor *editor)
{
    if (editor->closed) return FALSE;
    if (editor->ctx->data) {
        RadioSDataClient *current = radios_data_client_acquire(editor->ctx);
        gboolean same = current && radios_data_client_same_identity(current, editor->opened_client);
        g_clear_pointer(&current, radios_data_client_unref);
        if (!same) {
            status(editor, "Veri oturumu değişti veya bağlantı hazır değil; rotasyonu yeniden açın.");
            return FALSE;
        }
    }
    JsonObject *config = config_object(editor);
    if (!config || !g_uuid_string_is_valid(radios_json_string(config, "id", ""))) {
        status(editor, "Rotasyon kimliği geçersiz; tanımı yeniden açın."); return FALSE;
    }
    const gchar *name = radios_json_string(config, "name", ""); gchar *trimmed = g_strdup(name); g_strstrip(trimmed);
    gboolean valid_name = *trimmed && g_utf8_strlen(name, -1) <= 160; g_free(trimmed);
    if (!valid_name) { status(editor, "Kaydedilmiş rotasyon adı geçersiz; tanımı yeniden açın."); return FALSE; }
    JsonArray *sources = source_array(editor);
    if (!sources || json_array_get_length(sources) > 50) { status(editor, "Bir rotasyon en fazla 50 playlist içerebilir."); return FALSE; }
    return TRUE;
}
static gboolean validate_visible(RotationEditor *editor)
{
    if (!validate_stored(editor)) return FALSE;
    const gchar *name = gtk_entry_get_text(GTK_ENTRY(editor->name)); gchar *trimmed = g_strdup(name); g_strstrip(trimmed);
    gboolean valid_name = *trimmed && g_utf8_strlen(name, -1) <= 160; g_free(trimmed);
    if (!valid_name) { status(editor, "Rotasyon adı boş olamaz; en fazla 160 karakter kullanın."); return FALSE; }
    if (!settings_valid(editor, FALSE)) return FALSE;
    JsonObject *source = selected_source(editor, NULL);
    if (source) {
        gint start = 0, end = 0;
        if (!time_parse(editor->start, FALSE, &start) || !time_parse(editor->end, TRUE, &end) || start >= end) {
            status(editor, "Saat aralığını SS:DD biçiminde yazın; bitiş başlangıçtan sonra olmalı (24:00 kullanılabilir)."); return FALSE;
        }
        if (!spin_valid(editor->frequency, 1, 100) || !spin_valid(editor->count, 1, 100) ||
            !spin_valid(editor->weight, 0, 100) || !spin_valid(editor->interval, 0, 10080)) {
            status(editor, "Sıklık ve adet 1–100; ağırlık 0–100; aralık 0–10080 tam dakika olmalı."); return FALSE;
        }
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->override)) && !settings_valid(editor, TRUE)) return FALSE;
    }
    if (selected_song(editor,NULL)) {
        gint begin=0,end=0;
        if (!time_parse(editor->song_start,FALSE,&begin) || !time_parse(editor->song_end,TRUE,&end) || begin>=end) {
            status(editor,"Şarkının saat aralığı geçersiz; bitiş başlangıçtan sonra olmalı. Önceki kayıt korunuyor.");return FALSE;
        }
        if (!spin_valid(editor->song_weight,0,100) || !spin_valid(editor->song_order,1,5000) ||
            !spin_valid(editor->song_frequency,1,100) || !spin_valid(editor->song_max,0,200) || !spin_valid(editor->song_interval,0,10080)) {
            status(editor,"Şarkı ağırlığı 0–100, sıra 1–5000, sıklık 1–100, adet 0–200 ve aralık 0–10080 olmalı.");return FALSE;
        }
        JsonArray *rules=array_member(source,"track_rules");
        if (!song_rule(source,editor->selected_song) && rules && json_array_get_length(rules)>=500) {
            status(editor,"Bir playlistte en fazla 500 özel şarkı kuralı tanımlanabilir.");return FALSE;
        }
    }
    return TRUE;
}
static void emit_changed(RotationEditor *editor)
{
    if (!validate_stored(editor)) return;
    schema_refresh(editor);
    editor->valid = validate_visible(editor);
    if (editor->valid) status(editor, "Değişiklikler otomatik kaydediliyor.");
    if (editor->hooks.changed) {
        JsonNode *snapshot = copy_node(editor->config);
        editor->hooks.changed(editor->root, snapshot, editor->hooks.user); json_node_free(snapshot);
    }
}
/* Keep unfinished text with its source/song identity while independent
 * actions render the tables or temporarily select another source. */
static void draft_visit(RotationEditor *editor, GtkWidget *widget, gboolean restore)
{
    if (GTK_IS_ENTRY(widget)) {
        gboolean supported = GTK_IS_SPIN_BUTTON(widget) || widget == editor->name ||
            widget == editor->start || widget == editor->end || widget == editor->song_start || widget == editor->song_end;
        if (supported) {
            gboolean song = editor->song_properties && gtk_widget_is_ancestor(widget, editor->song_properties);
            gboolean source = song || (editor->source_properties && gtk_widget_is_ancestor(widget, editor->source_properties));
            gchar *key = g_strdup_printf("%s/%s/%s", source && editor->selected ? editor->selected : "global",
                song && editor->selected_song ? editor->selected_song : "", gtk_widget_get_name(widget));
            if (restore) {
                const gchar *text = g_hash_table_lookup(editor->draft_fields, key);
                if (text) gtk_entry_set_text(GTK_ENTRY(widget), text);
                g_free(key);
            } else {
                gboolean valid = TRUE;
                if (GTK_IS_SPIN_BUTTON(widget)) {
                    GtkAdjustment *adjustment = gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(widget));
                    valid = spin_valid(widget, (gint)gtk_adjustment_get_lower(adjustment), (gint)gtk_adjustment_get_upper(adjustment));
                } else if (widget == editor->name) {
                    gchar *name = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget))); g_strstrip(name);
                    valid = *name && g_utf8_strlen(name, -1) <= 160; g_free(name);
                } else {
                    GtkWidget *begin = song ? editor->song_start : editor->start, *end = song ? editor->song_end : editor->end;
                    gint start_minute = 0, end_minute = 0;
                    valid = time_parse(begin, FALSE, &start_minute) && time_parse(end, TRUE, &end_minute) && start_minute < end_minute;
                }
                if (valid) { g_hash_table_remove(editor->draft_fields, key); g_free(key); }
                else g_hash_table_replace(editor->draft_fields, key, g_strdup(gtk_entry_get_text(GTK_ENTRY(widget))));
            }
        }
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *item = children; item; item = item->next) draft_visit(editor, item->data, restore);
        g_list_free(children);
    }
}
static void draft_capture(RotationEditor *editor) { draft_visit(editor, editor->root, FALSE); }
static void draft_restore(RotationEditor *editor) { draft_visit(editor, editor->root, TRUE); }
static void totals_render(RotationEditor *editor)
{
    JsonArray *sources = source_array(editor); guint count = sources ? json_array_get_length(sources) : 0, tracks = 0;
    for (guint i = 0; sources && i < count; ++i) {
        JsonNode *node = json_array_get_element(sources, i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
        JsonObject *source = json_node_get_object(node);
        JsonArray *items = array_member(playlist_by_id(editor, radios_json_string(source, "playlist_id", "")), "tracks");
        if (items) tracks += json_array_get_length(items);
    }
    gchar *text = g_strdup_printf("%u yayın adımı · %u kaynak şarkı", count, tracks);
    gtk_label_set_text(GTK_LABEL(editor->total), text); g_free(text);
    gtk_widget_set_visible(editor->content_warning, count == 0);
}
static JsonObject *copy_object(JsonObject *object)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, object);
    JsonNode *copy = copy_node(node); json_node_free(node);
    JsonObject *result = json_object_ref(json_node_get_object(copy)); json_node_free(copy); return result;
}
/* Settings also contain independent draft fields. Copy the persisted values
 * and replace only this control (or its coupled AutoDJ policy). */
static gboolean autodj_control(RotationEditor *editor, GtkWidget *widget)
{
    if (widget == editor->autodj_enabled || (editor->autodj_body && gtk_widget_is_ancestor(widget, editor->autodj_body))) return TRUE;
    for (guint k = 0; k < AUTODJ_KIND_COUNT; ++k)
        for (guint d = 0; d < AUTODJ_PLAYER_COUNT; ++d) if (widget == editor->autodj_players[k][d]) return TRUE;
    return FALSE;
}
static gboolean schema_presets_known(JsonNode *node, JsonArray *presets)
{
    if (!node) return TRUE;
    if (JSON_NODE_HOLDS_ARRAY(node)) {
        JsonArray *array = json_node_get_array(node);
        for (guint i = 0; i < json_array_get_length(array); ++i)
            if (!schema_presets_known(json_array_get_element(array, i), presets)) return FALSE;
    } else if (JSON_NODE_HOLDS_OBJECT(node)) {
        JsonObject *object = json_node_get_object(node); const gchar *id = radios_json_string(object, "preset_id", "");
        if (*id) {
            gboolean found = FALSE;
            for (guint i = 0; presets && i < json_array_get_length(presets); ++i)
                found |= !g_ascii_strcasecmp(id, radios_json_string(json_array_get_object_element(presets, i), "id", ""));
            if (!found) return FALSE;
        }
        GList *keys = json_object_get_members(object); gboolean valid = TRUE;
        for (GList *key = keys; key && valid; key = key->next)
            valid = schema_presets_known(json_object_get_member(object, key->data), presets);
        g_list_free(keys); return valid;
    }
    return TRUE;
}
static gboolean settings_store(RotationEditor *editor, gboolean source, GtkWidget *widget)
{
    JsonObject *target = source ? selected_source(editor, NULL) : config_object(editor);
    JsonObject *previous = object_member(target, "settings");
    if (!previous) return FALSE;
    JsonObject *settings = copy_object(previous);
    GtkWidget **gaps = source ? editor->source_gaps : editor->global_gaps;
    gboolean handled = FALSE;
    for (guint i = 0; i < GAP_COUNT; ++i) if (widget == gaps[i]) {
        if (!spin_valid(widget, 0, 10080)) goto invalid;
        json_object_set_int_member(settings, gap_keys[i], spin_read(widget)); handled = TRUE;
    }
    if (!source && widget == editor->selection_mode) {
        json_object_set_string_member(settings, "selection_mode", gtk_combo_box_get_active(GTK_COMBO_BOX(widget)) == 1 ? "weighted" : "linear"); handled = TRUE;
    }
    if (!source && widget == editor->soft_end) {
        if (!spin_valid(widget, 0, 600)) goto invalid;
        json_object_set_int_member(settings, "soft_end_seconds", spin_read(widget)); handled = TRUE;
    }
    if (!source && widget == editor->relax_gaps) {
        json_object_set_boolean_member(settings, "relax_gaps", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))); handled = TRUE;
    }
    for (guint kind = 0; !source && kind < AUTODJ_KIND_COUNT; ++kind) if (widget == editor->excluded_kinds[kind]) {
        /* The list is rewritten from the four toggles; an empty list is
         * dropped so unchanged records keep their exact shape. */
        JsonArray *excluded = json_array_new();
        for (guint k = 0; k < AUTODJ_KIND_COUNT; ++k)
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->excluded_kinds[k]))) json_array_add_string_element(excluded, autodj_kind_ids[k]);
        if (json_array_get_length(excluded)) json_object_set_array_member(settings, "excluded_kinds", excluded);
        else { json_array_unref(excluded); if (json_object_has_member(settings, "excluded_kinds")) json_object_remove_member(settings, "excluded_kinds"); }
        handled = TRUE;
    }
    if (!source && autodj_control(editor, widget)) {
        if (!autodj_valid(editor)) goto invalid;
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->autodj_enabled))) {
            json_object_remove_member(settings, "kind_transitions"); json_object_remove_member(settings, "autodj_players");
        } else autodj_read(editor, settings);
        handled = TRUE;
    }
    if (!source && widget == editor->automation_page) {
        if (!automation_valid(editor)) goto invalid;
        json_object_remove_member(settings, "autodj_presets"); json_object_remove_member(settings, "autodj_automation");
        radios_autodj_automation_read(editor->automation_body, settings);
        if (!schema_presets_known(editor->config, array_member(settings, "autodj_presets"))) {
            radios_autodj_automation_render(editor->automation_body, previous); json_object_unref(settings);
            status(editor, "Bu otomasyon şemada kullanılıyor. Önce Şema Ayarları'ndaki atamasını değiştirin."); return FALSE;
        }
        autodj_sensitivity(editor); handled = TRUE;
    }
    if (!handled) goto invalid;
    json_object_set_object_member(target, "settings", settings); return TRUE;
invalid:
    json_object_unref(settings);
    status(editor, "Bu ayarı geçerli bir değerle tamamlayın; diğer alanlar düzenlenebilir."); return FALSE;
}
/* Save only the field being edited. An incomplete clock or numeric entry
 * stays in its widget while independent valid changes can still be saved. */
static void changed(GtkWidget *widget, gpointer data)
{
    RotationEditor *editor = data;
    if (editor->updating || editor->closed) return;
    if (!validate_stored(editor)) return;
    JsonObject *config = config_object(editor), *source = selected_source(editor, NULL);
    if (widget == editor->name) {
        const gchar *name = gtk_entry_get_text(GTK_ENTRY(widget)); gchar *trimmed = g_strdup(name); g_strstrip(trimmed);
        gboolean valid = *trimmed && g_utf8_strlen(name, -1) <= 160; g_free(trimmed);
        if (!valid) { status(editor, "Rotasyon adı boş olamaz; en fazla 160 karakter kullanın."); return; }
        json_object_set_string_member(config, "name", name);
    } else if (widget == editor->selection_mode || gtk_widget_is_ancestor(widget, editor->global_settings) ||
        widget == editor->automation_page || autodj_control(editor, widget)) {
        if (!settings_store(editor, FALSE, widget)) return;
    } else if (source && gtk_widget_is_ancestor(widget, editor->song_properties)) {
        if (!song_rule_store(editor, widget)) return;
    } else if (source) {
        GtkWidget *fields[] = {editor->frequency, editor->count, editor->weight, editor->interval};
        const gchar *keys[] = {"frequency", "count", "weight", "interval_minutes"};
        const gint minimum[] = {1, 1, 0, 0}, maximum[] = {100, 100, 100, 10080};
        for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) if (widget == fields[i]) {
            if (!spin_valid(widget, minimum[i], maximum[i])) { status(editor, "Geçerli aralıkta bir tam sayı yazın; diğer alanlar düzenlenebilir."); return; }
            json_object_set_int_member(source, keys[i], spin_read(widget));
        }
        if (widget == editor->weight) selection_mode_sync(editor);
        if (widget == editor->start || widget == editor->end) {
            gint begin = 0, end = 0;
            if (!time_parse(editor->start, FALSE, &begin) || !time_parse(editor->end, TRUE, &end) || begin >= end) {
                status(editor, "Saat aralığını SS:DD biçiminde tamamlayın; diğer alanlar düzenlenebilir."); return;
            }
            json_object_set_int_member(source, "start_minute", begin); json_object_set_int_member(source, "end_minute", end);
        }
        if (widget == editor->track_order) {
            gint mode = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
            json_object_set_boolean_member(source, "track_random", mode != 0);
            if (mode == 1) json_object_remove_member(source, "track_selection_mode");
            else json_object_set_string_member(source, "track_selection_mode", mode == 2 ? "weighted" : "linear");
        }
        GtkWidget *flags[] = {editor->random, editor->advert_random, editor->sweeper_random};
        const gchar *flag_keys[] = {"random", "advert_random", "sweeper_random"};
        for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) if (widget == flags[i])
            json_object_set_boolean_member(source, flag_keys[i], gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
        if (widget == editor->media_kind) {
            const gchar *kind = gtk_combo_box_get_active_id(GTK_COMBO_BOX(widget));
            json_object_set_string_member(source, "media_kind", kind ? kind : "all");
        }
        if (widget == editor->override || gtk_widget_is_ancestor(widget, editor->override_body)) {
            gboolean override = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(editor->override));
            gtk_widget_set_sensitive(editor->override_body, override);
            if (override) {
                if (widget == editor->override) {
                    if (!settings_valid(editor, TRUE)) return;
                    json_object_set_object_member(source, "settings", settings_read(editor, TRUE));
                } else if (!settings_store(editor, TRUE, widget)) return;
            }
            else {
                JsonObject *preserved = object_member(source, "settings");
                if (preserved) {
                    preserved = copy_object(preserved);
                    for (guint i = 0; i < GAP_COUNT; ++i) json_object_remove_member(preserved, gap_keys[i]);
                    json_object_remove_member(preserved, "transitions");
                    json_object_remove_member(preserved, "transition_selection_mode");
                    if (json_object_get_size(preserved)) json_object_set_object_member(source, "settings", preserved);
                    else { json_object_unref(preserved); json_object_remove_member(source, "settings"); }
                }
                editor->updating = TRUE;
                settings_render(editor, object_member(config, "settings"), TRUE); editor->updating = FALSE;
            }
        }
    }
    GtkWidget *song_fields[] = {editor->song_start, editor->song_end, editor->song_weight, editor->song_order, editor->song_frequency, editor->song_max, editor->song_interval};
    gchar *drafts[G_N_ELEMENTS(song_fields)];
    for (guint i = 0; i < G_N_ELEMENTS(song_fields); ++i) drafts[i] = g_strdup(gtk_entry_get_text(GTK_ENTRY(song_fields[i])));
    editor->updating = TRUE; sources_render(editor); songs_render(editor); source_summary_render(editor);
    for (guint i = 0; i < G_N_ELEMENTS(song_fields); ++i) { gtk_entry_set_text(GTK_ENTRY(song_fields[i]), drafts[i]); g_free(drafts[i]); }
    editor->updating = FALSE; totals_render(editor);
    editor->valid = validate_visible(editor);
    if (editor->valid) status(editor, "Değişiklikler otomatik kaydediliyor.");
    if (editor->hooks.changed) {
        JsonNode *snapshot = copy_node(editor->config);
        editor->hooks.changed(editor->root, snapshot, editor->hooks.user); json_node_free(snapshot);
    }
}
static GtkWidget *table_scrolled(GtkWidget *tree, gint height)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, height); gtk_widget_set_vexpand(scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), tree); return scroll;
}
static void table_column(GtkWidget *tree, const gchar *title, gint column, gboolean expand)
{
    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 7u, NULL);
    GtkTreeViewColumn *view = gtk_tree_view_column_new_with_attributes(title, cell, "text", column, NULL);
    gtk_tree_view_column_set_expand(view, expand); gtk_tree_view_column_set_min_width(view, expand ? 90 : 50);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), view);
}
static gchar *track_duration(JsonObject *track)
{
    gint64 ms = MAX(0, radios_json_int(track, "duration_ms", 0));
    return ms ? g_strdup_printf("%" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, ms / 60000, ms / 1000 % 60) : g_strdup("—");
}
static void songs_render(RotationEditor *editor)
{
    JsonObject *source = selected_source(editor, NULL); gtk_list_store_clear(editor->song_store);
    if (!source) {
        gtk_label_set_text(GTK_LABEL(editor->heading), "Playlist seçilmedi");
        /* Only the song and per-step AutoDJ pages need a step; every edit
         * re-renders here, so the probability, AutoDJ and schema pages stay
         * open while no playlist row is selected. */
        const gchar *visible = gtk_stack_get_visible_child_name(GTK_STACK(editor->pages));
        if (!g_strcmp0(visible, "songs") || !g_strcmp0(visible, "routing")) gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), "library");
        return;
    }
    JsonObject *playlist = playlist_by_id(editor, radios_json_string(source, "playlist_id", ""));
    JsonArray *tracks = array_member(playlist, "tracks");
    gtk_label_set_text(GTK_LABEL(editor->heading), playlist ? radios_json_string(playlist, "name", "Playlist") : "Playlist kullanılamıyor");
    guint total_weight=0;
    for(guint i=0;tracks && i<json_array_get_length(tracks);++i){JsonObject *t=json_array_get_object_element(tracks,i);total_weight+=(guint)radios_json_int(song_rule(source,track_id(t)),"weight",100);}
    /* A playlist's own policy with groups replaces the rotation's groups. */
    JsonObject *own = object_member(object_member(source, "settings"), "probability_policy");
    JsonObject *policy = own && json_object_has_member(own, "children") ? own : object_member(object_member(config_object(editor), "settings"), "probability_policy");
    for (guint i = 0; tracks && i < json_array_get_length(tracks); ++i) {
        JsonNode *node = json_array_get_element(tracks, i); if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
        JsonObject *track = json_node_get_object(node), *metadata = object_member(track, "metadata");
        if (!metadata) metadata = track;
        GtkTreeIter iter; gtk_list_store_append(editor->song_store, &iter);
        gchar *duration = track_duration(metadata); JsonObject *rule=song_rule(source,track_id(track));
        gint weight=(gint)radios_json_int(rule,"weight",100);
        const gchar *mode=radios_json_string(source,"track_selection_mode","");
        gchar *share=!weight ? g_strdup("Kapalı") : policy_share(policy,"track",track_id(track));
        if (!share) share=!g_strcmp0(mode,"weighted") ?
            g_strdup_printf("%.1f%%",total_weight ? 100.0*weight/total_weight:0) :
            (!*mode && bool_member(source,"track_random") ? g_strdup("Eşit") : g_strdup("Sırayla"));
        gchar *window=clock_window(radios_json_int(rule,"start_minute",0),radios_json_int(rule,"end_minute",1440));
        gchar *cap=radios_json_int(rule,"max_plays",0) ? g_strdup_printf("%" G_GINT64_FORMAT,radios_json_int(rule,"max_plays",0)):g_strdup("Sınırsız");
        gtk_list_store_set(editor->song_store, &iter, SONG_ID, radios_json_string(track, "_id", radios_json_string(track, "entry_id", "")),
            SONG_NUMBER, (gint)radios_json_int(rule,"order",i)+1, SONG_TITLE, radios_json_string(metadata, "title", "—"),
            SONG_ARTIST, radios_json_string(metadata, "artist", ""), SONG_ALBUM, radios_json_string(metadata, "album", ""),
            SONG_DURATION,duration,SONG_WEIGHT,weight,SONG_SHARE,share,SONG_WINDOW,window,SONG_MAX,cap,SONG_ORIGINAL,(gint)i,-1);
        if(!g_strcmp0(editor->selected_song,track_id(track))) gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->songs)),&iter);
        g_free(duration);g_free(share);g_free(window);g_free(cap);
    }
    if (!playlist || !tracks) status(editor, "Playlist kayıtları kullanılamıyor; verileri yenileyin. Önceki şarkılar gösterilmiyor.");
    else if (!json_array_get_length(tracks)) status(editor, "Bu playlist henüz şarkı içermiyor.");
    else status(editor, "");
    song_properties_render(editor);
}
static void render_source(RotationEditor *editor)
{
    JsonObject *source = selected_source(editor, NULL); songs_render(editor);
    source_summary_render(editor); if (!source) return;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->frequency), radios_json_int(source, "frequency", 1));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->count), radios_json_int(source, "count", 1));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->weight), radios_json_int(source, "weight", 100));
    time_set(editor->start, radios_json_int(source, "start_minute", 0));
    time_set(editor->end, radios_json_int(source, "end_minute", 1440));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(editor->interval), radios_json_int(source, "interval_minutes", 0));
    const gchar *track_mode=radios_json_string(source,"track_selection_mode","");
    gtk_combo_box_set_active(GTK_COMBO_BOX(editor->track_order),!g_strcmp0(track_mode,"weighted") ? 2 :
        !g_strcmp0(track_mode,"linear") ? 0 : bool_member(source,"track_random") ? 1:0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->random), bool_member(source, "random"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->advert_random), bool_member(source, "advert_random"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->sweeper_random), bool_member(source, "sweeper_random"));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->media_kind), radios_json_string(source, "media_kind", "all"));
    JsonObject *settings = object_member(source, "settings");
    gboolean override = settings && (json_object_has_member(settings, "transitions") ||
        json_object_has_member(settings, "transition_selection_mode") ||
        json_object_has_member(settings, "track_gap_minutes") || json_object_has_member(settings, "artist_gap_minutes") ||
        json_object_has_member(settings, "album_gap_minutes"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(editor->override), override);
    gtk_widget_set_sensitive(editor->override_body, override);
    JsonObject *global = object_member(config_object(editor), "settings"), *effective = json_object_new();
    for (guint i = 0; i < GAP_COUNT; ++i)
        json_object_set_int_member(effective, gap_keys[i], radios_json_int(settings, gap_keys[i], radios_json_int(global, gap_keys[i], 0)));
    settings_render(editor, effective, TRUE); json_object_unref(effective);
    gtk_widget_set_sensitive(editor->weight, TRUE);
    draft_restore(editor);
}
static void sources_render(RotationEditor *editor)
{
    gtk_list_store_clear(editor->source_store); JsonArray *sources = source_array(editor);guint total_weight=0;
    for(guint i=0;sources && i<json_array_get_length(sources);++i)total_weight+=(guint)radios_json_int(json_array_get_object_element(sources,i),"weight",100);
    JsonObject *settings = object_member(config_object(editor), "settings"), *policy = object_member(settings, "probability_policy");
    gboolean weighted = !g_strcmp0(radios_json_string(settings, "selection_mode", "linear"), "weighted");
    for (guint i = 0; sources && i < json_array_get_length(sources); ++i) {
        JsonNode *node = json_array_get_element(sources, i); if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
        JsonObject *source = json_node_get_object(node), *playlist = playlist_by_id(editor, radios_json_string(source, "playlist_id", ""));
        GtkTreeIter iter; gtk_list_store_append(editor->source_store, &iter);
        gint weight=(gint)radios_json_int(source,"weight",100);
        gchar *share=!weight ? g_strdup("Kapalı") : policy_share(policy,"playlist",radios_json_string(source,"playlist_id",""));
        if (!share) share=weighted ? g_strdup_printf("%.1f%%",total_weight ? 100.0*weight/total_weight:0) : g_strdup("Sırayla");
        gchar *window=clock_window(radios_json_int(source,"start_minute",0),radios_json_int(source,"end_minute",1440));
        gtk_list_store_set(editor->source_store, &iter, SOURCE_ID, radios_json_string(source, "id", ""),
            /* A missing catalogue is not a missing playlist (kılavuz 18.10: a too large
             * catalogue response once marked every source unusable). */
            SOURCE_NAME, object_member(source, "live") ? radios_json_string(object_member(source, "live"), "title", "Live event") : playlist ? radios_json_string(playlist, "name", "Playlist") :
                catalog_current(editor) ? "Kullanılamayan playlist" : "Playlist kataloğu yüklenmedi",
            SOURCE_KIND, object_member(source, "live") ? "Live event" : playlist ? playlist_kind_label(radios_json_string(playlist, "purpose", "music")) : "—",
            SOURCE_TRACKS, array_member(playlist, "tracks") ? (gint)json_array_get_length(array_member(playlist, "tracks")) : 0,
            SOURCE_FREQUENCY, (gint)radios_json_int(source, "frequency", 1), SOURCE_COUNT, (gint)radios_json_int(source, "count", 1),
            SOURCE_ORDER,source_order(source),SOURCE_WEIGHT,weight,SOURCE_SHARE,share,SOURCE_WINDOW,window,SOURCE_INTERVAL,(gint)radios_json_int(source,"interval_minutes",0),-1);
        g_free(share);g_free(window);
        if (!g_strcmp0(editor->selected, radios_json_string(source, "id", "")))
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->sources)), &iter);
    }
    guint index = 0; JsonObject *selected = selected_source(editor, &index);
    gtk_widget_set_sensitive(editor->remove, selected != NULL);
    gtk_widget_set_sensitive(editor->up, selected && index > 0);
    gtk_widget_set_sensitive(editor->down, selected && sources && index + 1 < json_array_get_length(sources));
    gtk_widget_set_sensitive(editor->add, catalog_current(editor) && sources && json_array_get_length(sources) < 50 &&
        gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->playlists_combo)) != NULL);
}
static void restore_page(RotationEditor *editor, const gchar *page)
{
    gboolean needs_source = !g_strcmp0(page, "songs") || !g_strcmp0(page, "routing");
    gboolean keep_page = page && gtk_stack_get_child_by_name(GTK_STACK(editor->pages), page) &&
        (!needs_source || selected_source(editor, NULL));
    gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), keep_page ? page : "library");
}
static void render(RotationEditor *editor, gboolean preserve_page)
{
    gchar *page = preserve_page ? g_strdup(gtk_stack_get_visible_child_name(GTK_STACK(editor->pages))) : NULL;
    editor->updating = TRUE; JsonObject *config = config_object(editor);
    gtk_entry_set_text(GTK_ENTRY(editor->name), radios_json_string(config, "name", ""));
    gtk_label_set_text(GTK_LABEL(editor->name_caption), bool_member(object_member(config, "settings"), "automation_schema") ? "Şema" : "Rotasyon");
    settings_render(editor, object_member(config, "settings"), FALSE);
    sources_render(editor); render_source(editor); totals_render(editor);
    schema_refresh(editor);
    restore_page(editor, page); g_free(page);
    draft_restore(editor); editor->updating = FALSE; editor->valid = validate_visible(editor);
}
static void source_selected(GtkTreeSelection *selection, gpointer data)
{
    RotationEditor *editor = data; if (editor->updating || editor->closed) return;
    GtkTreeModel *model; GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
    draft_capture(editor);
    g_clear_pointer(&editor->selected_song,g_free); g_free(editor->selected); gtk_tree_model_get(model, &iter, SOURCE_ID, &editor->selected, -1);
    editor->updating = TRUE; render_source(editor); sources_render(editor); editor->updating = FALSE;
}
static void open_songs(RotationEditor *editor)
{
    gtk_expander_set_expanded(GTK_EXPANDER(editor->global_settings), FALSE);
    gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), "songs");
}
static void source_activated(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column, gpointer data)
{
    (void)column; RotationEditor *editor = data;
    if (editor->updating || !validate_stored(editor)) return;
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(tree), path);
    if (object_member(selected_source(editor, NULL), "live")) step_edit(NULL, editor);
    else open_songs(editor);
}
static const gchar *const routing_kinds[] = {"music", "advertisement", "jingle", "sweeper"};
static const gchar *const routing_titles[] = {"Müzik", "Reklam", "Jingle", "Sweeper"};
typedef struct {
    RotationEditor *editor;
    GtkWidget *dialog, *summary, *status, *checks[4][32], *kind, *mode, *scope, *forms, *tabs;
    GWeakRef cards[32];
    guint decks[32], count;
    gchar *names[32], *groups[32], *group_names[32], *source_id, *return_page;
    RadioSDataClient *client;
    JsonObject *saved;
    gboolean updating, dirty[4];
} RoutingDialog;
static void routing_free(gpointer data)
{
    RoutingDialog *routing = data;
    for (guint i = 0; i < routing->count; ++i) {
        g_weak_ref_clear(&routing->cards[i]); g_free(routing->names[i]);
        g_free(routing->groups[i]); g_free(routing->group_names[i]);
    }
    if (routing->client) radios_data_client_unref(routing->client);
    if (routing->saved) json_object_unref(routing->saved);
    g_free(routing->source_id); g_free(routing->return_page); g_free(routing);
}
static guint routing_kind(RoutingDialog *routing)
{ return (guint)MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(routing->kind))); }
static void routing_summary(RoutingDialog *routing)
{
    guint kind = routing_kind(routing), selected = 0; GString *text = g_string_new(NULL);
    for (guint i = 0; i < routing->count; ++i)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(routing->checks[kind][i]))) {
            if (selected++) g_string_append(text, " → ");
            g_string_append(text, routing->names[i]);
        }
    if (!selected) g_string_append(text, "Bir oynatıcı seç.");
    gtk_label_set_text(GTK_LABEL(routing->summary), text->str); g_string_free(text, TRUE);
}
static void routing_scope_render(RoutingDialog *routing)
{
    const gchar *scope = gtk_combo_box_get_active_id(GTK_COMBO_BOX(routing->scope));
    gboolean mixed = !g_strcmp0(scope, "saved-mixed");
    for (guint kind = 0; kind < G_N_ELEMENTS(routing_kinds); ++kind)
        for (guint i = 0; i < routing->count; ++i) {
            gboolean selected = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(routing->checks[kind][i]));
            gtk_widget_set_visible(routing->checks[kind][i], mixed ? selected : !g_strcmp0(scope, routing->groups[i]));
            gtk_widget_set_sensitive(routing->checks[kind][i], !mixed);
        }
}
static void routing_scope_changed(GtkComboBox *combo, gpointer data)
{
    RoutingDialog *routing = data; if (routing->updating) return;
    const gchar *scope = gtk_combo_box_get_active_id(combo); guint kind = routing_kind(routing);
    if (g_strcmp0(scope, "saved-mixed")) {
        routing->updating = TRUE;
        for (guint i = 0; i < routing->count; ++i)
            if (g_strcmp0(scope, routing->groups[i]))
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(routing->checks[kind][i]), FALSE);
        routing->dirty[kind] = TRUE; routing->updating = FALSE;
    }
    routing_scope_render(routing); routing_summary(routing);
}
static void routing_kind_changed(GtkComboBox *combo, gpointer data)
{
    (void)combo; RoutingDialog *routing = data; guint kind = routing_kind(routing), count = 0;
    if (routing->updating) return;
    routing->updating = TRUE;
    gtk_stack_set_visible_child_name(GTK_STACK(routing->forms), routing_kinds[kind]);
    for (guint i = 0; i < routing->count; ++i)
        count += gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(routing->checks[kind][i]));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(routing->mode), count > 1 ? "multi" : "single");
    const gchar *scope = NULL; gboolean mixed = FALSE;
    for (guint i = 0; i < routing->count; ++i)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(routing->checks[kind][i]))) {
            if (scope && g_strcmp0(scope, routing->groups[i])) mixed = TRUE;
            scope = routing->groups[i];
        }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(routing->scope), mixed ? "saved-mixed" : scope ? scope : "independent");
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(routing->scope)) < 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(routing->scope), 0);
    routing->updating = FALSE; routing_scope_render(routing); routing_summary(routing);
}
static void routing_check_changed(GtkToggleButton *button, gpointer data)
{
    RoutingDialog *routing = data; if (routing->updating) return;
    guint kind = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "routing-kind")) - 1; routing->dirty[kind] = TRUE;
    if (gtk_toggle_button_get_active(button) &&
        !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(routing->mode)), "single")) {
        routing->updating = TRUE;
        for (guint i = 0; i < routing->count; ++i)
            if (routing->checks[kind][i] != GTK_WIDGET(button))
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(routing->checks[kind][i]), FALSE);
        routing->updating = FALSE;
    }
    routing_summary(routing);
}
static void routing_mode_changed(GtkComboBox *combo, gpointer data)
{
    RoutingDialog *routing = data; if (routing->updating) return;
    guint kind = routing_kind(routing); routing->dirty[kind] = TRUE;
    if (!g_strcmp0(gtk_combo_box_get_active_id(combo), "single")) {
        gboolean found = FALSE; routing->updating = TRUE;
        for (guint i = 0; i < routing->count; ++i)
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(routing->checks[kind][i]))) {
                if (found) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(routing->checks[kind][i]), FALSE);
                found = TRUE;
            }
        routing->updating = FALSE; routing->dirty[kind] = TRUE;
    }
    routing_summary(routing);
}
static void routing_apply_all(GtkButton *button, gpointer data)
{
    (void)button; RoutingDialog *routing = data; guint selected = routing_kind(routing);
    routing->updating = TRUE;
    for (guint kind = 0; kind < G_N_ELEMENTS(routing_kinds); ++kind) {
        for (guint i = 0; i < routing->count; ++i)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(routing->checks[kind][i]),
                gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(routing->checks[selected][i])));
        routing->dirty[kind] = TRUE;
    }
    routing->updating = FALSE;
    gtk_label_set_text(GTK_LABEL(routing->status), "Bu oynatıcı seçimi tüm içerik türlerine uygulandı. Kaydet ile tamamla.");
}
static void routing_destroyed(GtkWidget *widget, gpointer data)
{
    RoutingDialog *routing = data;
    if (routing->editor->routing_dialog == widget) routing->editor->routing_dialog = NULL;
}
static void routing_back(GtkButton *button, gpointer data)
{
    (void)button; RoutingDialog *routing = data;
    gtk_stack_set_visible_child_name(GTK_STACK(routing->editor->pages), routing->return_page);
    gtk_widget_destroy(routing->dialog);
}
static gboolean routing_player_current(RoutingDialog *routing, guint i)
{
    gchar *key = g_strdup_printf("radios-deck-owner-%u", routing->decks[i]);
    GtkWidget *card = g_object_get_data(G_OBJECT(routing->editor->ctx->parent), key); g_free(key);
    GtkWidget *original = g_weak_ref_get(&routing->cards[i]);
    gboolean same = card && card == original && !gtk_widget_in_destruction(card);
    if (same) {
        const gchar *cluster = g_object_get_data(G_OBJECT(card), "radios-player-cluster-id");
        gchar *scope = cluster && *cluster ? g_strdup_printf("cluster:%s", cluster) : g_strdup("independent");
        same = !g_strcmp0(scope, routing->groups[i]); g_free(scope);
    }
    g_clear_object(&original); return same;
}
static void routing_save(GtkButton *button, gpointer data)
{
    (void)button; RoutingDialog *routing = data; RotationEditor *editor = routing->editor;
    RadioSDataClient *client = radios_data_client_acquire(editor->ctx);
    gboolean current = radios_data_client_same_identity(client, routing->client); if (client) radios_data_client_unref(client);
    JsonObject *source = selected_source(editor, NULL);
    if (editor->closed || !current || !source ||
        g_strcmp0(radios_json_string(source, "id", ""), routing->source_id) || !validate_stored(editor)) {
        gtk_label_set_text(GTK_LABEL(routing->status), "Playlist veya veri bağlantısı değişti; ayarları yeniden açın."); return;
    }
    if (!g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(routing->mode)), "multi")) {
        guint selected = 0, kind = routing_kind(routing);
        for (guint i = 0; i < routing->count; ++i)
            selected += gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(routing->checks[kind][i]));
        if (selected < 2) {
            gtk_label_set_text(GTK_LABEL(routing->status), "Geçiş için aynı gruptan en az iki oynatıcı seç."); return;
        }
    }
    for (guint kind = 0; kind < G_N_ELEMENTS(routing_kinds); ++kind)
        for (guint i = 0; i < routing->count; ++i)
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(routing->checks[kind][i])) && !routing_player_current(routing, i)) {
                gtk_label_set_text(GTK_LABEL(routing->status), "Oynatıcı veya grubu değişti; ayarları yeniden açın."); return;
            }
    JsonObject *mapping = routing->saved ? copy_object(routing->saved) : json_object_new();
    for (guint kind = 0; kind < G_N_ELEMENTS(routing_kinds); ++kind) {
        /* Unedited content-specific routes, including temporarily unavailable
         * players, survive opening and saving this simple form. */
        if (routing->saved && !routing->dirty[kind]) continue;
        JsonArray *decks = json_array_new();
        for (guint i = 0; i < routing->count; ++i) if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(routing->checks[kind][i]))) {
            json_array_add_int_element(decks, routing->decks[i] + 1);
        }
        if (!json_array_get_length(decks)) {
            json_array_unref(decks); json_object_unref(mapping);
            gchar *message = g_strdup_printf("%s için en az bir oynatıcı seç.", routing_titles[kind]);
            gtk_label_set_text(GTK_LABEL(routing->status), message); g_free(message); return;
        }
        json_object_set_array_member(mapping, routing_kinds[kind], decks);
    }
    json_object_set_object_member(source, "autodj", mapping);
    if (routing->saved) json_object_unref(routing->saved);
    routing->saved = copy_object(mapping); memset(routing->dirty, 0, sizeof(routing->dirty));
    source_summary_render(editor); emit_changed(editor);
    gtk_label_set_text(GTK_LABEL(routing->status), "Oynatıcı ayarları kaydedildi.");
}
static void source_autodj(GtkMenuItem *item, gpointer data)
{
    (void)item; RotationEditor *editor = data; JsonObject *source = selected_source(editor, NULL);
    if (editor->closed || !source || !validate_stored(editor)) return;
    if (editor->routing_dialog) {
        RoutingDialog *opened = g_object_get_data(G_OBJECT(editor->routing_dialog), "radios-routing-dialog");
        if (opened && !g_strcmp0(opened->source_id, editor->selected)) {
            gtk_stack_set_visible_child(GTK_STACK(editor->pages), editor->routing_dialog); return;
        }
        gtk_widget_destroy(editor->routing_dialog);
    }
    RoutingDialog *routing = g_new0(RoutingDialog, 1); routing->editor = editor;
    routing->source_id = g_strdup(radios_json_string(source, "id", ""));
    routing->return_page = g_strdup(gtk_stack_get_visible_child_name(GTK_STACK(editor->pages)));
    routing->client = radios_data_client_acquire(editor->ctx);
    JsonObject *saved = object_member(source, "autodj"); routing->saved = saved ? copy_object(saved) : NULL;
    routing->dialog = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, NULL), "autodj-dialog");
    editor->routing_dialog = routing->dialog;
    GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    GtkWidget *back = named(radios_ui_button("← Geri"), "autodj-cancel");
    g_signal_connect(back, "clicked", G_CALLBACK(routing_back), routing);
    gtk_box_pack_start(GTK_BOX(heading), back, FALSE, FALSE, 0);
    JsonObject *playlist = playlist_by_id(editor, radios_json_string(source, "playlist_id", ""));
    gchar *title = g_strdup_printf("%s · Ayarlar", radios_json_string(playlist, "name", "Playlist"));
    gtk_box_pack_start(GTK_BOX(heading), label(title, "re-heading"), TRUE, TRUE, 0); g_free(title);
    gtk_box_pack_start(GTK_BOX(routing->dialog), heading, FALSE, FALSE, 0);
    routing->tabs = named(gtk_notebook_new(), "autodj-settings-tabs");
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(routing->tabs), TRUE);
    gtk_box_pack_start(GTK_BOX(routing->dialog), routing->tabs, TRUE, TRUE, 0);
    GtkWidget *form = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL);
    gtk_container_set_border_width(GTK_CONTAINER(form), 12);
    GtkWidget *fields = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(fields), 12); gtk_grid_set_row_spacing(GTK_GRID(fields), 8);
    routing->kind = named(gtk_combo_box_text_new(), "autodj-content-kind");
    for (guint kind = 0; kind < G_N_ELEMENTS(routing_kinds); ++kind)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(routing->kind), routing_kinds[kind], routing_titles[kind]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(routing->kind), 0);
    routing->mode = named(gtk_combo_box_text_new(), "autodj-working-mode");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(routing->mode), "single", "Tek oynatıcıda çal");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(routing->mode), "multi", "Oynatıcılar arasında geçiş yap");
    routing->scope = named(gtk_combo_box_text_new(), "autodj-player-scope");
    grid_field(fields, "Oynatıcı grubu", routing->scope, 0, 0);
    grid_field(fields, "İçerik", routing->kind, 0, 1); grid_field(fields, "Çalışma biçimi", routing->mode, 0, 2);
    gtk_widget_set_hexpand(routing->mode, TRUE); gtk_widget_set_hexpand(routing->kind, TRUE);
    gtk_box_pack_start(GTK_BOX(form), fields, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(form), label("Hangi oynatıcıda çalsın?", "re-heading"), FALSE, FALSE, 0);
    routing->forms = named(gtk_stack_new(), "autodj-player-forms");
    gtk_stack_set_homogeneous(GTK_STACK(routing->forms), FALSE);
    for (guint deck = 0; deck < 32; ++deck) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
        GtkWidget *card = g_object_get_data(G_OBJECT(editor->ctx->parent), key); g_free(key);
        if (!card || gtk_widget_in_destruction(card)) continue;
        guint i = routing->count++; routing->decks[i] = deck; g_weak_ref_init(&routing->cards[i], card);
        const gchar *name = g_object_get_data(G_OBJECT(card), "radios-player-name");
        routing->names[i] = name ? g_strdup(name) : g_strdup_printf("Oynatıcı %u", deck + 1);
        const gchar *cluster = g_object_get_data(G_OBJECT(card), "radios-player-cluster-id");
        const gchar *cluster_name = g_object_get_data(G_OBJECT(card), "radios-player-cluster-name");
        routing->groups[i] = cluster && *cluster ? g_strdup_printf("cluster:%s", cluster) : g_strdup("independent");
        routing->group_names[i] = cluster && *cluster ? g_strdup(cluster_name && *cluster_name ? cluster_name : "Küme") : g_strdup("Kümesizler");
        gboolean added = FALSE;
        for (guint j = 0; j < i; ++j) if (!g_strcmp0(routing->groups[j], routing->groups[i])) added = TRUE;
        if (!added) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(routing->scope), routing->groups[i], routing->group_names[i]);
    }
    const gchar *default_group = routing->count ? routing->groups[0] : "independent";
    for (guint i = 0; i < routing->count; ++i)
        if (!g_strcmp0(routing->groups[i], "independent")) { default_group = routing->groups[i]; break; }
    for (guint kind = 0; kind < G_N_ELEMENTS(routing_kinds); ++kind) {
        GtkWidget *players = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, NULL);
        for (guint i = 0; i < routing->count; ++i) {
            gchar *id = g_strdup_printf("autodj-%s-player-%u", routing_kinds[kind], routing->decks[i] + 1);
            GtkWidget *check = routing->checks[kind][i] = named(gtk_check_button_new_with_label(routing->names[i]), id); g_free(id);
            guint in_group = 0, group_count = 0;
            for (guint j = 0; j < routing->count; ++j)
                if (!g_strcmp0(routing->groups[j], default_group)) {
                    if (j < i) ++in_group;
                    ++group_count;
                }
            gboolean enabled = !saved && !g_strcmp0(routing->groups[i], default_group) &&
                (kind == 0 ? in_group < MIN(group_count, 2u) : in_group == MIN(kind + 1, group_count - 1));
            JsonArray *decks = array_member(saved, routing_kinds[kind]);
            for (guint j = 0; decks && j < json_array_get_length(decks); ++j)
                if (json_array_get_int_element(decks, j) == (gint64)routing->decks[i] + 1) enabled = TRUE;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), enabled);
            gtk_box_pack_start(GTK_BOX(players), check, FALSE, FALSE, 0);
            gtk_widget_set_no_show_all(check, TRUE);
            g_object_set_data(G_OBJECT(check), "routing-kind", GUINT_TO_POINTER(kind + 1));
            g_signal_connect(check, "toggled", G_CALLBACK(routing_check_changed), routing);
        }
        gtk_stack_add_named(GTK_STACK(routing->forms), players, routing_kinds[kind]);
    }
    gboolean saved_mixed = FALSE;
    for (guint kind = 0; saved && kind < G_N_ELEMENTS(routing_kinds); ++kind) {
        const gchar *group = NULL;
        for (guint i = 0; i < routing->count; ++i)
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(routing->checks[kind][i]))) {
                if (group && g_strcmp0(group, routing->groups[i])) saved_mixed = TRUE;
                group = routing->groups[i];
            }
    }
    if (saved_mixed) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(routing->scope), "saved-mixed", "Kayıtlı seçim · farklı gruplar");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 55);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 145);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), routing->forms);
    gtk_box_pack_start(GTK_BOX(form), scroll, TRUE, TRUE, 0);
    routing->summary = named(label("", "re-caption"), "autodj-summary");
    gtk_label_set_line_wrap(GTK_LABEL(routing->summary), TRUE);
    gtk_box_pack_start(GTK_BOX(form), routing->summary, FALSE, FALSE, 0);
    GtkWidget *all = named(radios_ui_button("Bu seçimi tüm içeriklere uygula"), "autodj-apply-all");
    g_signal_connect(all, "clicked", G_CALLBACK(routing_apply_all), routing);
    gtk_box_pack_start(GTK_BOX(form), all, FALSE, FALSE, 0);
    routing->status = named(label(routing->count ? "" : "Önce Canlı oynatıcılar bölümünde bir oynatıcı ekle.", "application-status"), "autodj-status");
    gtk_label_set_line_wrap(GTK_LABEL(routing->status), TRUE);
    gtk_box_pack_start(GTK_BOX(form), routing->status, FALSE, FALSE, 0);
    GtkWidget *save = named(radios_ui_button("Oynatıcı ayarlarını kaydet"), "autodj-apply");
    gtk_style_context_add_class(gtk_widget_get_style_context(save), "suggested-action");
    gtk_widget_set_sensitive(save, routing->count > 0);
    g_signal_connect(save, "clicked", G_CALLBACK(routing_save), routing);
    gtk_box_pack_start(GTK_BOX(form), save, FALSE, FALSE, 0);
    GtkWidget *players_tab = gtk_label_new("Oynatıcılar");
    gtk_label_set_line_wrap(GTK_LABEL(players_tab), FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(routing->tabs), form, players_tab);
    GtkWidget *probability = probability_widget_new(editor, TRUE, FALSE);
    GtkWidget *probability_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(probability_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(probability_scroll), 300);
    gtk_container_add(GTK_CONTAINER(probability_scroll), probability);
    GtkWidget *probability_tab = gtk_label_new("Olasılık · Geçiş · Jingle");
    gtk_label_set_line_wrap(GTK_LABEL(probability_tab), FALSE);
    gtk_notebook_append_page(GTK_NOTEBOOK(routing->tabs), probability_scroll, probability_tab);
    g_signal_connect(routing->scope, "changed", G_CALLBACK(routing_scope_changed), routing);
    g_signal_connect(routing->kind, "changed", G_CALLBACK(routing_kind_changed), routing);
    g_signal_connect(routing->mode, "changed", G_CALLBACK(routing_mode_changed), routing);
    g_object_set_data_full(G_OBJECT(routing->dialog), "radios-routing-dialog", routing, routing_free);
    g_signal_connect(routing->dialog, "destroy", G_CALLBACK(routing_destroyed), routing);
    gtk_stack_add_named(GTK_STACK(editor->pages), routing->dialog, "routing");
    routing_kind_changed(NULL, routing); gtk_widget_show_all(routing->dialog);
    gtk_stack_set_visible_child(GTK_STACK(editor->pages), routing->dialog);
}
static gboolean source_pressed(GtkWidget *tree, GdkEventButton *event, gpointer data)
{
    RotationEditor *editor = data; if (event->button != 3 || editor->closed) return FALSE;
    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree), (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL)) return FALSE;
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), path); gtk_tree_path_free(path);
    if (!selected_source(editor, NULL)) return FALSE;
    gtk_widget_show_all(editor->source_menu); gtk_menu_popup_at_pointer(GTK_MENU(editor->source_menu), (GdkEvent *)event); return TRUE;
}
static gboolean source_menu_key(GtkWidget *tree, GdkEventKey *event, gpointer data)
{
    RotationEditor *editor = data;
    if (event->keyval != GDK_KEY_Menu && !(event->keyval == GDK_KEY_F10 && (event->state & GDK_SHIFT_MASK))) return FALSE;
    if (!selected_source(editor, NULL)) return FALSE;
    gtk_widget_show_all(editor->source_menu);
    gtk_menu_popup_at_widget(GTK_MENU(editor->source_menu), tree, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, (GdkEvent *)event); return TRUE;
}
static gboolean source_released(GtkWidget *tree, GdkEventButton *event, gpointer data)
{
    if (event->button != 1) return FALSE;
    GtkTreePath *path = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree), (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL)) {
        source_activated(GTK_TREE_VIEW(tree), path, NULL, data); gtk_tree_path_free(path);
    }
    return FALSE;
}
static void combo_changed(GtkComboBox *combo, gpointer data)
{
    (void)combo; RotationEditor *editor = data; if (editor->updating) return;
    editor->updating = TRUE; sources_render(editor); editor->updating = FALSE;
}
/* Appends one playlist as a rotation source; the caller renders once. */
static gboolean source_append(RotationEditor *editor, const gchar *playlist_id)
{
    JsonArray *sources = source_array(editor);
    if (!playlist_id || !playlist_by_id(editor, playlist_id) || !sources || json_array_get_length(sources) >= 50) return FALSE;
    JsonObject *source = json_object_new(); gchar *id = g_uuid_string_random();
    json_object_set_string_member(source, "id", id); json_object_set_string_member(source, "playlist_id", playlist_id);
    json_object_set_int_member(source, "frequency", 1); json_object_set_int_member(source, "count", 1);
    json_object_set_int_member(source, "weight", 100); json_object_set_int_member(source, "start_minute", 0);
    json_object_set_int_member(source, "end_minute", 1440); json_object_set_int_member(source, "interval_minutes", 0);
    const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
    for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(source, flags[i], FALSE);
    json_object_set_string_member(source, "media_kind", "all"); json_array_add_object_element(sources, source);
    g_clear_pointer(&editor->selected_song, g_free);
    g_free(editor->selected); editor->selected = id; return TRUE;
}
static void add_source(GtkButton *button, gpointer data)
{
    (void)button; RotationEditor *editor = data;
    if (editor->closed || !validate_stored(editor)) return;
    draft_capture(editor);
    if (!source_append(editor, gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->playlists_combo)))) {
        status(editor, "Playlist seçin; veriler güncel olmalı ve en fazla 50 kaynak kullanılabilir."); return;
    }
    render(editor, FALSE); emit_changed(editor);
    status(editor, "Playlist rotasyona eklendi. Sıklık, adet ve saat kurallarını aşağıdan düzenleyebilirsin.");
}
typedef struct {
    GWeakRef editor;
    RadioSDataClient *client;
    gchar *rotation_id;
} RotationPlaylistCreate;

static void playlist_create_free(gpointer data)
{
    RotationPlaylistCreate *write = data;
    g_weak_ref_clear(&write->editor);
    g_clear_pointer(&write->client, radios_data_client_unref);
    g_free(write->rotation_id); g_free(write);
}
static void playlist_create_sensitive(GtkWidget *dialog, gboolean enabled)
{
    gtk_widget_set_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT), enabled);
    gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(dialog), "create-name"), enabled);
    gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(dialog), "create-purpose"), enabled);
    g_object_set_data(G_OBJECT(dialog), "create-busy", GINT_TO_POINTER(!enabled));
}
static void playlist_created(RadioSViewContext *ctx, GtkWidget *dialog, JsonNode *result,
    const GError *error, gpointer data)
{
    RotationPlaylistCreate *write = data;
    GtkWidget *owner = g_weak_ref_get(&write->editor);
    RotationEditor *editor = editor_get(owner);
    GtkWidget *message = g_object_get_data(G_OBJECT(dialog), "create-status");
    RadioSDataClient *current = radios_data_client_acquire(ctx);
    gboolean same = editor && !editor->closed && radios_data_client_same_identity(current, write->client) &&
        !g_strcmp0(radios_json_string(config_object(editor), "id", ""), write->rotation_id);
    g_clear_pointer(&current, radios_data_client_unref);
    if (!same || !validate_stored(editor)) {
        gtk_label_set_text(GTK_LABEL(message), "Veri oturumu veya rotasyon değişti; bu sayfaya geç gelen kayıt eklenmedi.");
        g_clear_object(&owner); return;
    }
    playlist_create_sensitive(dialog, TRUE);
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        gtk_label_set_text(GTK_LABEL(message), error ? error->message : "Playlist kaydı alınamadı; girdiler korunuyor.");
        g_clear_object(&owner); return;
    }
    JsonObject *playlist = json_node_get_object(result);
    const gchar *id = radios_json_string(playlist, "_id", "");
    if (!g_uuid_string_is_valid(id) || !array_member(playlist, "tracks")) {
        gtk_label_set_text(GTK_LABEL(message), "Veri servisi geçerli bir playlist döndürmedi.");
        g_clear_object(&owner); return;
    }
    draft_capture(editor);
    JsonArray *catalogue = json_array_new(); gboolean replaced = FALSE;
    for (guint i = 0; editor->playlists && i < json_array_get_length(editor->playlists); ++i) {
        JsonNode *row = json_array_get_element(editor->playlists, i);
        gboolean match = JSON_NODE_HOLDS_OBJECT(row) &&
            !g_strcmp0(radios_json_string(json_node_get_object(row), "_id", ""), id);
        json_array_add_element(catalogue, copy_node(match ? result : row)); replaced |= match;
    }
    if (!replaced) json_array_add_element(catalogue, copy_node(result));
    radios_rotation_editor_set_playlists(editor->root, catalogue); json_array_unref(catalogue);
    gboolean added = source_append(editor, id);
    if (added) { render(editor, FALSE); emit_changed(editor); }
    status(editor, added ? "Playlist oluşturuldu ve rotasyona eklendi." : "Playlist kaydedildi; rotasyonda en fazla 50 kaynak kullanılabilir.");
    radios_data_refresh(ctx);
    gtk_widget_destroy(dialog); g_clear_object(&owner);
}
static void playlist_create_response(GtkDialog *dialog, gint response, gpointer data)
{
    RotationEditor *editor = data;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    if (editor->closed || g_object_get_data(G_OBJECT(dialog), "create-busy")) return;
    GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "create-name");
    GtkWidget *purpose = g_object_get_data(G_OBJECT(dialog), "create-purpose");
    GtkWidget *message = g_object_get_data(G_OBJECT(dialog), "create-status");
    gchar *name = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))); g_strstrip(name);
    if (!*name || g_utf8_strlen(name, -1) > 120) {
        gtk_label_set_text(GTK_LABEL(message), "Playlist adı 1–120 karakter olmalı.");
        gtk_widget_grab_focus(entry); g_free(name); return;
    }
    if (!validate_stored(editor) || !radios_data_connected(editor->ctx) || !catalog_current(editor)) {
        gtk_label_set_text(GTK_LABEL(message), "Veri bağlantısı veya playlist listesi güncel değil; girdiler korunuyor.");
        g_free(name); return;
    }
    JsonNode *body = radios_data_body(editor->ctx);
    if (!body || !JSON_NODE_HOLDS_OBJECT(body)) {
        g_clear_pointer(&body, json_node_free); g_free(name); return;
    }
    JsonObject *payload = json_node_get_object(body);
    const gchar *kind = gtk_combo_box_get_active_id(GTK_COMBO_BOX(purpose));
    json_object_set_string_member(payload, "name", name);
    json_object_set_string_member(payload, "purpose", kind ? kind : "music"); g_free(name);
    gchar *serialized = json_to_string(body, FALSE);
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, serialized, -1); g_free(serialized);
    if (g_strcmp0(hash, g_object_get_data(G_OBJECT(dialog), "create-request-hash"))) {
        g_object_set_data_full(G_OBJECT(dialog), "create-request-hash", g_strdup(hash), g_free);
        g_object_set_data_full(G_OBJECT(dialog), "create-request-key", g_uuid_string_random(), g_free);
    }
    g_free(hash);
    json_object_set_string_member(payload, "idempotency_key", g_object_get_data(G_OBJECT(dialog), "create-request-key"));
    RotationPlaylistCreate *write = g_new0(RotationPlaylistCreate, 1);
    g_weak_ref_init(&write->editor, editor->root); write->client = radios_data_client_acquire(editor->ctx);
    write->rotation_id = g_strdup(radios_json_string(config_object(editor), "id", ""));
    playlist_create_sensitive(GTK_WIDGET(dialog), FALSE);
    gtk_label_set_text(GTK_LABEL(message), "Playlist kaydediliyor…");
    gboolean queued = radios_data_request(editor->ctx, GTK_WIDGET(dialog), "POST", "/v1/workflows/desktop-playlists/create",
        body, playlist_created, write, playlist_create_free);
    json_node_free(body);
    if (!queued) {
        playlist_create_sensitive(GTK_WIDGET(dialog), TRUE);
        gtk_label_set_text(GTK_LABEL(message), "Kayıt gönderilemedi; girdiler korunuyor.");
    }
}
static void playlist_create_closed(GtkWidget *dialog, gpointer data)
{
    RotationEditor *editor = data;
    if (editor->create_dialog == dialog) editor->create_dialog = NULL;
}
static void create_playlist(GtkButton *button, gpointer data)
{
    (void)button; RotationEditor *editor = data;
    if (editor->closed || !validate_stored(editor)) return;
    if (editor->create_dialog) { gtk_window_present(GTK_WINDOW(editor->create_dialog)); return; }
    GtkWidget *dialog = named(gtk_dialog_new_with_buttons("Rotasyonda playlist oluştur", editor->ctx->parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL,
        "Oluştur ve ekle", GTK_RESPONSE_ACCEPT, NULL), "create-dialog");
    editor->create_dialog = dialog; gtk_window_set_default_size(GTK_WINDOW(dialog), 440, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(body), 16); gtk_box_set_spacing(GTK_BOX(body), 8);
    GtkWidget *entry = named(radios_ui_entry("Playlist adı"), "create-name");
    gtk_entry_set_max_length(GTK_ENTRY(entry), 120); gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    GtkWidget *purpose = named(gtk_combo_box_text_new(), "create-purpose");
    const gchar *kinds[] = {"music", "jingle", "advert", "sweeper", "announcement", "programme", "other"};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(purpose), kinds[i], playlist_kind_label(kinds[i]));
    gtk_combo_box_set_active(GTK_COMBO_BOX(purpose), 0);
    GtkWidget *message = named(label("", "application-status"), "create-status");
    gtk_label_set_line_wrap(GTK_LABEL(message), TRUE);
    gtk_box_pack_start(GTK_BOX(body), label("Playlist adı", "re-caption"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), label("Playlist türü", "re-caption"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), purpose, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), message, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(dialog), "create-name", entry);
    g_object_set_data(G_OBJECT(dialog), "create-purpose", purpose);
    g_object_set_data(G_OBJECT(dialog), "create-status", message);
    g_signal_connect_object(editor->root, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    g_signal_connect(dialog, "destroy", G_CALLBACK(playlist_create_closed), editor);
    g_signal_connect(dialog, "response", G_CALLBACK(playlist_create_response), editor);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); gtk_widget_show_all(dialog);
}

enum { PICK_ON, PICK_ID, PICK_NAME, PICK_KIND, PICK_TRACKS, PICK_COLUMNS };
static void pick_toggled(GtkCellRendererToggle *cell, gchar *path, gpointer data)
{
    (void)cell; GtkTreeModel *model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(data)); GtkTreeIter view, iter; gboolean on = FALSE;
    if (!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(data), &view, path)) return;
    gtk_tree_model_filter_convert_iter_to_child_iter(GTK_TREE_MODEL_FILTER(data), &iter, &view);
    gtk_tree_model_get(model, &iter, PICK_ON, &on, -1); gtk_list_store_set(GTK_LIST_STORE(model), &iter, PICK_ON, !on, -1);
}
static void pick_activated(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column, gpointer data)
{
    (void)column; gchar *text = gtk_tree_path_to_string(path); pick_toggled(NULL, text, gtk_tree_view_get_model(tree)); g_free(text); (void)data;
}
static gboolean pick_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    const gchar *needle = gtk_entry_get_text(GTK_ENTRY(data)); if (!*needle) return TRUE;
    gchar *name = NULL; gtk_tree_model_get(model, iter, PICK_NAME, &name, -1);
    gchar *a = g_utf8_casefold(name ? name : "", -1), *b = g_utf8_casefold(needle, -1);
    gboolean visible = strstr(a, b) != NULL; g_free(a); g_free(b); g_free(name); return visible;
}
static void pick_search(GtkSearchEntry *entry, gpointer data) { (void)entry; gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(data)); }
static gboolean pick_add(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
    (void)path; gpointer *state = data; RotationEditor *editor = state[0]; gboolean on = FALSE; gchar *id = NULL;
    gtk_tree_model_get(model, iter, PICK_ON, &on, PICK_ID, &id, -1);
    if (on) { if (source_append(editor, id)) ++*(guint *)state[1]; else ++*(guint *)state[2]; }
    g_free(id); return FALSE;
}
static void schema_load_response(GtkDialog *dialog, gint response, gpointer data)
{
    RotationEditor *editor = data;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    GtkWidget *message = g_object_get_data(G_OBJECT(dialog), "schema-load-status");
    JsonNode *base = g_object_get_data(G_OBJECT(dialog), "schema-load-base");
    if (!validate_stored(editor) || !catalog_current(editor) || !json_node_equal(base, editor->config)) {
        gtk_label_set_text(GTK_LABEL(message), "Rotasyon veya veri oturumu değişti; şemayı yeniden seçin."); return;
    }
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(g_object_get_data(G_OBJECT(dialog), "schema-load-choice")));
    JsonObject *selected = NULL;
    for (guint i = 0; id && editor->schemas && i < json_array_get_length(editor->schemas); ++i) {
        JsonObject *candidate = json_array_get_object_element(editor->schemas, i);
        if (!g_strcmp0(id, radios_json_string(candidate, "id", ""))) { selected = candidate; break; }
    }
    GtkWidget *replace = g_object_get_data(G_OBJECT(dialog), "schema-load-replace");
    if (!selected || !array_member(selected, "sources") || !json_array_get_length(array_member(selected, "sources"))) {
        gtk_label_set_text(GTK_LABEL(message), "Yayın adımları bulunan kayıtlı bir şema seçin."); return;
    }
    if (json_array_get_length(source_array(editor)) && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(replace))) {
        gtk_label_set_text(GTK_LABEL(message), "Mevcut adımların yerine seçilen şemayı yüklemeyi işaretleyin."); return;
    }
    JsonObject *replacement = copy_object(selected);
    /* Keep the edited definition's identity and name; all content, ordered
     * source rules, links and automation references come from the schema. */
    json_object_set_string_member(replacement, "id", radios_json_string(config_object(editor), "id", ""));
    json_object_set_string_member(replacement, "name", radios_json_string(config_object(editor), "name", ""));
    g_hash_table_remove_all(editor->draft_fields);
    g_clear_pointer(&editor->selected, g_free); g_clear_pointer(&editor->selected_song, g_free);
    json_node_free(editor->config); editor->config = json_node_new(JSON_NODE_OBJECT); json_node_take_object(editor->config, replacement);
    gtk_widget_destroy(GTK_WIDGET(dialog)); render(editor, FALSE); emit_changed(editor);
}
static void schema_load(RotationEditor *editor)
{
    if (editor->closed || !validate_stored(editor) || !catalog_current(editor)) return;
    GtkWidget *parent = gtk_widget_get_toplevel(editor->root);
    GtkWidget *dialog = named(gtk_dialog_new_with_buttons("Playlist ve live eventleri şemadan yükle",
        GTK_IS_WINDOW(parent) ? GTK_WINDOW(parent) : editor->ctx->parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Şemayı yükle", GTK_RESPONSE_ACCEPT, NULL), "schema-load-dialog");
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_container_set_border_width(GTK_CONTAINER(body), 16); gtk_box_set_spacing(GTK_BOX(body), 10);
    GtkWidget *choice = named(gtk_combo_box_text_new(), "schema-load-choice"); guint count = 0;
    for (guint i = 0; editor->schemas && i < json_array_get_length(editor->schemas); ++i) {
        JsonObject *definition = json_array_get_object_element(editor->schemas, i);
        if (!bool_member(object_member(definition, "settings"), "automation_schema") ||
            !g_strcmp0(radios_json_string(definition, "id", ""), radios_json_string(config_object(editor), "id", ""))) continue;
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(choice), radios_json_string(definition, "id", ""), radios_json_string(definition, "name", "Şema")); ++count;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(choice), count ? 0 : -1);
    GtkWidget *replace = named(gtk_check_button_new_with_label("Mevcut adımların yerine seçilen şemayı yükle"), "schema-load-replace");
    GtkWidget *message = named(label(count ? "Playlistler, live eventler, iç sıra ve atanmış otomasyonlar şemadan alınır." : "Önce Şema Ayarları bölümünde bir şema kaydedin.", "re-caption"), "schema-load-status");
    gtk_label_set_line_wrap(GTK_LABEL(message), TRUE);
    gtk_box_pack_start(GTK_BOX(body), choice, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(body), replace, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(body), message, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(dialog), "schema-load-choice", choice); g_object_set_data(G_OBJECT(dialog), "schema-load-replace", replace); g_object_set_data(G_OBJECT(dialog), "schema-load-status", message);
    g_object_set_data_full(G_OBJECT(dialog), "schema-load-base", copy_node(editor->config), (GDestroyNotify)json_node_free);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, count > 0);
    g_signal_connect(dialog, "response", G_CALLBACK(schema_load_response), editor);
    g_signal_connect_object(editor->root, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED); gtk_widget_show_all(dialog);
}
/* A clear chooser: search, tick any number of playlists, add them together. */
static void add_dialog(GtkButton *button, gpointer data)
{
    (void)button; RotationEditor *editor = data;
    if (editor->closed || !validate_stored(editor) || !catalog_current(editor) || !editor->playlists) {
        status(editor, "Playlist listesi henüz güncel değil; birkaç saniye sonra yeniden deneyin."); return;
    }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Playlist ya da live event ekle", editor->ctx->parent, GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Şemadan yükle…", GTK_RESPONSE_YES, "Live event ekle…", GTK_RESPONSE_APPLY,
        "Seçilen playlistleri ekle", GTK_RESPONSE_ACCEPT, NULL);
    /* gtk_dialog_run starts a nested loop; keep the owner and chooser alive
     * if a reload or parent close destroys either surface during that loop. */
    g_object_ref(editor->root); g_object_ref_sink(dialog);
    gulong close_handler = g_signal_connect_object(editor->root, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    named(dialog, "add-dialog"); gtk_window_set_default_size(GTK_WINDOW(dialog), 560, 520);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_box_set_spacing(GTK_BOX(content), 8);
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    GtkWidget *search = named(gtk_search_entry_new(), "add-search"); gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Playlist ara…");
    gtk_box_pack_start(GTK_BOX(content), search, FALSE, FALSE, 0);
    GtkListStore *store = gtk_list_store_new(PICK_COLUMNS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    for (guint i = 0; i < json_array_get_length(editor->playlists); ++i) {
        JsonNode *node = json_array_get_element(editor->playlists, i); if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
        JsonObject *playlist = json_node_get_object(node); const gchar *id = radios_json_string(playlist, "_id", ""), *purpose = radios_json_string(playlist, "purpose", "");
        if (!g_uuid_string_is_valid(id)) continue;
        const gchar *kind = playlist_kind_label(purpose);
        JsonArray *tracks = array_member(playlist, "tracks"); GtkTreeIter iter;
        gtk_list_store_insert_with_values(store, &iter, -1, PICK_ON, FALSE, PICK_ID, id, PICK_NAME, radios_json_string(playlist, "name", "Playlist"),
            PICK_KIND, kind, PICK_TRACKS, tracks ? (gint)json_array_get_length(tracks) : 0, -1);
    }
    GtkTreeModel *filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(store), NULL); g_object_unref(store);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(filter), pick_visible, search, NULL);
    GtkWidget *tree = named(gtk_tree_view_new_with_model(filter), "add-list"); g_object_unref(filter);
    GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new(); g_signal_connect(toggle, "toggled", G_CALLBACK(pick_toggled), filter);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), gtk_tree_view_column_new_with_attributes("Ekle", toggle, "active", PICK_ON, NULL));
    const gchar *titles[] = {"Playlist", "Tür", "Parça"}; const gint columns[] = {PICK_NAME, PICK_KIND, PICK_TRACKS};
    for (guint i = 0; i < G_N_ELEMENTS(titles); ++i) {
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], gtk_cell_renderer_text_new(), "text", columns[i], NULL);
        gtk_tree_view_column_set_expand(column, i == 0); gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
    }
    g_signal_connect(tree, "row-activated", G_CALLBACK(pick_activated), NULL);
    g_signal_connect(search, "search-changed", G_CALLBACK(pick_search), filter);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_container_add(GTK_CONTAINER(scroll), tree);
    gtk_widget_set_vexpand(scroll, TRUE); gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);
    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_ACCEPT && !editor->closed) {
        draft_capture(editor);
        guint added = 0, refused = 0; gpointer state[] = {editor, &added, &refused};
        gtk_tree_model_foreach(GTK_TREE_MODEL(store), pick_add, state);
        if (added) { render(editor, FALSE); emit_changed(editor); }
        gchar *message = !added && !refused ? g_strdup("Hiç playlist işaretlenmedi.") :
            refused ? g_strdup_printf("%u playlist eklendi; %u tanesi eklenemedi (en fazla 50 kaynak).", added, refused) :
            g_strdup_printf("%u playlist rotasyona eklendi.", added);
        status(editor, message); g_free(message);
    }
    if (g_signal_handler_is_connected(editor->root, close_handler)) g_signal_handler_disconnect(editor->root, close_handler);
    gtk_widget_destroy(dialog); g_object_unref(dialog);
    if (!editor->closed && response == GTK_RESPONSE_APPLY) step_add(NULL, editor);
    if (!editor->closed && response == GTK_RESPONSE_YES) schema_load(editor);
    g_object_unref(editor->root);
}
/* Source steps carry content and scheduling; their automation is assigned
 * separately on the schema page. Dialog drafts never mutate the definition. */
typedef struct {
    RotationEditor *editor;
    GtkWidget *dialog, *title, *label, *channels, *schema, *end_rule, *duration, *duration_caption, *start_date, *end_date, *message;
    JsonNode *base;
    gchar *definition_id, *source_id;
    gboolean live;
} StepDialog;
static void step_free(gpointer data)
{
    StepDialog *step = data; g_clear_pointer(&step->base, json_node_free);
    g_free(step->definition_id); g_free(step->source_id); g_free(step);
}
static void step_closed(GtkWidget *dialog, gpointer data)
{
    StepDialog *step = data;
    if (step->editor->step_dialog == dialog) step->editor->step_dialog = NULL;
}
static gboolean step_date_valid(const gchar *text)
{
    if (!*text) return TRUE;
    if (!g_regex_match_simple("^[0-9]{4}-[0-9]{2}-[0-9]{2}$", text, 0, 0)) return FALSE;
    guint year = (guint)g_ascii_strtoull(text, NULL, 10), month = (guint)g_ascii_strtoull(text + 5, NULL, 10), day = (guint)g_ascii_strtoull(text + 8, NULL, 10);
    return year >= 1 && year <= 9999 && g_date_valid_dmy(day, month, year);
}
/* Kılavuz s.4 adım 5 / s.7: when every labelled mixer strip typed into the
 * channels field carries the same DJ/Konuşma/Müzik label, the step's label
 * follows it; unlabelled or mixed strips leave the operator's choice. */
static void step_channels_changed(GtkWidget *picker, gpointer data)
{
    StepDialog *step = data; const gchar *agreed = radios_ui_channel_picker_agreed_label(picker);
    if (agreed) gtk_combo_box_set_active_id(GTK_COMBO_BOX(step->label), agreed);
}
/* Kullanıcı isteği (Foto 3): the duration shows only for a step that ends
 * after a duration; the dialog shrinks back around the form. */
static void step_end_changed(GtkComboBox *combo, gpointer data)
{
    StepDialog *step = data; gboolean duration = !g_strcmp0(gtk_combo_box_get_active_id(combo), "duration");
    gtk_widget_set_sensitive(step->duration, duration);
    gtk_widget_set_visible(step->duration, duration); gtk_widget_set_visible(step->duration_caption, duration);
}
static void step_response(GtkDialog *dialog, gint response, gpointer data)
{
    StepDialog *step = data; RotationEditor *editor = step->editor;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    const gchar *problem = NULL;
    if (!validate_stored(editor) || g_strcmp0(step->definition_id, radios_json_string(config_object(editor), "id", "")))
        problem = "Şema veya veri oturumu değişti; adımı yeniden açın.";
    JsonArray *sources = source_array(editor); JsonObject *existing = NULL; guint index = 0;
    for (guint i = 0; sources && step->source_id && i < json_array_get_length(sources); ++i)
        if (!g_strcmp0(step->source_id, radios_json_string(json_array_get_object_element(sources, i), "id", ""))) {
            existing = json_array_get_object_element(sources, i); index = i; break;
        }
    if (step->source_id) {
        JsonNode *current = json_node_new(JSON_NODE_OBJECT);
        if (existing) json_node_set_object(current, existing);
        if (!existing || !json_node_equal(current, step->base)) problem = "Bu adım başka bir işlemle değişti; ayarlarını yeniden açın.";
        json_node_free(current);
    } else if (!sources || json_array_get_length(sources) >= 50) problem = "Bir şema en fazla 50 yayın adımı içerebilir.";
    const gchar *start = gtk_entry_get_text(GTK_ENTRY(step->start_date)), *end = gtk_entry_get_text(GTK_ENTRY(step->end_date));
    if (!step_date_valid(start) || !step_date_valid(end) || (*start && *end && strcmp(start, end) > 0))
        problem = "Tarihleri YYYY-AA-GG biçiminde girin; bitiş başlangıçtan önce olamaz. Boş tarih sınırsızdır.";
    JsonArray *channels = json_array_new();
    const gchar *title = step->live ? gtk_entry_get_text(GTK_ENTRY(step->title)) : "";
    if (step->live) {
        gchar *trimmed = g_strdup(title); g_strstrip(trimmed);
        if (!*trimmed) problem = "Live event adı gerekli.";
        g_free(trimmed);
        JsonArray *chosen = radios_ui_channel_picker_selected(step->channels);
        for (guint i = 0; i < json_array_get_length(chosen); ++i) json_array_add_int_element(channels, json_array_get_int_element(chosen, i));
        json_array_unref(chosen);
        if (!g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(step->end_rule)), "duration") && !spin_valid(step->duration, 1, 86400))
            problem = "Süre 1–86400 saniye arasında olmalı.";
    }
    if (problem) { gtk_label_set_text(GTK_LABEL(step->message), problem); json_array_unref(channels); return; }
    JsonObject *source = existing ? copy_object(existing) : json_object_new();
    if (!existing) {
        gchar *id = g_uuid_string_random(); json_object_set_string_member(source, "id", id); g_free(id);
        json_object_set_int_member(source, "frequency", 1); json_object_set_int_member(source, "count", 1);
        json_object_set_int_member(source, "weight", 100); json_object_set_int_member(source, "start_minute", 0); json_object_set_int_member(source, "end_minute", 1440);
        json_object_set_int_member(source, "interval_minutes", 0);
        const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
        for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(source, flags[i], FALSE);
    }
    if (*start) json_object_set_string_member(source, "start_date", start); else json_object_remove_member(source, "start_date");
    if (*end) json_object_set_string_member(source, "end_date", end); else json_object_remove_member(source, "end_date");
    if (step->live) {
        JsonObject *live = json_object_new();
        json_object_set_string_member(live, "title", title);
        json_object_set_string_member(live, "label", gtk_combo_box_get_active_id(GTK_COMBO_BOX(step->label)));
        json_object_set_array_member(live, "channels", channels); channels = NULL;
        const gchar *schema = gtk_combo_box_get_active_id(GTK_COMBO_BOX(step->schema));
        json_object_set_string_member(live, "automation_id", schema ? schema : "");
        const gchar *rule = gtk_combo_box_get_active_id(GTK_COMBO_BOX(step->end_rule));
        json_object_set_string_member(live, "end_rule", rule);
        json_object_set_int_member(live, "duration_ms", !g_strcmp0(rule, "duration") ? (gint64)spin_read(step->duration) * 1000 : 0);
        json_object_set_string_member(source, "source_type", "live"); json_object_set_object_member(source, "live", live);
        json_object_remove_member(source, "playlist_id");
    }
    if (channels) json_array_unref(channels);
    draft_capture(editor);
    g_free(editor->selected); editor->selected = g_strdup(radios_json_string(source, "id", ""));
    if (existing) {
        JsonArray *replacement = json_array_new();
        for (guint i = 0; i < json_array_get_length(sources); ++i) {
            if (i == index) json_array_add_object_element(replacement, source);
            else json_array_add_element(replacement, copy_node(json_array_get_element(sources, i)));
        }
        json_object_set_array_member(config_object(editor), "sources", replacement);
    } else json_array_add_object_element(sources, source);
    gtk_widget_destroy(GTK_WIDGET(dialog)); render(editor, TRUE); schema_refresh(editor); emit_changed(editor);
}
static void step_open(RotationEditor *editor, gboolean adding)
{
    if (!validate_stored(editor)) return;
    if (editor->step_dialog) { gtk_window_present(GTK_WINDOW(editor->step_dialog)); return; }
    JsonObject *source = adding ? NULL : selected_source(editor, NULL);
    if (!adding && !source) { status(editor, source_array(editor) && json_array_get_length(source_array(editor)) ? "Önce listeden bir yayın adımı seçin." : "Yayın adımı yok; önce playlist ya da live event ekleyin."); return; }
    StepDialog *step = g_new0(StepDialog, 1); step->editor = editor;
    step->definition_id = g_strdup(radios_json_string(config_object(editor), "id", ""));
    if (source) {
        step->source_id = g_strdup(radios_json_string(source, "id", ""));
        JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, source); step->base = copy_node(node); json_node_free(node);
    }
    step->live = adding || object_member(source, "live") != NULL;
    GtkWidget *parent = gtk_widget_get_toplevel(editor->root);
    GtkWidget *dialog = gtk_dialog_new_with_buttons(step->live ? "Live event ayarları" : "Playlist tarih aralığı", GTK_IS_WINDOW(parent) ? GTK_WINDOW(parent) : editor->ctx->parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    step->dialog = editor->step_dialog = named(dialog, "step-dialog");
    /* Kullanıcı isteği (Foto 3): a compact two-column form, no empty area. */
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 8); gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 12); gint row = 0;
    step->start_date = named(gtk_entry_new(), "step-start-date"); step->end_date = named(gtk_entry_new(), "step-end-date");
    gtk_entry_set_placeholder_text(GTK_ENTRY(step->start_date), "YYYY-AA-GG · isteğe bağlı");
    gtk_entry_set_placeholder_text(GTK_ENTRY(step->end_date), "YYYY-AA-GG · isteğe bağlı");
    gtk_entry_set_width_chars(GTK_ENTRY(step->start_date), 22); gtk_entry_set_width_chars(GTK_ENTRY(step->end_date), 22);
    gtk_entry_set_text(GTK_ENTRY(step->start_date), radios_json_string(source, "start_date", ""));
    gtk_entry_set_text(GTK_ENTRY(step->end_date), radios_json_string(source, "end_date", ""));
    if (step->live) {
        JsonObject *live = object_member(source, "live");
        step->title = named(gtk_entry_new(), "step-title"); gtk_entry_set_max_length(GTK_ENTRY(step->title), 160);
        gtk_entry_set_width_chars(GTK_ENTRY(step->title), 22);
        gtk_entry_set_text(GTK_ENTRY(step->title), radios_json_string(live, "title", "Konuşma")); grid_field(grid, "Ad", step->title, 0, row);
        step->label = named(gtk_combo_box_text_new(), "step-label");
        const gchar *ids[] = {"talk", "dj", "music"}, *names[] = {"Konuşma", "DJ", "Müzik"};
        for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(step->label), ids[i], names[i]);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(step->label), radios_json_string(live, "label", "talk")); grid_field(grid, "Tür", step->label, 1, row++);
        step->channels = radios_ui_channel_picker_new("rotation-editor-step-channels");
        JsonArray *strips = channel_list(editor); radios_ui_channel_picker_set_channels(step->channels, strips); json_array_unref(strips);
        radios_ui_channel_picker_set_selected(step->channels, array_member(live, "channels"));
        gtk_widget_set_halign(step->channels, GTK_ALIGN_FILL); grid_field(grid, "Mixer kanalları", step->channels, 0, row);
        radios_ui_channel_picker_set_changed(step->channels, step_channels_changed, step);
        step->schema = named(gtk_combo_box_text_new(), "step-schema"); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(step->schema), "", "Özel şema yok");
        const gchar *current = radios_json_string(live, "automation_id", ""); gboolean found = !*current;
        for (guint i = 0; editor->schemas && i < json_array_get_length(editor->schemas); ++i) {
            JsonObject *def = json_array_get_object_element(editor->schemas, i);
            if (!bool_member(object_member(def, "settings"), "automation_schema")) continue;
            const gchar *id = radios_json_string(def, "id", "");
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(step->schema), id, radios_json_string(def, "name", "Şema")); found |= !g_strcmp0(current, id);
        }
        if (!found) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(step->schema), current, "Kayıtlı şema · şu an kullanılamıyor");
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(step->schema), current); grid_field(grid, "Live event şeması", step->schema, 1, row++);
        step->end_rule = named(gtk_combo_box_text_new(), "step-end-rule");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(step->end_rule), "manual", "Operatör tamamlayınca");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(step->end_rule), "duration", "Süre sonunda");
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(step->end_rule), radios_json_string(live, "end_rule", "manual")); grid_field(grid, "Bitiş şekli", step->end_rule, 0, row);
        step->duration = named(gtk_spin_button_new_with_range(1, 86400, 1), "step-duration");
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(step->duration), MAX(1, radios_json_int(live, "duration_ms", 60000) / 1000));
        step->duration_caption = label("Süre (saniye)", "re-caption");
        gtk_grid_attach(GTK_GRID(grid), step->duration_caption, 2, row, 1, 1); gtk_grid_attach(GTK_GRID(grid), step->duration, 3, row++, 1, 1);
        gtk_widget_set_no_show_all(step->duration_caption, TRUE); gtk_widget_set_no_show_all(step->duration, TRUE);
        g_signal_connect(step->end_rule, "changed", G_CALLBACK(step_end_changed), step); step_end_changed(GTK_COMBO_BOX(step->end_rule), step);
    }
    grid_field(grid, "Başlangıç tarihi", step->start_date, 0, row); grid_field(grid, "Bitiş tarihi", step->end_date, 1, row++);
    step->message = named(label("", "error"), "step-status"); gtk_label_set_line_wrap(GTK_LABEL(step->message), TRUE);
    gtk_grid_attach(GTK_GRID(grid), step->message, 0, row, 4, 1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid, FALSE, FALSE, 0);
    g_object_set_data_full(G_OBJECT(dialog), "step-draft", step, step_free);
    g_signal_connect(dialog, "response", G_CALLBACK(step_response), step); g_signal_connect(dialog, "destroy", G_CALLBACK(step_closed), step);
    g_signal_connect_object(editor->root, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED); gtk_widget_show_all(dialog);
}
static void step_edit(GtkButton *button, gpointer data) { (void)button; step_open(data, FALSE); }
static void step_add(GtkButton *button, gpointer data) { (void)button; step_open(data, TRUE); }

static void schema_remap_entry(JsonObject *entry, guint index, gint other, guint remaining)
{
    if (!entry) return;
    gint64 order = radios_json_int(entry, "source_order", 1), position = order - 1;
    if (other >= 0) {
        if (position == (gint64)index) order = other + 1;
        else if (position == other) order = index + 1;
    } else if (position > (gint64)index) --order;
    else if (position == (gint64)index) order = MIN(index + 1, remaining);
    json_object_set_int_member(entry, "source_order", MAX(1, order));
}
static void schema_source_references(RotationEditor *editor, guint index, gint other)
{
    JsonObject *settings = object_member(config_object(editor), "settings");
    if (!settings) return;
    guint remaining = json_array_get_length(source_array(editor)) - (other < 0);
    if (other < 0) {
        JsonArray *links = array_member(settings, "link_transitions");
        for (guint i = 0; links && i < json_array_get_length(links);) {
            JsonObject *link = json_array_get_object_element(links, i);
            if (!g_strcmp0(radios_json_string(link, "from_source_id", ""), editor->selected) ||
                !g_strcmp0(radios_json_string(link, "to_source_id", ""), editor->selected)) json_array_remove_element(links, i);
            else ++i;
        }
    }
    JsonObject *entry = object_member(settings, "programme_entry"); if (!entry) return;
    if (!remaining) { json_object_remove_member(settings, "programme_entry"); return; }
    schema_remap_entry(entry, index, other, remaining);
    JsonArray *alternatives = array_member(entry, "alternatives");
    for (guint i = 0; alternatives && i < json_array_get_length(alternatives);) {
        JsonObject *alternative = json_array_get_object_element(alternatives, i);
        if (other < 0 && radios_json_int(alternative, "source_order", 1) == (gint64)index + 1) {
            json_array_remove_element(alternatives, i); continue;
        }
        schema_remap_entry(alternative, index, other, remaining); ++i;
    }
}

static void source_action(GtkButton *button, gpointer data)
{
    RotationEditor *editor = data; guint index = 0;
    if (editor->closed || !validate_stored(editor) || !selected_source(editor, &index)) return;
    draft_capture(editor);
    JsonArray *sources = source_array(editor); guint count = json_array_get_length(sources);
    if (GTK_WIDGET(button) == editor->remove) {
        schema_source_references(editor, index, -1);
        gchar *prefix = g_strconcat(editor->selected, "/", NULL);
        GHashTableIter drafts; gpointer key;
        g_hash_table_iter_init(&drafts, editor->draft_fields);
        while (g_hash_table_iter_next(&drafts, &key, NULL)) if (g_str_has_prefix(key, prefix)) g_hash_table_iter_remove(&drafts);
        g_free(prefix); g_clear_pointer(&editor->selected_song, g_free);
        json_array_remove_element(sources, index); g_clear_pointer(&editor->selected, g_free);
        if (json_array_get_length(sources)) {
            JsonObject *source = json_array_get_object_element(sources, MIN(index, json_array_get_length(sources) - 1));
            editor->selected = g_strdup(radios_json_string(source, "id", ""));
        }
    } else {
        gint target = GTK_WIDGET(button) == editor->up ? (gint)index - 1 : (gint)index + 1;
        if (target < 0 || target >= (gint)count) return;
        schema_source_references(editor, index, target);
        JsonArray *replacement = json_array_new();
        for (guint i = 0; i < count; ++i) {
            guint from = i == index ? (guint)target : i == (guint)target ? index : i;
            json_array_add_element(replacement, json_node_copy(json_array_get_element(sources, from)));
        }
        json_object_set_array_member(config_object(editor), "sources", replacement);
    }
    render(editor, TRUE); emit_changed(editor);
}
static void go_back(GtkButton *button, gpointer data)
{
    (void)button; RotationEditor *editor = data;
    if (!validate_stored(editor)) return;
    editor->updating = TRUE; sources_render(editor); editor->updating = FALSE;
    gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), "library");
}
static void reset_song_rules(GtkButton *button, gpointer data)
{
    (void)button; RotationEditor *editor = data;
    JsonObject *source = selected_source(editor, NULL);
    if (editor->closed || editor->updating || !source || !validate_stored(editor)) return;
    if (!json_object_has_member(source, "track_rules")) return;
    draft_capture(editor);
    json_object_remove_member(source, "track_rules");
    render(editor, TRUE); emit_changed(editor);
}
typedef struct {
    GWeakRef editor;
    RadioSDataClient *client;
    gchar *rotation_id, *source_id;
} RotationProbabilityOpen;
static void probability_open_free(gpointer data)
{
    RotationProbabilityOpen *opened = data; g_weak_ref_clear(&opened->editor);
    g_clear_pointer(&opened->client, radios_data_client_unref);
    g_free(opened->rotation_id); g_free(opened->source_id); g_free(opened);
}
static JsonObject *source_by_id(RotationEditor *editor, const gchar *id)
{
    JsonArray *sources = source_array(editor);
    for (guint i = 0; id && sources && i < json_array_get_length(sources); ++i) {
        JsonObject *source = json_array_get_object_element(sources, i);
        if (!g_strcmp0(radios_json_string(source, "id", ""), id)) return source;
    }
    return NULL;
}
static void probability_changed(GtkWidget *widget, JsonNode *snapshot, gpointer data)
{
    (void)widget; RotationProbabilityOpen *opened = data; GtkWidget *owner = g_weak_ref_get(&opened->editor);
    RotationEditor *editor = editor_get(owner);
    RadioSDataClient *current = editor ? radios_data_client_acquire(editor->ctx) : NULL;
    gboolean same = editor && !editor->closed && radios_data_client_same_identity(current, opened->client) &&
        !g_strcmp0(radios_json_string(config_object(editor), "id", ""), opened->rotation_id);
    g_clear_pointer(&current, radios_data_client_unref);
    if (!same || !validate_stored(editor)) { g_clear_object(&owner); return; }
    JsonObject *target = opened->source_id ? source_by_id(editor, opened->source_id) : config_object(editor);
    if (!target || !snapshot || (!JSON_NODE_HOLDS_OBJECT(snapshot) && !JSON_NODE_HOLDS_NULL(snapshot))) { g_clear_object(&owner); return; }
    JsonObject *previous = object_member(target, "settings"); JsonObject *settings = previous ? copy_object(previous) : json_object_new();
    /* An emptied policy is removed, so weights and linear order apply again. */
    if (JSON_NODE_HOLDS_NULL(snapshot)) json_object_remove_member(settings, "probability_policy");
    else json_object_set_member(settings, "probability_policy", copy_node(snapshot));
    if (opened->source_id && !json_object_get_size(settings)) { json_object_remove_member(target, "settings"); json_object_unref(settings); }
    else json_object_set_object_member(target, "settings", settings);
    /* The Pay column shows the new percentages at once. */
    draft_capture(editor); editor->updating = TRUE; sources_render(editor); songs_render(editor); editor->updating = FALSE; draft_restore(editor);
    emit_changed(editor); g_clear_object(&owner);
}
static gboolean rotation_has_playlist(RotationEditor *editor, const gchar *id)
{
    JsonArray *sources = source_array(editor);
    for (guint i = 0; sources && i < json_array_get_length(sources); ++i)
        if (!g_strcmp0(radios_json_string(json_array_get_object_element(sources, i), "playlist_id", ""), id)) return TRUE;
    return FALSE;
}
/* The percentage table lists this rotation's playlists and their songs.
 * Song shares stay at rotation level: a playlist's own policy would replace
 * the rotation's playlist shares in the planner. That window therefore only
 * offers transitions and jingles; other jingle playlists are sources only. */
static JsonArray *probability_targets(RotationEditor *editor, gboolean playlist_level)
{
    JsonArray *targets = json_array_new(); JsonObject *config = config_object(editor), *automation = json_object_new();
    json_object_set_string_member(automation, "id", radios_json_string(config, "id", ""));
    json_object_set_string_member(automation, "scope", "automation"); json_object_set_string_member(automation, "name", radios_json_string(config, "name", "Rotasyon"));
    json_object_set_boolean_member(automation, "listed", FALSE); json_array_add_object_element(targets, automation);
    if (!catalog_current(editor)) return targets;
    for (guint i = 0; editor->playlists && i < json_array_get_length(editor->playlists); ++i) {
        JsonObject *playlist = json_array_get_object_element(editor->playlists, i); const gchar *id = radios_json_string(playlist, "_id", "");
        const gchar *purpose = radios_json_string(playlist, "purpose", "music"); gboolean member = rotation_has_playlist(editor, id);
        if (!g_uuid_string_is_valid(id) || (!member && g_strcmp0(purpose, "jingle"))) continue;
        JsonObject *target = json_object_new(); json_object_set_string_member(target, "id", id);
        json_object_set_string_member(target, "scope", "playlist"); json_object_set_string_member(target, "name", radios_json_string(playlist, "name", "Playlist"));
        json_object_set_string_member(target, "purpose", purpose); json_object_set_boolean_member(target, "listed", member && !playlist_level);
        json_array_add_object_element(targets, target);
        if (!member || playlist_level) continue;
        JsonArray *tracks = array_member(playlist, "tracks");
        for (guint j = 0; tracks && j < json_array_get_length(tracks); ++j) {
            JsonObject *track = json_array_get_object_element(tracks, j); const gchar *entry = track_id(track);
            if (!g_uuid_string_is_valid(entry)) continue;
            JsonObject *metadata = object_member(track, "metadata"); if (!metadata) metadata = track;
            target = json_object_new(); json_object_set_string_member(target, "id", entry); json_object_set_string_member(target, "scope", "track");
            json_object_set_string_member(target, "playlist_id", id);
            gchar *name = g_strdup_printf("%s · %s", radios_json_string(playlist, "name", "Playlist"), radios_json_string(metadata, "title", "Şarkı"));
            json_object_set_string_member(target, "name", name); g_free(name); json_array_add_object_element(targets, target);
        }
    }
    return targets;
}
static void probability_dialog_closed(GtkWidget *dialog, gpointer data)
{
    RotationEditor *editor = data; if (editor->probability_dialog == dialog) editor->probability_dialog = NULL;
}
/* ---- Geçiş dağılımı (kullanıcı isteği 18.10: "o eski olasılık dağılımı
 * kalkmış, onu getir") -------------------------------------------------------
 * The rotation's own share of each transition type with its duration and
 * low-pass, drawn by probability or taken in turn: the entries of
 * settings.transitions without "kind" and transition_selection_mode, the
 * "normal geçiş kuralları" the planner applies whenever no probability rule,
 * schema link or kind distribution decides a boundary, and the rest of every
 * kind's shares. The shared table (transition_distribution.c, same widget
 * names) writes only these entries and keeps the kinds' own (kılavuz 18.10). */
static void transitions_changed(GtkWidget *widget, gpointer data)
{
    RotationEditor *editor = data;
    if (editor->updating || editor->closed || !validate_stored(editor)) return;
    JsonObject *config = config_object(editor), *previous = object_member(config, "settings"); if (!previous) return;
    gchar *problem = NULL;
    if (!radios_transition_distribution_valid(widget, &problem)) { status(editor, problem); g_free(problem); return; }
    JsonObject *settings = copy_object(previous);
    radios_transition_distribution_read(widget, settings);
    json_object_set_object_member(config, "settings", settings);
    emit_changed(editor);
}
static GtkWidget *transitions_widget_new(RotationEditor *editor)
{
    RadioSTransitionDistributionHooks hooks = {transitions_changed, editor, NULL};
    GtkWidget *base = radios_transition_distribution_new(NULL, FALSE, &hooks);
    radios_transition_distribution_render(base, object_member(config_object(editor), "settings"));
    gtk_widget_set_tooltip_text(base, "Olasılık kuralı, şema bağlantısı ya da tür dağılımı olmayan her geçişte ve türlerin kalan payında uygulanır.");
    return base;
}
static GtkWidget *probability_widget_new(RotationEditor *editor, gboolean source, gboolean songs)
{
    JsonObject *target = source ? selected_source(editor, NULL) : config_object(editor);
    RotationProbabilityOpen *opened = g_new0(RotationProbabilityOpen, 1); g_weak_ref_init(&opened->editor, editor->root);
    opened->client = radios_data_client_acquire(editor->ctx); opened->rotation_id = g_strdup(radios_json_string(config_object(editor), "id", ""));
    opened->source_id = source ? g_strdup(radios_json_string(target, "id", "")) : NULL;
    RadioSProbabilityEditorHooks hooks = {probability_changed, opened, probability_open_free};
    GtkWidget *probability = radios_probability_editor_new(editor->ctx, &hooks);
    JsonArray *targets = probability_targets(editor, source);
    radios_probability_editor_set_targets(probability, targets); json_array_unref(targets);
    radios_probability_editor_set_policy(probability, object_member(object_member(target, "settings"), "probability_policy"));
    if (songs) radios_probability_editor_show_scope(probability, "track");
    return probability;
}
static void probability_back(GtkButton *button, gpointer data)
{
    (void)button; RotationEditor *editor = data;
    const gchar *back = g_object_get_data(G_OBJECT(editor->probability_dialog), "return-page");
    gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), back ? back : "library");
    gtk_widget_destroy(editor->probability_dialog);
}
static void open_probability(GtkButton *button, gpointer data)
{
    RotationEditor *editor = data; if (editor->closed || !validate_stored(editor)) return;
    if (!catalog_current(editor)) { status(editor, "Playlist kataloğu henüz yüklenmedi; birkaç saniye sonra yeniden deneyin."); return; }
    const gchar *name = gtk_widget_get_name(GTK_WIDGET(button));
    gboolean source = !g_strcmp0(name, "rotation-editor-source-probability");
    if (source) {
        source_autodj(NULL, editor);
        if (editor->routing_dialog) {
            RoutingDialog *routing = g_object_get_data(G_OBJECT(editor->routing_dialog), "radios-routing-dialog");
            gtk_notebook_set_current_page(GTK_NOTEBOOK(routing->tabs), 1);
        }
        return;
    }
    if (editor->probability_dialog) gtk_widget_destroy(editor->probability_dialog);
    GtkWidget *page = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, NULL), "probability-dialog");
    editor->probability_dialog = page;
    g_object_set_data_full(G_OBJECT(page), "return-page",
        g_strdup(gtk_stack_get_visible_child_name(GTK_STACK(editor->pages))), g_free);
    GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    GtkWidget *back = named(radios_ui_button("← Geri"), "probability-back");
    g_signal_connect(back, "clicked", G_CALLBACK(probability_back), editor);
    gtk_box_pack_start(GTK_BOX(heading), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(heading), label("Otomasyon ayarları", "re-heading"), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);
    gboolean songs = !g_strcmp0(name, "rotation-editor-song-probability");
    if (!songs) gtk_box_pack_start(GTK_BOX(page), transitions_widget_new(editor), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), probability_widget_new(editor, FALSE, songs), TRUE, TRUE, 0);
    g_signal_connect(page, "destroy", G_CALLBACK(probability_dialog_closed), editor);
    gtk_stack_add_named(GTK_STACK(editor->pages), page, "probability");
    gtk_widget_show_all(page); gtk_stack_set_visible_child(GTK_STACK(editor->pages), page);
}
/* "AutoDJ otomasyonu": the rotation's AutoDJ transitions on one page. Its
 * "Türler" rows carry the kind rule and players built here by autodj_page
 * (read, checked and written by this editor as before) beside the kind's
 * designed presets; below follow the presets and their clock/day schedules.
 * The footer's "AutoDJ: geçiş ve kanallar…" opens the same page at "Türler"
 * (C8, 18.10: the former separate kind/channel page is merged here). */
static void automation_back(GtkButton *button, gpointer data)
{
    (void)button; RotationEditor *editor = data;
    const gchar *back = g_object_get_data(G_OBJECT(editor->automation_page), "return-page");
    gboolean known = back && g_strcmp0(back, "autodj-automation") && gtk_stack_get_child_by_name(GTK_STACK(editor->pages), back);
    gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), known ? back : "library");
}
static void open_automation(GtkButton *button, gpointer data)
{
    (void)button; RotationEditor *editor = data; if (editor->closed) return;
    const gchar *current = gtk_stack_get_visible_child_name(GTK_STACK(editor->pages));
    if (g_strcmp0(current, "autodj-automation"))
        g_object_set_data_full(G_OBJECT(editor->automation_page), "return-page", g_strdup(current ? current : "library"), g_free);
    gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), "autodj-automation");
}
static void open_autodj_policy(GtkButton *button, gpointer data)
{
    RotationEditor *editor = data; if (editor->closed) return;
    open_automation(button, editor); radios_autodj_automation_show_kind_policy(editor->automation_body);
}
static void automation_changed(GtkWidget *body, gpointer data)
{ (void)body; RotationEditor *editor = data; changed(editor->automation_page, editor); schema_refresh(editor); }
/* The kind rule box keeps its former page name for callers that look it up. */
static GtkWidget *automation_kind_policy(GtkWidget *body, gpointer data)
{
    (void)body; GtkWidget *policy = autodj_page(data);
    gtk_widget_set_name(policy, "rotation-editor-autodj-policy-page"); return policy;
}
static GtkWidget *automation_page_new(RotationEditor *editor)
{
    GtkWidget *page = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, NULL), "autodj-automation-page");
    GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    GtkWidget *back = named(radios_ui_button("← Geri"), "autodj-automation-back");
    g_signal_connect(back, "clicked", G_CALLBACK(automation_back), editor);
    gtk_box_pack_start(GTK_BOX(heading), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(heading), label("AutoDJ otomasyonu", "re-heading"), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);
    editor->automation_page = page;
    /* Kind distributions only while the connected data service reads them. */
    RadioSDataClient *client = radios_data_client_acquire(editor->ctx);
    gboolean distributions = client && radios_data_client_has_feature(client, "kind_transition_distribution");
    if (client) radios_data_client_unref(client);
    g_object_set_data(G_OBJECT(page), "radios-kind-distributions", GINT_TO_POINTER(distributions));
    RadioSAutodjAutomationHooks hooks = { .changed = automation_changed, .user = editor, .kind_policy = automation_kind_policy,
        .kind_distributions = distributions };
    editor->automation_body = radios_autodj_automation_new(editor->ctx, &hooks);
    gtk_box_pack_start(GTK_BOX(page), editor->automation_body, TRUE, TRUE, 0);
    return page;
}
/* The schema assigns existing automation to typed steps; its structural
 * editor is independent of the AutoDJ transition designer. */
static void schema_refresh(RotationEditor *editor)
{
    if (editor->schema_body) radios_schema_settings_set_config(editor->schema_body, config_object(editor));
}
/* Without the parent's mixer list every strip is offered with the label the
 * audio layer knows (a detached editor, a test harness). */
static JsonArray *channel_list(RotationEditor *editor)
{
    if (editor->channels) return json_array_ref(editor->channels);
    JsonArray *channels = json_array_new();
    for (guint number = 1; number <= 32; ++number) {
        const gchar *label = editor->ctx ? radios_audio_ui_get_channel_label(editor->ctx, number) : NULL;
        JsonObject *row = json_object_new(); gchar *name = g_strdup_printf("Kanal %u", number);
        json_object_set_int_member(row, "number", number); json_object_set_string_member(row, "name", name);
        json_object_set_string_member(row, "label", label ? label : ""); json_object_set_string_member(row, "source", "");
        json_array_add_object_element(channels, row); g_free(name);
    }
    return channels;
}
/* The schema page's live context (kullanıcı isteği: şema kümeli oynatıcılara
 * etki etmeli): the players and clusters of the player cards, the defined
 * mixer strips and the station's rotation library (its automations). */
static void schema_context(RotationEditor *editor)
{
    if (!editor->schema_body) return;
    JsonArray *players = json_array_new();
    for (guint deck = 0; editor->ctx && editor->ctx->parent && deck < AUTODJ_PLAYER_COUNT; ++deck) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
        GtkWidget *card = g_object_get_data(G_OBJECT(editor->ctx->parent), key); g_free(key);
        if (!card || gtk_widget_in_destruction(card)) continue;
        const gchar *name = g_object_get_data(G_OBJECT(card), "radios-player-name");
        const gchar *cluster = g_object_get_data(G_OBJECT(card), "radios-player-cluster-id"), *cluster_name = g_object_get_data(G_OBJECT(card), "radios-player-cluster-name");
        JsonObject *row = json_object_new(); gchar *fallback = g_strdup_printf("Oynatıcı %u", deck + 1);
        json_object_set_int_member(row, "deck", deck + 1); json_object_set_string_member(row, "name", name && *name ? name : fallback);
        json_object_set_string_member(row, "cluster_id", cluster ? cluster : ""); json_object_set_string_member(row, "cluster_name", cluster_name ? cluster_name : "");
        json_array_add_object_element(players, row); g_free(fallback);
    }
    radios_schema_settings_set_players(editor->schema_body, players); json_array_unref(players);
    JsonArray *channels = channel_list(editor); radios_schema_settings_set_channels(editor->schema_body, channels); json_array_unref(channels);
    radios_schema_settings_set_library(editor->schema_body, editor->schemas);
}
static void schema_changed(GtkWidget *widget, JsonNode *snapshot, gpointer data)
{
    (void)widget; RotationEditor *editor = data;
    if (editor->updating || editor->closed || !validate_stored(editor) || !snapshot || !JSON_NODE_HOLDS_OBJECT(snapshot) ||
        g_strcmp0(radios_json_string(json_node_get_object(snapshot), "id", ""), radios_json_string(config_object(editor), "id", ""))) return;
    JsonNode *replacement = copy_node(snapshot); json_node_free(editor->config); editor->config = replacement;
    draft_capture(editor); render(editor, TRUE); emit_changed(editor);
}
static void schema_back(GtkButton *button, gpointer data)
{
    (void)button; RotationEditor *editor = data; gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), "library");
}
static void schema_routing(GtkWidget *widget, const gchar *source_id, gpointer data)
{
    (void)widget; RotationEditor *editor = data;
    if (!validate_stored(editor)) return;
    g_free(editor->selected); editor->selected = g_strdup(source_id); g_clear_pointer(&editor->selected_song, g_free);
    editor->updating = TRUE; sources_render(editor); render_source(editor); editor->updating = FALSE;
    /* render_source may select the library when no songs are present. */
    gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), "schema-settings");
    source_autodj(NULL, editor);
}
void radios_rotation_editor_open_schema_settings(GtkWidget *widget)
{
    RotationEditor *editor = editor_get(widget); if (!editor || !validate_stored(editor)) return;
    if (!editor->schema_page) {
        editor->schema_page = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 10, NULL), "schema-settings-page");
        GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
        GtkWidget *back = named(radios_ui_button("← Playlist ve live eventler"), "schema-settings-back");
        g_signal_connect(back, "clicked", G_CALLBACK(schema_back), editor);
        gtk_box_pack_start(GTK_BOX(heading), back, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(heading), label("Şema Ayarları", "re-heading"), TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(editor->schema_page), heading, FALSE, FALSE, 0);
        editor->schema_body = radios_schema_settings_new(config_object(editor), schema_changed, editor);
        radios_schema_settings_set_routing_handler(editor->schema_body, schema_routing, editor);
        gtk_box_pack_start(GTK_BOX(editor->schema_page), editor->schema_body, TRUE, TRUE, 0);
        gtk_stack_add_named(GTK_STACK(editor->pages), editor->schema_page, "schema-settings");
    }
    schema_refresh(editor); radios_schema_settings_set_playlists(editor->schema_body, editor->playlists); schema_context(editor);
    gtk_widget_show_all(editor->schema_page); gtk_stack_set_visible_child_name(GTK_STACK(editor->pages), "schema-settings");
}
static void open_schema_settings(GtkButton *button, gpointer data)
{ (void)button; radios_rotation_editor_open_schema_settings(((RotationEditor *)data)->root); }
void radios_rotation_editor_set_schemas(GtkWidget *widget, JsonArray *definitions)
{
    RotationEditor *editor = editor_get(widget); if (!editor || editor->closed) return;
    JsonArray *copy = NULL;
    if (definitions) {
        JsonNode *node = json_node_new(JSON_NODE_ARRAY); json_node_set_array(node, definitions);
        JsonNode *cloned = copy_node(node); json_node_free(node); copy = json_array_ref(json_node_get_array(cloned)); json_node_free(cloned);
    }
    g_clear_pointer(&editor->schemas, json_array_unref); editor->schemas = copy;
    if (editor->schema_body) radios_schema_settings_set_library(editor->schema_body, editor->schemas);
}
void radios_rotation_editor_set_channels(GtkWidget *widget, JsonArray *channels)
{
    RotationEditor *editor = editor_get(widget); if (!editor || editor->closed) return;
    JsonArray *copy = NULL;
    if (channels) {
        JsonNode *node = json_node_new(JSON_NODE_ARRAY); json_node_set_array(node, channels);
        JsonNode *cloned = copy_node(node); json_node_free(node); copy = json_array_ref(json_node_get_array(cloned)); json_node_free(cloned);
    }
    g_clear_pointer(&editor->channels, json_array_unref); editor->channels = copy;
    if (editor->schema_body) { JsonArray *list = channel_list(editor); radios_schema_settings_set_channels(editor->schema_body, list); json_array_unref(list); }
}

static void page_changed(GObject *stack, GParamSpec *property, gpointer data)
{
    (void)property; RotationEditor *editor = data;
    gboolean songs = !g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(stack)), "songs");
    if (editor->global_settings) gtk_widget_set_visible(editor->global_settings, songs);
    if (editor->source_properties) gtk_widget_set_visible(editor->source_properties, songs);
    gboolean library = !g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(stack)), "library");
    if (editor->library_toolbar) gtk_widget_set_visible(editor->library_toolbar, library);
    if (editor->library_footer) gtk_widget_set_visible(editor->library_footer, library);
    if (editor->name_row) gtk_widget_set_visible(editor->name_row, library || songs);
    if (editor->total) gtk_widget_set_visible(editor->total, library || songs);
}

static GtkWidget *action(RotationEditor *editor, const gchar *text, const gchar *id, GCallback callback)
{
    GtkWidget *button = named(radios_ui_button(text), id);
    g_signal_connect(button, "clicked", callback, editor); return button;
}
static GtkWidget *source_page(RotationEditor *editor)
{
    GtkWidget *page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "re-song-page");
    GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    editor->back = action(editor, "← Playlistler", "back", G_CALLBACK(go_back));
    editor->heading = named(label("Playlist seçilmedi", "re-heading"), "song-heading");
    gtk_label_set_ellipsize(GTK_LABEL(editor->heading), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(heading), editor->back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(heading), editor->heading, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);

    GtkWidget *song_heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    gtk_box_pack_start(GTK_BOX(song_heading), label("Şarkı kuralları", "re-heading"), TRUE, TRUE, 0);
    GtkWidget *song_probability = action(editor, "Olasılık…", "song-probability", G_CALLBACK(open_probability));
    gtk_style_context_add_class(gtk_widget_get_style_context(song_probability), "suggested-action");
    gtk_box_pack_start(GTK_BOX(song_heading), song_probability, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), song_heading, FALSE, FALSE, 0);
    editor->song_store = gtk_list_store_new(SONG_COLUMNS,G_TYPE_STRING,G_TYPE_INT,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_INT,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_INT);
    editor->songs = named(gtk_tree_view_new_with_model(GTK_TREE_MODEL(editor->song_store)), "songs");
    table_column(editor->songs, "#", SONG_NUMBER, FALSE);
    table_column(editor->songs, "Şarkı", SONG_TITLE, TRUE);
    table_column(editor->songs, "Sanatçı", SONG_ARTIST, TRUE);
    table_column(editor->songs, "Süre", SONG_DURATION, FALSE);
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(editor->song_store),SONG_NUMBER,song_sort,NULL,NULL);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(editor->song_store),SONG_NUMBER,GTK_SORT_ASCENDING);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->songs)), "changed", G_CALLBACK(song_selected), editor);
    gtk_box_pack_start(GTK_BOX(page), table_scrolled(editor->songs, 200), TRUE, TRUE, 0);

    GtkWidget *song_advanced = named(gtk_expander_new("Gelişmiş · seçili şarkı"), "song-advanced");
    editor->song_properties = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "re-policy-panel"), "song-properties");
    editor->song_summary = named(label("", "re-caption"), "song-summary");
    gtk_label_set_line_wrap(GTK_LABEL(editor->song_summary), TRUE);
    gtk_box_pack_start(GTK_BOX(editor->song_properties), editor->song_summary, FALSE, FALSE, 0);
    GtkWidget *song_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(song_grid), 6); gtk_grid_set_column_spacing(GTK_GRID(song_grid), 10);
    editor->song_weight = spin(editor, "song-weight", 0, 100);
    editor->song_order = spin(editor, "song-order", 1, 5000);
    editor->song_frequency = spin(editor, "song-frequency", 1, 100);
    editor->song_max = spin(editor, "song-max-plays", 0, 200);
    editor->song_interval = spin(editor, "song-interval", 0, 10080);
    editor->song_start = named(radios_ui_entry("00:00"), "song-start");
    editor->song_end = named(radios_ui_entry("24:00"), "song-end");
    GtkWidget *times[] = {editor->song_start, editor->song_end};
    for (guint i = 0; i < G_N_ELEMENTS(times); ++i) {
        gtk_entry_set_width_chars(GTK_ENTRY(times[i]), 5); gtk_entry_set_max_length(GTK_ENTRY(times[i]), 5);
        g_signal_connect(times[i], "changed", G_CALLBACK(changed), editor);
    }
    grid_field(song_grid, "Ağırlık (0 = kapalı)", editor->song_weight, 0, 0);
    grid_field(song_grid, "Sıra", editor->song_order, 1, 0);
    grid_field(song_grid, "Her N turda", editor->song_frequency, 0, 1);
    grid_field(song_grid, "En fazla (0 = sınırsız)", editor->song_max, 1, 1);
    grid_field(song_grid, "Başlangıç", editor->song_start, 0, 2);
    grid_field(song_grid, "Bitiş", editor->song_end, 1, 2);
    grid_field(song_grid, "En az aralık (dk)", editor->song_interval, 0, 3);
    gtk_box_pack_start(GTK_BOX(editor->song_properties), song_grid, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(song_advanced), editor->song_properties);
    gtk_box_pack_start(GTK_BOX(page), song_advanced, FALSE, FALSE, 0);

    editor->source_properties = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, NULL), "source-properties");
    editor->source_summary = named(label("", "re-caption"), "selected-source-summary");
    gtk_label_set_line_wrap(GTK_LABEL(editor->source_summary), TRUE);
    gtk_box_pack_start(GTK_BOX(editor->source_properties), editor->source_summary, FALSE, FALSE, 0);
    GtkWidget *simple = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    const gchar *orders[] = {"Sırayla çal", "Karıştır", "Olasılığa göre çal", NULL};
    editor->track_order = named(radios_ui_combo(orders, 0), "source-track-order");
    g_signal_connect(editor->track_order, "changed", G_CALLBACK(changed), editor);
    gtk_box_pack_start(GTK_BOX(simple), label("Şarkı sırası", "re-caption"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(simple), editor->track_order, TRUE, TRUE, 0);
    GtkWidget *probability = action(editor, "Oynatıcı ve geçiş ayarları…", "source-probability", G_CALLBACK(open_probability));
    gtk_box_pack_start(GTK_BOX(simple), probability, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(editor->source_properties), simple, FALSE, FALSE, 0);

    GtkWidget *advanced = named(gtk_expander_new("Gelişmiş · playlist kuralları"), "source-advanced");
    GtkWidget *advanced_body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 10, "re-policy-panel");
    gtk_container_add(GTK_CONTAINER(advanced), advanced_body);
    gtk_box_pack_start(GTK_BOX(editor->source_properties), advanced, FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6); gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    editor->frequency = spin(editor, "source-frequency", 1, 100);
    editor->count = spin(editor, "source-count", 1, 100); editor->weight = spin(editor, "source-weight", 0, 100);
    editor->start = named(radios_ui_entry("00:00"), "source-start"); editor->end = named(radios_ui_entry("24:00"), "source-end");
    gtk_entry_set_width_chars(GTK_ENTRY(editor->start), 5); gtk_entry_set_width_chars(GTK_ENTRY(editor->end), 5);
    gtk_entry_set_max_length(GTK_ENTRY(editor->start), 5); gtk_entry_set_max_length(GTK_ENTRY(editor->end), 5);
    g_signal_connect(editor->start, "changed", G_CALLBACK(changed), editor); g_signal_connect(editor->end, "changed", G_CALLBACK(changed), editor);
    editor->interval = spin(editor, "source-interval", 0, 10080);
    grid_field(grid, "Her N turda", editor->frequency, 0, 0); grid_field(grid, "Parça adedi", editor->count, 1, 0);
    grid_field(grid, "Ağırlık (0 = kapalı)", editor->weight, 0, 1); grid_field(grid, "En az aralık (dk)", editor->interval, 1, 1);
    grid_field(grid, "Başlangıç", editor->start, 0, 2); grid_field(grid, "Bitiş", editor->end, 1, 2);
    editor->media_kind = named(gtk_combo_box_text_new(), "source-media-kind");
    const gchar *kinds[] = {"all", "music", "jingle", "advertisement", "sweeper"}, *titles[] = {"Tüm türler", "Müzik", "Jingle", "Reklam", "Sweeper"};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->media_kind), kinds[i], titles[i]);
    grid_field(grid, "Medya filtresi", editor->media_kind, 0, 3);
    g_signal_connect(editor->media_kind, "changed", G_CALLBACK(changed), editor);
    gtk_box_pack_start(GTK_BOX(advanced_body), grid, FALSE, FALSE, 0);
    GtkWidget *flags = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    editor->random = named(gtk_check_button_new_with_label("Playlistte rastgelelik"), "source-random");
    editor->advert_random = named(gtk_check_button_new_with_label("Reklam rastgeleliği"), "source-advert-random");
    editor->sweeper_random = named(gtk_check_button_new_with_label("Sweeper rastgeleliği"), "source-sweeper-random");
    gtk_widget_set_no_show_all(editor->random, TRUE); gtk_widget_hide(editor->random);
    GtkWidget *checks[] = {editor->random, editor->advert_random, editor->sweeper_random};
    for (guint i = 0; i < G_N_ELEMENTS(checks); ++i) { gtk_box_pack_start(GTK_BOX(flags), checks[i], FALSE, FALSE, 0); g_signal_connect(checks[i], "toggled", G_CALLBACK(changed), editor); }
    gtk_box_pack_start(GTK_BOX(advanced_body), flags, FALSE, FALSE, 0);
    GtkWidget *override = named(gtk_expander_new("Bu playlistin tekrar kuralları"), "source-settings");
    GtkWidget *override_box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, NULL);
    editor->override = named(gtk_check_button_new_with_label("Genel kuralları bu playlist için değiştir"), "source-override");
    gtk_box_pack_start(GTK_BOX(override_box), editor->override, FALSE, FALSE, 0);
    editor->override_body = settings_grid(editor, TRUE);
    gtk_box_pack_start(GTK_BOX(override_box), editor->override_body, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(override), override_box);
    g_signal_connect(editor->override, "toggled", G_CALLBACK(changed), editor);
    gtk_box_pack_start(GTK_BOX(advanced_body), override, FALSE, FALSE, 0);
    GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    GtkWidget *reset = action(editor, "Şarkı kurallarını sıfırla", "source-reset-song-rules", G_CALLBACK(reset_song_rules));
    gtk_widget_set_tooltip_text(reset, "Yalnız seçili playlistin özel şarkı kurallarını kaldırır.");
    gtk_box_pack_start(GTK_BOX(actions), reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(advanced_body), actions, FALSE, FALSE, 0);
    return page;
}
static void destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; RotationEditor *editor = data; editor->closed = TRUE;
    if (editor->routing_dialog) gtk_widget_destroy(editor->routing_dialog);
    if (editor->create_dialog) gtk_widget_destroy(editor->create_dialog);
    if (editor->step_dialog) gtk_widget_destroy(editor->step_dialog);
    if (editor->probability_dialog) gtk_widget_destroy(editor->probability_dialog);
    if (editor->source_menu) { gtk_menu_popdown(GTK_MENU(editor->source_menu)); gtk_widget_destroy(editor->source_menu); }
}
static void editor_free(gpointer data)
{
    RotationEditor *editor = data;
    if (editor->config) json_node_free(editor->config);
    g_hash_table_unref(editor->draft_fields);
    if (editor->playlists) json_array_unref(editor->playlists);
    if (editor->schemas) json_array_unref(editor->schemas);
    g_clear_pointer(&editor->channels, json_array_unref);
    if (editor->playlist_client) radios_data_client_unref(editor->playlist_client);
    g_clear_pointer(&editor->opened_client, radios_data_client_unref);
    g_clear_object(&editor->source_store); g_clear_object(&editor->song_store); g_clear_object(&editor->source_menu);
    if (editor->hooks.destroy) editor->hooks.destroy(editor->hooks.user);
    g_free(editor->selected); g_free(editor->selected_song); g_free(editor);
}

static void simple_workspace_style(void)
{
    static gboolean installed = FALSE; if (installed) return; installed = TRUE;
    GtkCssProvider *provider = gtk_css_provider_new();
    const gchar *css =
        "#rotation-editor { background: #142027; color: #e6f0f4; }"
        "#rotation-editor .re-heading { font-size: 16px; font-weight: bold; color: #edf7fa; }"
        "#rotation-editor .re-caption { color: #b0c2cc; }"
        "#rotation-editor entry, #rotation-editor combobox button { min-height: 32px; border-radius: 6px; background: #1e3039; color: #edf7fa; border-color: #3b535f; }"
        "#rotation-editor button { min-height: 30px; border-radius: 6px; background-image: none; background-color: #263e49; color: #e7f2f7; border-color: #45606d; }"
        "#rotation-editor button:hover { background-color: #34515e; }"
        "#rotation-editor button.suggested-action { background-color: #197e83; color: #ffffff; border-color: #32b5b2; }"
        "#rotation-editor treeview { background: #1b2a32; color: #e7f2f7; }"
        "#rotation-editor treeview:selected { background: #285763; color: #ffffff; }"
        "#rotation-editor treeview header button { background-color: #253c46; color: #b9d4df; }"
        "#rotation-editor expander { color: #b4cbd5; padding: 6px 0; }"
        "#rotation-editor notebook { background: #202529; color: #e6f0f4; }"
        "#rotation-editor notebook > stack, #rotation-editor notebook viewport { background-color: #202529; color: #e6f0f4; }"
        "#rotation-editor notebook label { color: #dbe8ee; }"
        "#rotation-editor notebook header { background: #25343d; }"
        "#rotation-editor notebook tab { min-height: 22px; padding: 7px 14px; color: #dbe8ee; }"
        "#rotation-editor notebook tab:checked { background-color: #314b57; }"
        "#rotation-editor notebook tab label { color: #e7f2f7; }"
        "#rotation-editor checkbutton { color: #e4f1f6; padding: 5px; }";
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

GtkWidget *radios_rotation_editor_new(RadioSViewContext *ctx, const RadioSRotationEditorHooks *hooks)
{
    g_return_val_if_fail(ctx != NULL, NULL);
    simple_workspace_style();
    RotationEditor *editor = g_new0(RotationEditor, 1); editor->ctx = ctx;
    editor->draft_fields = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    editor->opened_client = radios_data_client_acquire(ctx);
    if (hooks) editor->hooks = *hooks;
    editor->root = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, "rotation-editor"); gtk_widget_set_name(editor->root, "rotation-editor");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    named(scroll, "scroll");
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
    GtkWidget *body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 14, NULL); gtk_container_set_border_width(GTK_CONTAINER(body), 16);
    gtk_container_add(GTK_CONTAINER(scroll), body); gtk_box_pack_start(GTK_BOX(editor->root), scroll, TRUE, TRUE, 0);
    GtkWidget *name = editor->name_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    editor->name = named(radios_ui_entry("Rotasyon adı"), "name"); gtk_entry_set_max_length(GTK_ENTRY(editor->name), 160);
    editor->name_caption = label("Rotasyon", "re-caption"); gtk_box_pack_start(GTK_BOX(name), editor->name_caption, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(name), editor->name, TRUE, TRUE, 0); gtk_box_pack_start(GTK_BOX(body), name, FALSE, FALSE, 0);
    g_signal_connect(editor->name, "changed", G_CALLBACK(changed), editor);
    editor->total = named(label("0 playlist · 0 kaynak şarkı", "re-caption"), "total"); gtk_box_pack_start(GTK_BOX(body), editor->total, FALSE, FALSE, 0);
    GtkWidget *settings = named(gtk_expander_new("Gelişmiş · genel kurallar"), "settings");
    editor->global_settings = settings;
    GtkWidget *settings_body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, NULL);
    const gchar *selections[] = {"Doğrusal sıra", "Olasılıklı sıra", NULL};
    editor->selection_mode = named(radios_ui_combo(selections, 0), "selection-mode");
    /* Keep the legacy persisted selection value without exposing the removed
     * playlist-between-playlists selector in the PDF workspace. */
    gtk_widget_set_no_show_all(editor->selection_mode, TRUE);
    gtk_widget_hide(editor->selection_mode);
    gtk_box_pack_start(GTK_BOX(settings_body), editor->selection_mode, FALSE, FALSE, 0);
    g_signal_connect(editor->selection_mode, "changed", G_CALLBACK(changed), editor);
    gtk_box_pack_start(GTK_BOX(settings_body), settings_grid(editor, FALSE), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(settings), settings_body);
    GtkWidget *toolbar = editor->library_toolbar = named(radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL), "library-toolbar");
    gtk_box_pack_start(GTK_BOX(toolbar), label("1. Playlist ve live event sırası", "re-heading"), TRUE, TRUE, 0);
    GtkWidget *create = action(editor, "Yeni playlist…", "create-playlist", G_CALLBACK(create_playlist));
    GtkWidget *choose = action(editor, "Playlist ya da live event ekle…", "add-choose", G_CALLBACK(add_dialog));
    gtk_style_context_add_class(gtk_widget_get_style_context(choose), "suggested-action");
    gtk_box_pack_start(GTK_BOX(toolbar), create, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), choose, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), action(editor, "Live event ekle…", "add-live", G_CALLBACK(step_add)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), toolbar, FALSE, FALSE, 0);
    /* Keep existing quick-add integrations; the searchable chooser is the
     * only add flow shown in the simplified workspace. */
    GtkWidget *legacy_add = named(radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL), "quick-add");
    gtk_widget_set_no_show_all(legacy_add, TRUE);
    editor->playlists_combo = named(gtk_combo_box_text_new(), "playlist-combo");
    editor->add = action(editor, "Ekle", "add", G_CALLBACK(add_source));
    gtk_box_pack_start(GTK_BOX(legacy_add), editor->playlists_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(legacy_add), editor->add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), legacy_add, FALSE, FALSE, 0);
    g_signal_connect(editor->playlists_combo, "changed", G_CALLBACK(combo_changed), editor);
    editor->pages = named(gtk_stack_new(), "pages"); gtk_stack_set_homogeneous(GTK_STACK(editor->pages), FALSE);
    editor->source_store = gtk_list_store_new(SOURCE_COLUMNS,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_INT,G_TYPE_INT,G_TYPE_STRING,G_TYPE_INT,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_INT,G_TYPE_STRING,G_TYPE_INT);
    editor->sources = named(gtk_tree_view_new_with_model(GTK_TREE_MODEL(editor->source_store)), "sources");
    table_column(editor->sources, "Playlist", SOURCE_NAME, TRUE);
    table_column(editor->sources, "Tür", SOURCE_KIND, FALSE);
    table_column(editor->sources, "Parça", SOURCE_TRACKS, FALSE);
    GtkWidget *footer = editor->library_footer = named(radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL), "library-footer");
    GtkWidget *probability = action(editor, "Olasılık ve geçişler…", "probability", G_CALLBACK(open_probability));
    gtk_box_pack_start(GTK_BOX(footer), action(editor, "Şema Ayarları…", "schema-settings-open", G_CALLBACK(open_schema_settings)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer), probability, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer), action(editor, "Adım ayarları…", "step-edit", G_CALLBACK(step_edit)), FALSE, FALSE, 0);
    GtkWidget *autodj_policy = action(editor, "AutoDJ: geçiş ve kanallar…", "autodj-policy-open", G_CALLBACK(open_autodj_policy));
    gtk_box_pack_start(GTK_BOX(footer), autodj_policy, FALSE, FALSE, 0);
    GtkWidget *automation = action(editor, "AutoDJ otomasyonu…", "autodj-automation-open", G_CALLBACK(open_automation));
    gtk_widget_set_tooltip_text(automation, "Geçiş ön ayarlarını tasarla, türlere ata, rotasyonla ve gün/saat dilimlerine programla.");
    gtk_box_pack_start(GTK_BOX(footer), automation, FALSE, FALSE, 0);
    GtkWidget *hint = named(label("", "re-caption"), "share-legend");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_box_pack_start(GTK_BOX(footer), hint, TRUE, TRUE, 0);
    editor->up = action(editor, "↑", "up", G_CALLBACK(source_action));
    editor->down = action(editor, "↓", "down", G_CALLBACK(source_action));
    editor->remove = action(editor, "Kaldır", "remove", G_CALLBACK(source_action));
    gtk_widget_set_tooltip_text(editor->up, "Seçili playlisti yukarı taşı");
    gtk_widget_set_tooltip_text(editor->down, "Seçili playlisti aşağı taşı");
    GtkWidget *source_actions[] = {editor->up, editor->down, editor->remove};
    for (guint i = 0; i < G_N_ELEMENTS(source_actions); ++i)
        gtk_box_pack_start(GTK_BOX(footer), source_actions[i], FALSE, FALSE, 0);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(editor->sources)), "changed", G_CALLBACK(source_selected), editor);
    editor->source_menu = named(gtk_menu_new(), "source-menu"); g_object_ref_sink(editor->source_menu);
    gtk_menu_attach_to_widget(GTK_MENU(editor->source_menu), editor->sources, NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(editor->source_menu), "radios");
    GtkWidget *autodj = named(gtk_menu_item_new_with_label("AutoDJ"), "autodj");
    gtk_menu_shell_append(GTK_MENU_SHELL(editor->source_menu), autodj);
    g_signal_connect(autodj, "activate", G_CALLBACK(source_autodj), editor);
    g_signal_connect(editor->sources, "button-press-event", G_CALLBACK(source_pressed), editor);
    g_signal_connect(editor->sources, "key-press-event", G_CALLBACK(source_menu_key), editor);
    g_signal_connect(editor->sources, "button-release-event", G_CALLBACK(source_released), editor);
    g_signal_connect(editor->sources, "row-activated", G_CALLBACK(source_activated), editor);
    gtk_stack_add_named(GTK_STACK(editor->pages), table_scrolled(editor->sources, 200), "library");
    gtk_stack_add_named(GTK_STACK(editor->pages), source_page(editor), "songs");
    gtk_stack_add_named(GTK_STACK(editor->pages), automation_page_new(editor), "autodj-automation");
    gtk_box_pack_start(GTK_BOX(body), editor->pages, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(body), footer, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), editor->source_properties, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), settings, FALSE, FALSE, 0);
    g_signal_connect(editor->pages, "notify::visible-child-name", G_CALLBACK(page_changed), editor);
    editor->content_warning = named(label("Yayın adımı yok", "error"), "content-warning");
    gtk_widget_set_tooltip_text(editor->content_warning, "Playlist ya da live event ekleyin.");
    gtk_label_set_line_wrap(GTK_LABEL(editor->content_warning), TRUE); gtk_label_set_xalign(GTK_LABEL(editor->content_warning), 0);
    gtk_widget_set_no_show_all(editor->content_warning, TRUE);
    gtk_box_pack_start(GTK_BOX(body), editor->content_warning, FALSE, FALSE, 0);
    editor->status = named(label("", "application-status"), "status"); gtk_label_set_line_wrap(GTK_LABEL(editor->status), TRUE);
    gtk_box_pack_start(GTK_BOX(body), editor->status, FALSE, FALSE, 0);
    g_object_set_data_full(G_OBJECT(editor->root), "radios-rotation-editor", editor, editor_free);
    g_signal_connect(editor->root, "destroy", G_CALLBACK(destroyed), editor);
    editor->config = default_config(); render(editor, FALSE); gtk_widget_show_all(editor->root);
    gtk_widget_set_no_show_all(editor->global_settings, TRUE);
    gtk_widget_set_no_show_all(editor->source_properties, TRUE);
    gtk_widget_set_no_show_all(editor->library_toolbar, TRUE);
    gtk_widget_set_no_show_all(editor->library_footer, TRUE);
    gtk_widget_set_no_show_all(editor->name_row, TRUE);
    gtk_widget_set_no_show_all(editor->total, TRUE);
    page_changed(G_OBJECT(editor->pages), NULL, editor); return editor->root;
}
void radios_rotation_editor_set_config(GtkWidget *widget, JsonObject *config)
{
    RotationEditor *editor = editor_get(widget); if (!editor || editor->closed) return;
    /* A clean document ACK reapplies this same UUID. Keep its active
     * independent editor open; only a real record switch or explicit reload
     * retires captured asynchronous/dialog intents. */
    RadioSDataClient *current = editor->ctx->data ? radios_data_client_acquire(editor->ctx) : NULL;
    gboolean context_changed = editor->ctx->data && !radios_data_client_same_identity(current, editor->opened_client);
    g_clear_pointer(&current, radios_data_client_unref);
    gboolean switched = context_changed || !config || g_strcmp0(radios_json_string(config, "id", ""),
        radios_json_string(config_object(editor), "id", "")) != 0 ||
        g_object_get_data(G_OBJECT(widget), "radios-data-explicit-reload") != NULL;
    if (switched && editor->probability_dialog) gtk_widget_destroy(editor->probability_dialog);
    if (switched && editor->routing_dialog) gtk_widget_destroy(editor->routing_dialog);
    if (switched && editor->create_dialog) gtk_widget_destroy(editor->create_dialog);
    if (switched && editor->step_dialog) gtk_widget_destroy(editor->step_dialog);
    if (switched) g_hash_table_remove_all(editor->draft_fields);
    else draft_capture(editor);
    JsonNode *replacement = NULL;
    if (config) {
        JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, config); replacement = copy_node(node); json_node_free(node);
    }
    if (!replacement) replacement = default_config();
    if (editor->config) json_node_free(editor->config);
    editor->config = replacement;
    if (switched) {
        g_clear_pointer(&editor->selected, g_free); g_clear_pointer(&editor->selected_song, g_free);
    }
    JsonObject *object = config_object(editor);
    if (!object_member(object, "settings")) json_object_set_object_member(object, "settings", default_settings());
    if (!array_member(object, "sources")) json_object_set_array_member(object, "sources", json_array_new());
    if (editor->selected && !selected_source(editor, NULL)) {
        g_clear_pointer(&editor->selected, g_free); g_clear_pointer(&editor->selected_song, g_free);
        if (editor->routing_dialog) gtk_widget_destroy(editor->routing_dialog);
    }
    render(editor, !switched); schema_refresh(editor);
}
void radios_rotation_editor_set_playlists(GtkWidget *widget, JsonArray *playlists)
{
    RotationEditor *editor = editor_get(widget); if (!editor || editor->closed) return;
    draft_capture(editor);
    gchar *page = g_strdup(gtk_stack_get_visible_child_name(GTK_STACK(editor->pages)));
    JsonArray *replacement = NULL;
    if (playlists) {
        JsonNode *node = json_node_new(JSON_NODE_ARRAY); json_node_set_array(node, playlists); JsonNode *copy = copy_node(node); json_node_free(node);
        if (copy) { replacement = json_array_ref(json_node_get_array(copy)); json_node_free(copy); }
    }
    if (editor->playlists) json_array_unref(editor->playlists);
    editor->playlists = replacement;
    if (editor->playlist_client) radios_data_client_unref(editor->playlist_client);
    editor->playlist_client = replacement ? radios_data_client_acquire(editor->ctx) : NULL;
    const gchar *active = gtk_combo_box_get_active_id(GTK_COMBO_BOX(editor->playlists_combo)); gchar *remember = g_strdup(active);
    editor->updating = TRUE; gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(editor->playlists_combo));
    if (catalog_current(editor)) for (guint i = 0; replacement && i < json_array_get_length(replacement); ++i) {
        JsonNode *node = json_array_get_element(replacement, i); if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
        JsonObject *playlist = json_node_get_object(node); const gchar *id = radios_json_string(playlist, "_id", "");
        if (g_uuid_string_is_valid(id)) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(editor->playlists_combo), id, radios_json_string(playlist, "name", "Playlist"));
    }
    if (!remember || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(editor->playlists_combo), remember)) gtk_combo_box_set_active(GTK_COMBO_BOX(editor->playlists_combo), 0);
    g_free(remember); sources_render(editor); songs_render(editor); source_summary_render(editor); totals_render(editor);
    draft_restore(editor); restore_page(editor, page); g_free(page);
    editor->updating = FALSE; editor->valid = validate_visible(editor);
    if (editor->schema_body) radios_schema_settings_set_playlists(editor->schema_body, editor->playlists);
}
JsonNode *radios_rotation_editor_snapshot(GtkWidget *widget)
{
    RotationEditor *editor = editor_get(widget);
    if (!editor || editor->closed || !validate_visible(editor)) return NULL;
    return copy_node(editor->config);
}
gboolean radios_rotation_editor_ready(GtkWidget *widget)
{
    RotationEditor *editor = editor_get(widget);
    return editor && !editor->closed && validate_visible(editor);
}
