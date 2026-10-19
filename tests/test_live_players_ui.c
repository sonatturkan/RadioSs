#include "radios/views.h"
#include "radios/audio_ui.h"
#include "radios/media_ui.h"
#include "radios/player_loader.h"
#include "radios/player_transport.h"
#include "radios/playout_ui.h"
#include <string.h>

/* Exercise the actual broadcast cards and their GTK callbacks. Link wrappers
 * replace only the asynchronous library lookup, playlist picker, and deck
 * transport boundary; PCM and database contracts retain their existing native
 * acceptance suites. Delayed replies are delivered in adversarial order. */

typedef struct {
    RadioSViewContext *ctx;
    GWeakRef owner;
    gchar *media_id;
    RadioSDataCallback callback;
    gpointer user;
    GDestroyNotify destroy;
} Lookup;
typedef struct { RadioSViewContext ctx; GtkWidget *window, *page; } Harness;

static RadioSAudioUiSnapshot snapshots[32];
static GPtrArray *lookups;
static JsonArray *picker_items;
static gboolean picker_append;
static gboolean managed_takeover;
static guint loads, plays, stops, notices, library_opens;
static gchar last_loaded[37];
static const gchar *ids[] = {
    "9c420ab3-2c37-4f9b-b89d-004d949b0001",
    "9c420ab3-2c37-4f9b-b89d-004d949b0002",
    "9c420ab3-2c37-4f9b-b89d-004d949b0003"
};
static const gchar *version_ids[] = {
    "983926c8-eb5f-4880-a98d-458450ea0001",
    "983926c8-eb5f-4880-a98d-458450ea0002",
    "983926c8-eb5f-4880-a98d-458450ea0003"
};
static const gchar *file_ids[] = {
    "4b32d114-e3b7-4777-819b-df29cc690001",
    "4b32d114-e3b7-4777-819b-df29cc690002",
    "4b32d114-e3b7-4777-819b-df29cc690003"
};
static const gchar *digest = "1111111111111111111111111111111111111111111111111111111111111111";

static guint index_for(const gchar *id)
{
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) if (g_str_equal(id, ids[i])) return i;
    g_error("Unexpected test media identity: %s", id); return 0;
}
static JsonObject *input_new(guint index)
{
    JsonObject *input = json_object_new();
    json_object_set_string_member(input, "media_id", ids[index]);
    json_object_set_string_member(input, "version_id", version_ids[index]);
    json_object_set_string_member(input, "media_file_id", file_ids[index]);
    json_object_set_string_member(input, "media_sha256", digest);
    gchar *title = g_strdup_printf("Queued media %u", index + 1);
    json_object_set_string_member(input, "title", title); g_free(title);
    json_object_set_string_member(input, "artist", "Test artist");
    json_object_set_string_member(input, "album", "Test album");
    json_object_set_int_member(input, "duration_ms", 10000);
    return input;
}
static JsonNode *detail_new(guint index)
{
    JsonObject *detail = json_object_new(), *media = input_new(index), *version = json_object_new();
    json_object_set_string_member(media, "_id", ids[index]);
    json_object_set_string_member(media, "station_id", "06f63525-d61f-4078-928f-82bd4482d687");
    json_object_set_string_member(version, "_id", version_ids[index]);
    json_object_set_string_member(version, "media_file_id", file_ids[index]);
    json_object_set_string_member(version, "sha256", digest);
    json_object_set_int_member(version, "duration_ms", 10000);
    json_object_set_object_member(detail, "media", media);
    json_object_set_object_member(detail, "version", version);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, detail); return node;
}
static void lookup_free(gpointer value)
{
    Lookup *lookup = value; if (!lookup) return;
    if (lookup->destroy) lookup->destroy(lookup->user);
    g_weak_ref_clear(&lookup->owner); g_free(lookup->media_id); g_free(lookup);
}
gboolean __wrap_radios_playout_ui_owns_deck(RadioSViewContext *ctx, guint deck)
{ (void)ctx; (void)deck; return managed_takeover; }
gboolean __wrap_radios_playout_ui_deck_metadata(RadioSViewContext *ctx, guint deck, RadioSPlayoutDeckMetadata *metadata)
{
    (void)ctx; (void)deck; memset(metadata, 0, sizeof *metadata);
    if (!managed_takeover) return FALSE;
    metadata->duration_ms = 10000; g_strlcpy(metadata->title, "Managed programme PCM", sizeof metadata->title); return TRUE;
}
gboolean __wrap_radios_media_library_get(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *media_id, RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{
    Lookup *lookup = g_new0(Lookup, 1); lookup->ctx = ctx; lookup->media_id = g_strdup(media_id);
    lookup->callback = callback; lookup->user = user; lookup->destroy = destroy;
    g_weak_ref_init(&lookup->owner, owner); g_ptr_array_add(lookups, lookup); return TRUE;
}
void __wrap_radios_player_loader_open(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *title, RadioSPlayerLoaderDone done, gpointer data)
{
    (void)ctx; (void)owner; (void)title;
    g_assert_nonnull(picker_items); done(picker_items, "Regression queue", picker_append, data);
}
void __wrap_radios_media_library_open_for_deck(RadioSViewContext *ctx, guint deck)
{ (void)ctx; g_assert_cmpuint(deck, ==, 0); ++library_opens; }
gboolean __wrap_radios_audio_ui_get_snapshot(RadioSViewContext *ctx, guint deck, RadioSAudioUiSnapshot *snapshot)
{ (void)ctx; g_assert_cmpuint(deck, <, G_N_ELEMENTS(snapshots)); *snapshot = snapshots[deck]; return TRUE; }
gboolean __wrap_radios_audio_ui_load_media(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *detail, guint deck, gboolean autoplay, GError **error)
{
    (void)ctx; (void)error; ++loads;
    JsonObject *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version");
    RadioSAudioUiSnapshot *snapshot = &snapshots[deck]; ++snapshot->generation;
    snapshot->state = autoplay ? RADIOS_AUDIO_UI_PLAYING : RADIOS_AUDIO_UI_READY;
    snapshot->position_ms = 0; snapshot->duration_ms = 10000;
    g_strlcpy(snapshot->media_id, radios_json_string(media, "_id", ""), sizeof snapshot->media_id);
    g_strlcpy(snapshot->version_id, radios_json_string(version, "_id", ""), sizeof snapshot->version_id);
    g_strlcpy(snapshot->title, radios_json_string(media, "title", ""), sizeof snapshot->title);
    JsonNode *cache = json_node_new(JSON_NODE_OBJECT); json_node_set_object(cache, detail);
    g_object_set_data_full(G_OBJECT(owner), "radios-deck-media-detail", cache, (GDestroyNotify)json_node_free);
    g_strlcpy(last_loaded, snapshot->media_id, sizeof last_loaded); return TRUE;
}
gboolean __wrap_radios_audio_ui_play(RadioSViewContext *ctx, guint deck, GError **error)
{ (void)ctx; (void)error; ++plays; snapshots[deck].state = RADIOS_AUDIO_UI_PLAYING; return TRUE; }
gboolean __wrap_radios_audio_ui_pause(RadioSViewContext *ctx, guint deck, GError **error)
{ (void)ctx; (void)error; snapshots[deck].state = RADIOS_AUDIO_UI_PAUSED; return TRUE; }
gboolean __wrap_radios_audio_ui_stop(RadioSViewContext *ctx, guint deck, GError **error)
{
    (void)ctx; (void)error; ++stops; ++snapshots[deck].generation;
    snapshots[deck].state = snapshots[deck].media_id[0] ? RADIOS_AUDIO_UI_READY : RADIOS_AUDIO_UI_EMPTY;
    snapshots[deck].position_ms = 0; return TRUE;
}
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static void run_for(guint milliseconds)
{
    gint64 end = g_get_monotonic_time() + (gint64)milliseconds * 1000;
    do { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); } while (g_get_monotonic_time() < end);
}
static void notify(gpointer data, const gchar *message)
{ (void)data; g_assert_nonnull(message); ++notices; }
static Harness *harness_new(void)
{
    memset(snapshots, 0, sizeof snapshots);
    loads = plays = stops = notices = library_opens = 0; last_loaded[0] = '\0';
    managed_takeover = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(snapshots); ++i) {
        snapshots[i].output_ready = TRUE; snapshots[i].sync_master = -1; snapshots[i].tempo_ratio = 1;
    }
    lookups = g_ptr_array_new_with_free_func(lookup_free);
    Harness *h = g_new0(Harness, 1); h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(h->window);
    h->ctx.parent = GTK_WINDOW(h->window); h->ctx.runtime = radios_runtime_new(8, 32); h->ctx.notify = notify;
    h->ctx.audio = radios_audio_ui_new(&h->ctx); g_assert_nonnull(h->ctx.audio);
    h->page = radios_broadcast_new(&h->ctx); gtk_container_add(GTK_CONTAINER(h->window), h->page);
    gtk_widget_show_all(h->window); run_for(110); return h;
}
static void harness_free(Harness *h)
{
    gtk_widget_destroy(h->window); g_ptr_array_unref(lookups); lookups = NULL;
    radios_audio_ui_close(h->ctx.audio); radios_audio_ui_unref(h->ctx.audio);
    radios_runtime_free(h->ctx.runtime); g_object_unref(h->window); g_free(h);
    while (g_main_context_iteration(NULL, FALSE)) {}
}
static void click(Harness *h, const gchar *name)
{
    GtkWidget *button = find(h->page, name); g_assert_true(GTK_IS_BUTTON(button));
    g_assert_true(gtk_widget_is_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button));
}
static void queue_set(Harness *h, const guint *indexes, guint count, gboolean append)
{
    picker_items = json_array_new(); picker_append = append;
    for (guint i = 0; i < count; ++i) json_array_add_object_element(picker_items, input_new(indexes[i]));
    GtkWidget *button = find(h->page, "deck-0-playlist"); g_assert_true(GTK_IS_BUTTON(button));
    /* The wrapper supplies the picker result; no data service is needed. */
    gtk_button_clicked(GTK_BUTTON(button)); json_array_unref(picker_items); picker_items = NULL; run_for(110);
}
static void reply_with_identity(guint index, gboolean success, const gchar *media_override, const gchar *version_override)
{
    g_assert_cmpuint(index, <, lookups->len); Lookup *lookup = g_ptr_array_index(lookups, index); g_assert_nonnull(lookup);
    GtkWidget *owner = g_weak_ref_get(&lookup->owner); g_assert_nonnull(owner);
    JsonNode *detail = success ? detail_new(index_for(lookup->media_id)) : NULL;
    if (detail && media_override) json_object_set_string_member(
        json_object_get_object_member(json_node_get_object(detail), "media"), "_id", media_override);
    if (detail && version_override) json_object_set_string_member(
        json_object_get_object_member(json_node_get_object(detail), "version"), "_id", version_override);
    GError *error = success ? NULL : g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Controlled lookup failure");
    lookup->callback(lookup->ctx, owner, detail, error, lookup->user);
    g_clear_pointer(&detail, json_node_free); g_clear_error(&error); g_object_unref(owner);
    g_ptr_array_index(lookups, index) = NULL; lookup_free(lookup);
}
static void reply(guint index, gboolean success)
{ reply_with_identity(index, success, NULL, NULL); }
static void test_replacement_late_reply(void)
{
    Harness *h = harness_new(); const guint first[] = {0}, second[] = {1};
    queue_set(h, first, 1, FALSE); click(h, "deck-0-play"); g_assert_cmpuint(lookups->len, ==, 1);
    queue_set(h, second, 1, FALSE); click(h, "deck-0-play"); g_assert_cmpuint(lookups->len, ==, 2);
    reply(1, TRUE); g_assert_cmpuint(loads, ==, 1); g_assert_cmpstr(last_loaded, ==, ids[1]);
    reply(0, TRUE); g_assert_cmpuint(loads, ==, 1); g_assert_cmpstr(last_loaded, ==, ids[1]); harness_free(h);
}
static void test_stop_invalidates_lookup(void)
{
    Harness *h = harness_new(); const guint items[] = {0}; queue_set(h, items, 1, FALSE);
    click(h, "deck-0-play"); run_for(110); click(h, "deck-0-stop"); reply(0, TRUE);
    g_assert_cmpuint(loads, ==, 0); g_assert_cmpuint(stops, >, 0); harness_free(h);
}
static void test_stopped_next_loads_current(void)
{
    Harness *h = harness_new(); const guint items[] = {0, 1}; queue_set(h, items, 2, FALSE);
    click(h, "deck-0-play"); reply(0, TRUE); run_for(110); click(h, "deck-0-stop");
    run_for(110); click(h, "deck-0-next"); run_for(110); click(h, "deck-0-play");
    g_assert_cmpuint(lookups->len, ==, 2); g_assert_cmpuint(plays, ==, 0);
    reply(1, TRUE); g_assert_cmpstr(last_loaded, ==, ids[1]); harness_free(h);
}
static void test_stopped_next_invalidates_old_lookup(void)
{
    Harness *h = harness_new(); const guint items[] = {0, 1}; queue_set(h, items, 2, FALSE);
    click(h, "deck-0-play"); run_for(110); click(h, "deck-0-stop"); run_for(110);
    click(h, "deck-0-next"); run_for(110); click(h, "deck-0-play");
    g_assert_cmpuint(lookups->len, ==, 2);
    reply(1, TRUE); g_assert_cmpstr(last_loaded, ==, ids[1]);
    reply(0, TRUE); g_assert_cmpuint(loads, ==, 1); g_assert_cmpstr(last_loaded, ==, ids[1]); harness_free(h);
}
static GtkWidget *current_remove(GtkWidget *root)
{
    GtkStyleContext *style = gtk_widget_get_style_context(root);
    if (GTK_IS_CONTAINER(root) && gtk_style_context_has_class(style, "is-current")) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *button = NULL;
        for (GList *item = children; item; item = item->next) if (GTK_IS_BUTTON(item->data)) button = item->data;
        g_list_free(children); if (button) return button;
    }
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = current_remove(item->data);
    g_list_free(children); return result;
}
static void test_removed_current_does_not_resume_old_media(void)
{
    Harness *h = harness_new(); const guint items[] = {0, 1}; queue_set(h, items, 2, FALSE);
    click(h, "deck-0-play"); reply(0, TRUE); run_for(110); click(h, "deck-0-stop"); run_for(110);
    GtkWidget *card = g_object_get_data(G_OBJECT(h->window), "radios-deck-owner-0");
    GtkWidget *remove = current_remove(card); g_assert_true(GTK_IS_BUTTON(remove)); g_assert_true(gtk_widget_is_sensitive(remove));
    gtk_button_clicked(GTK_BUTTON(remove)); run_for(110); click(h, "deck-0-play");
    g_assert_cmpuint(lookups->len, ==, 2); g_assert_cmpuint(plays, ==, 0);
    reply(1, TRUE); g_assert_cmpstr(last_loaded, ==, ids[1]); harness_free(h);
}
static void test_lookup_pending_does_not_skip_on_old_eof(void)
{
    Harness *h = harness_new(); const guint items[] = {0, 1, 2}; queue_set(h, items, 3, FALSE);
    click(h, "deck-0-play");
    /* An old deck's terminal snapshot can arrive after Stop was published. */
    snapshots[0].state = RADIOS_AUDIO_UI_EOF; run_for(250);
    g_assert_cmpuint(lookups->len, ==, 1);
    Lookup *lookup = g_ptr_array_index(lookups, 0); g_assert_cmpstr(lookup->media_id, ==, ids[0]);
    reply(0, TRUE); g_assert_cmpstr(last_loaded, ==, ids[0]); harness_free(h);
}
static void test_failed_lookup_can_retry_same_current(void)
{
    Harness *h = harness_new(); const guint items[] = {0, 1}; queue_set(h, items, 2, FALSE);
    click(h, "deck-0-play"); reply(0, FALSE); run_for(110); g_assert_cmpuint(notices, >, 0);
    click(h, "deck-0-play"); g_assert_cmpuint(lookups->len, ==, 2);
    Lookup *lookup = g_ptr_array_index(lookups, 1); g_assert_cmpstr(lookup->media_id, ==, ids[0]);
    reply(1, TRUE); g_assert_cmpstr(last_loaded, ==, ids[0]); harness_free(h);
}
static void test_lookup_version_mismatch_rejected(void)
{
    Harness *h = harness_new(); const guint items[] = {0}; queue_set(h, items, 1, FALSE);
    click(h, "deck-0-play"); reply_with_identity(0, TRUE, NULL, version_ids[1]); run_for(110);
    g_assert_cmpuint(loads, ==, 0); g_assert_cmpuint(notices, >, 0);
    click(h, "deck-0-play"); reply(1, TRUE); g_assert_cmpstr(last_loaded, ==, ids[0]); harness_free(h);
}
static void test_lookup_media_mismatch_rejected(void)
{
    Harness *h = harness_new(); const guint items[] = {0}; queue_set(h, items, 1, FALSE);
    click(h, "deck-0-play"); reply_with_identity(0, TRUE, ids[1], NULL); run_for(110);
    g_assert_cmpuint(loads, ==, 0); g_assert_cmpuint(notices, >, 0);
    click(h, "deck-0-play"); reply(1, TRUE); g_assert_cmpstr(last_loaded, ==, ids[0]); harness_free(h);
}
static void test_clear_after_stop_rejects_late_lookup(void)
{
    Harness *h = harness_new(); const guint items[] = {0, 1}; queue_set(h, items, 2, FALSE);
    click(h, "deck-0-play"); run_for(110);
    g_assert_false(gtk_widget_is_sensitive(find(h->page, "deck-0-clear")));
    click(h, "deck-0-stop"); run_for(110); click(h, "deck-0-clear"); reply(0, TRUE); run_for(110);
    g_assert_cmpuint(loads, ==, 0);
    g_assert_cmpstr(gtk_expander_get_label(GTK_EXPANDER(find(h->page, "deck-0-inputs"))), ==, "Girdiler (0)");
    g_assert_false(gtk_widget_is_sensitive(find(h->page, "deck-0-play")));
    g_assert_false(gtk_widget_is_sensitive(find(h->page, "deck-0-next"))); harness_free(h);
}
static void test_direct_library_selection_retires_queue_lookup(void)
{
    Harness *h = harness_new(); const guint items[] = {0}; queue_set(h, items, 1, FALSE);
    click(h, "deck-0-play"); click(h, "deck-0-load"); g_assert_cmpuint(library_opens, ==, 1);
    reply(0, TRUE); g_assert_cmpuint(loads, ==, 0);
    /* The independent library picker has now loaded another verified source.
     * The retained playlist must not hijack Play for that direct selection. */
    snapshots[0].state = RADIOS_AUDIO_UI_READY; snapshots[0].duration_ms = 10000;
    g_strlcpy(snapshots[0].media_id, ids[1], sizeof snapshots[0].media_id);
    g_strlcpy(snapshots[0].version_id, version_ids[1], sizeof snapshots[0].version_id);
    run_for(110); click(h, "deck-0-play");
    g_assert_cmpuint(lookups->len, ==, 1); g_assert_cmpuint(plays, ==, 1); harness_free(h);
}
static void test_external_library_load_preserves_direct_selection(void)
{
    Harness *h = harness_new(); const guint items[] = {0}; queue_set(h, items, 1, FALSE);
    click(h, "deck-0-play"); reply(0, TRUE); run_for(110);
    GtkWidget *card = g_object_get_data(G_OBJECT(h->window), "radios-deck-owner-0");
    JsonNode *direct = detail_new(1); GError *error = NULL;
    /* media.c's general library loads a chosen deck directly; it does not
     * invoke the live card's Kütüphaneden yükle button first. */
    g_assert_true(radios_audio_ui_load_media(&h->ctx, card, json_node_get_object(direct), 0, FALSE, &error));
    g_assert_no_error(error); radios_broadcast_player_library_loaded(&h->ctx, 0);
    json_node_free(direct); run_for(110);
    click(h, "deck-0-play");
    g_assert_cmpuint(lookups->len, ==, 1); g_assert_cmpuint(plays, ==, 1);
    g_assert_cmpstr(snapshots[0].media_id, ==, ids[1]); harness_free(h);
}
static void test_external_library_load_retires_pending_lookup(void)
{
    Harness *h = harness_new(); const guint items[] = {0}; queue_set(h, items, 1, FALSE);
    click(h, "deck-0-play"); g_assert_cmpuint(lookups->len, ==, 1);
    GtkWidget *card = g_object_get_data(G_OBJECT(h->window), "radios-deck-owner-0");
    JsonNode *direct = detail_new(1); GError *error = NULL;
    g_assert_true(radios_audio_ui_load_media(&h->ctx, card, json_node_get_object(direct), 0, FALSE, &error));
    g_assert_no_error(error); radios_broadcast_player_library_loaded(&h->ctx, 0); json_node_free(direct);
    reply(0, TRUE); run_for(110);
    g_assert_cmpuint(loads, ==, 1); g_assert_cmpstr(snapshots[0].media_id, ==, ids[1]);
    click(h, "deck-0-play"); g_assert_cmpuint(plays, ==, 1); g_assert_cmpuint(lookups->len, ==, 1);
    g_assert_cmpstr(snapshots[0].media_id, ==, ids[1]); harness_free(h);
}
static void test_repeated_media_next_requires_fresh_lookup(void)
{
    Harness *h = harness_new(); const guint items[] = {0, 0}; queue_set(h, items, 2, FALSE);
    click(h, "deck-0-play"); reply(0, TRUE); run_for(110); click(h, "deck-0-stop"); run_for(110);
    click(h, "deck-0-next"); run_for(110); click(h, "deck-0-play");
    g_assert_cmpuint(lookups->len, ==, 2); g_assert_cmpuint(plays, ==, 0);
    reply(1, TRUE); g_assert_cmpstr(last_loaded, ==, ids[0]); harness_free(h);
}
static void test_managed_takeover_retires_local_lookup(void)
{
    Harness *h = harness_new(); const guint items[] = {0}; queue_set(h, items, 1, FALSE); click(h, "deck-0-play");
    /* Deliver before the next 100 ms UI tick: callback ownership must suffice. */
    managed_takeover = TRUE; reply(0, TRUE); g_assert_cmpuint(loads, ==, 0);
    managed_takeover = FALSE; harness_free(h);
}
typedef struct { guint label, sensitive, inserted, deleted; } IdleControlChanges;
static void idle_property_changed(GObject *object, GParamSpec *property, gpointer data)
{ (void)object; (void)property; ++*(guint *)data; }
static void idle_row_inserted(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{ (void)model; (void)path; (void)iter; ++((IdleControlChanges *)data)->inserted; }
static void idle_row_deleted(GtkTreeModel *model, GtkTreePath *path, gpointer data)
{ (void)model; (void)path; ++((IdleControlChanges *)data)->deleted; }
static void test_idle_start_and_targets_keep_widgets(void)
{
    Harness *h = harness_new(); GtkWidget *start = find(h->page, "deck-0-autodj-start");
    GtkWidget *targets = find(h->page, "deck-0-autodj-target");
    g_assert_true(GTK_IS_BUTTON(start)); g_assert_true(GTK_IS_COMBO_BOX(targets));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(start)), ==, "Başlat"); g_assert_false(gtk_widget_is_sensitive(start));
    GtkWidget *label = g_object_ref(gtk_bin_get_child(GTK_BIN(start)));
    GtkTreeModel *model = g_object_ref(gtk_combo_box_get_model(GTK_COMBO_BOX(targets))); IdleControlChanges changes = {0};
    gulong label_changed = g_signal_connect(start, "notify::label", G_CALLBACK(idle_property_changed), &changes.label);
    gulong sensitive_changed = g_signal_connect(start, "notify::sensitive", G_CALLBACK(idle_property_changed), &changes.sensitive);
    gulong inserted = g_signal_connect(model, "row-inserted", G_CALLBACK(idle_row_inserted), &changes);
    gulong deleted = g_signal_connect(model, "row-deleted", G_CALLBACK(idle_row_deleted), &changes);
    /* Observe notifications as well as the final appearance: a setter that
     * replaces Başlat with the same text still destroys its GtkLabel child. */
    for (guint poll = 0; poll < 4; ++poll) {
        run_for(110); g_assert_true(gtk_bin_get_child(GTK_BIN(start)) == label);
        g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(start)), ==, "Başlat");
        g_assert_false(gtk_widget_is_sensitive(start));
        g_assert_true(gtk_combo_box_get_model(GTK_COMBO_BOX(targets)) == model);
    }
    g_assert_cmpuint(changes.label, ==, 0); g_assert_cmpuint(changes.sensitive, ==, 0);
    g_assert_cmpuint(changes.inserted, ==, 0); g_assert_cmpuint(changes.deleted, ==, 0);
    g_signal_handler_disconnect(start, label_changed); g_signal_handler_disconnect(start, sensitive_changed);
    g_signal_handler_disconnect(model, inserted); g_signal_handler_disconnect(model, deleted);
    g_object_unref(label); g_object_unref(model); harness_free(h);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check(&argc, &argv)) { g_test_message("A GTK display is required for live-player callbacks."); return 77; }
    g_test_add_func("/live-players/replacement-late-reply", test_replacement_late_reply);
    g_test_add_func("/live-players/stop-invalidates-lookup", test_stop_invalidates_lookup);
    g_test_add_func("/live-players/stopped-next-loads-current", test_stopped_next_loads_current);
    g_test_add_func("/live-players/stopped-next-invalidates-old-lookup", test_stopped_next_invalidates_old_lookup);
    g_test_add_func("/live-players/current-removal-loads-current", test_removed_current_does_not_resume_old_media);
    g_test_add_func("/live-players/pending-lookup-keeps-current-on-old-EOF", test_lookup_pending_does_not_skip_on_old_eof);
    g_test_add_func("/live-players/failed-lookup-retries-current", test_failed_lookup_can_retry_same_current);
    g_test_add_func("/live-players/lookup-version-mismatch-rejected", test_lookup_version_mismatch_rejected);
    g_test_add_func("/live-players/lookup-media-mismatch-rejected", test_lookup_media_mismatch_rejected);
    g_test_add_func("/live-players/clear-after-stop-retires-lookup", test_clear_after_stop_rejects_late_lookup);
    g_test_add_func("/live-players/direct-library-retires-queue-lookup", test_direct_library_selection_retires_queue_lookup);
    g_test_add_func("/live-players/external-library-load-preserves-direct-selection", test_external_library_load_preserves_direct_selection);
    g_test_add_func("/live-players/external-library-load-retires-pending-lookup", test_external_library_load_retires_pending_lookup);
    g_test_add_func("/live-players/repeated-media-next-requires-fresh-lookup", test_repeated_media_next_requires_fresh_lookup);
    g_test_add_func("/live-players/managed-takeover-retires-local-lookup", test_managed_takeover_retires_local_lookup);
    g_test_add_func("/live-players/idle-start-and-targets-keep-widgets", test_idle_start_and_targets_keep_widgets);
    return g_test_run();
}
