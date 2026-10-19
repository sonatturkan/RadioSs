#include "radios/midi_ui.h"
#include "radios/midi.h"
#include "radios/audio_ui.h"
#include "radios/programme_model.h"
#include "radios/preferences.h"
#include <math.h>
#include <string.h>

#define SERVICE_KEY "radios-midi-service"
#define MIDI_PLAYERS 6
static const gchar *const deck_actions[] = {"playpause", "play", "pause", "stop", "next", "cue", "sync", "volume"};
static const gchar *const deck_action_labels[] = {"Oynat / Duraklat", "Oynat", "Duraklat", "Durdur", "Sonraki parça", "CUE", "SYNC", "Ses seviyesi"};

typedef struct {
    gint refs;                   /* the window and each pending device job */
    gboolean disposed;           /* the window is gone: pending jobs only clean up */
    RadioSViewContext *ctx;
    GtkWidget *window;
    RadioSDocument *document;
    JsonNode *config;            /* {"device","active","maps":[{"name","device","bindings"}]} */
    GMainContext *context;       /* the window's: events, notices and job results */
    RadioSMidiInput *input;      /* the connected device, or NULL */
    RadioSMidiInput *watch;      /* a lost input (its device released), kept only for its hotplug notices */
    GPtrArray *ports;            /* RadioSMidiPort* of the last background listing; NULL before it */
    GCancellable *cancel;
    gboolean busy, again;        /* one listing/connect job at a time; another one is due */
    gchar *device, *last_key;
    gchar *in_use;               /* the configured device, listed but held by another application */
    GHashTable *bindings, *pressed; /* active map: control -> action; control -> last pressed */
    GPtrArray *seen;             /* controls seen this session, in order */
    GPtrArray *pages;            /* GWeakRef* of open settings pages */
    GPtrArray *combos;           /* GWeakRef* of Technical page map selectors */
    guint64 events;              /* controller messages received */
    guint verifying;             /* open check dialogs: record, never dispatch */
    guint timer;
} MidiService;

typedef struct {
    MidiService *service;
    GtkWidget *root, *devices, *status, *active, *tree, *action, *assign, *clear, *load;
    GtkListStore *store;
    gboolean updating;
} MidiPage;
enum { ROW_KEY, ROW_LABEL, ROW_ASSIGNMENT, ROW_COLUMNS };

gchar *radios_midi_ui_action_label(const gchar *action)
{
    if (!action) return g_strdup("Atanmamış");
    if (g_str_equal(action, "master.volume")) return g_strdup("Master ses seviyesi");
    if (g_str_equal(action, "microphone.toggle")) return g_strdup("Mikrofon aç / kapat");
    guint deck = 0; gchar name[24] = {0};
    if (sscanf(action, "deck%u.%23s", &deck, name) == 2 && deck >= 1 && deck <= 32)
        for (guint i = 0; i < G_N_ELEMENTS(deck_actions); ++i)
            if (g_str_equal(name, deck_actions[i])) return g_strdup_printf("Oynatıcı %u · %s", deck, deck_action_labels[i]);
    return g_strdup(action);
}
static JsonObject *config_object(MidiService *service) { return json_node_get_object(service->config); }
static JsonArray *maps_of(MidiService *service) { return json_object_get_array_member(config_object(service), "maps"); }
static JsonObject *map_named(MidiService *service, const gchar *name)
{
    JsonArray *maps = maps_of(service);
    for (guint i = 0; name && maps && i < json_array_get_length(maps); ++i) {
        JsonObject *map = json_array_get_object_element(maps, i);
        if (g_str_equal(radios_json_string(map, "name", ""), name)) return map;
    }
    return NULL;
}
static JsonObject *active_map(MidiService *service)
{ return map_named(service, radios_json_string(config_object(service), "active", "")); }
static void bindings_rebuild(MidiService *service)
{
    g_hash_table_remove_all(service->bindings);
    JsonObject *map = active_map(service); JsonArray *bindings = map ? json_object_get_array_member(map, "bindings") : NULL;
    for (guint i = 0; bindings && i < json_array_get_length(bindings); ++i) {
        JsonObject *binding = json_array_get_object_element(bindings, i);
        g_hash_table_replace(service->bindings, g_strdup(radios_json_string(binding, "control", "")), g_strdup(radios_json_string(binding, "action", "")));
    }
}
static void pages_refresh(MidiService *service, gboolean devices);

/* --- action dispatch ---------------------------------------------------- */
static GtkWidget *find_named(GtkWidget *widget, const gchar *name)
{
    if (!widget) return NULL;
    if (!g_strcmp0(gtk_widget_get_name(widget), name)) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget)); GtkWidget *found = NULL;
    for (GList *child = children; child && !found; child = child->next) found = find_named(child->data, name);
    g_list_free(children); return found;
}
static GtkWidget *deck_widget(MidiService *service, guint deck, const gchar *suffix)
{
    gchar *name = g_strdup_printf("deck-%u-%s", deck, suffix); GtkWidget *widget = find_named(service->window, name); g_free(name);
    return widget && gtk_widget_is_sensitive(widget) ? widget : NULL;
}
/* Faders use a square law: full travel is unity gain, half travel -12 dB. */
static gdouble fader_db(gdouble value) { return value <= 0.001 ? -60 : MAX(-60.0, 40 * log10(value)); }
static void dispatch(MidiService *service, const gchar *action, const RadioSMidiEvent *event, gboolean pressed_edge)
{
    if (g_str_equal(action, "master.volume")) { radios_audio_ui_set_master_gain(service->ctx, MIN(6.0, fader_db(event->value)), NULL); return; }
    if (g_str_equal(action, "microphone.toggle")) {
        if (pressed_edge) radios_audio_ui_microphone(service->ctx, !radios_audio_ui_microphone_active(service->ctx), NULL);
        return;
    }
    guint deck = 0; gchar name[24] = {0};
    if (sscanf(action, "deck%u.%23s", &deck, name) != 2 || deck < 1 || deck > 32) return;
    guint index = deck - 1;
    if (g_str_equal(name, "volume")) {
        GtkWidget *scale = deck_widget(service, index, "volume");
        if (GTK_IS_RANGE(scale)) gtk_range_set_value(GTK_RANGE(scale), MIN(0.0, fader_db(event->value)));
        return;
    }
    if (!pressed_edge) return;
    if (g_str_equal(name, "sync")) {
        GtkWidget *toggle = deck_widget(service, index, "sync");
        if (GTK_IS_TOGGLE_BUTTON(toggle)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle)));
        return;
    }
    const gchar *button = name;
    if (g_str_equal(name, "playpause")) {
        RadioSAudioUiSnapshot snapshot = {0};
        button = radios_audio_ui_get_snapshot(service->ctx, index, &snapshot) && snapshot.state == RADIOS_AUDIO_UI_PLAYING ? "pause" : "play";
    }
    GtkWidget *widget = deck_widget(service, index, button);
    if (GTK_IS_BUTTON(widget)) gtk_button_clicked(GTK_BUTTON(widget));
}
static void midi_event(const RadioSMidiEvent *event, gpointer data)
{
    MidiService *service = data; if (service->disposed) return;
    gchar *key = radios_midi_event_key(event);
    gboolean was = GPOINTER_TO_INT(g_hash_table_lookup(service->pressed, key));
    g_hash_table_replace(service->pressed, g_strdup(key), GINT_TO_POINTER(event->pressed));
    gboolean known = FALSE;
    for (guint i = 0; i < service->seen->len && !known; ++i) known = g_str_equal(g_ptr_array_index(service->seen, i), key);
    if (!known) g_ptr_array_add(service->seen, g_strdup(key));
    g_free(service->last_key); service->last_key = g_strdup(key); ++service->events;
    const gchar *action = g_hash_table_lookup(service->bindings, key);
    if (action && !service->verifying) dispatch(service, action, event, event->pressed && !was);
    pages_refresh(service, FALSE); g_free(key);
}

/* --- device connection -------------------------------------------------- */
/* Listing the WinMM MIDI devices and opening one can stall for hundreds of
 * milliseconds while a USB device is being enumerated or the Windows MIDI
 * service starts (PDF R8.4). They run in a GTask, one job at a time; the
 * result is applied here on the main loop. */
static MidiService *service_ref(MidiService *service) { g_atomic_int_inc(&service->refs); return service; }
static void service_unref(MidiService *service)
{
    if (!g_atomic_int_dec_and_test(&service->refs)) return;
    json_node_free(service->config); g_free(service->device); g_free(service->last_key); g_free(service->in_use);
    g_hash_table_unref(service->bindings); g_hash_table_unref(service->pressed);
    g_ptr_array_unref(service->seen); g_ptr_array_unref(service->pages); g_ptr_array_unref(service->combos);
    if (service->ports) g_ptr_array_unref(service->ports);
    g_object_unref(service->cancel); g_main_context_unref(service->context); g_free(service);
}
typedef struct {
    MidiService *service;
    GMainContext *context;
    gchar *wanted;           /* the configured device name, "" for none */
    gboolean connect;        /* open it when it is listed */
    GPtrArray *ports;        /* result: the listing */
    RadioSMidiInput *input;  /* result: the opened device, unless the service took it */
    GError *error;           /* result: why the listed device could not be opened */
} MidiJob;
static void midi_notice(RadioSMidiNotice notice, gpointer data);
static void job_free(gpointer data)
{
    MidiJob *job = data;
    if (job->input) radios_midi_input_close(job->input);
    if (job->ports) g_ptr_array_unref(job->ports);
    g_clear_error(&job->error);
    g_main_context_unref(job->context); g_free(job->wanted); service_unref(job->service); g_free(job);
}
static void job_run(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; MidiJob *job = data;
    job->ports = radios_midi_list_inputs(NULL);
    for (guint i = 0; job->connect && job->ports && i < job->ports->len && !job->input && !g_cancellable_is_cancelled(cancel); ++i) {
        RadioSMidiPort *port = g_ptr_array_index(job->ports, i);
        if (!g_str_equal(port->name, job->wanted)) continue;
        g_clear_error(&job->error);
        job->input = radios_midi_input_open_full(port->id, job->context, midi_event, midi_notice, job->service, &job->error);
    }
    g_task_return_boolean(task, TRUE);
}
static void job_start(MidiService *service);
static void job_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; (void)data; MidiJob *job = g_task_get_task_data(G_TASK(result)); MidiService *service = job->service;
    service->busy = FALSE;
    if (service->disposed) return;
    if (service->ports) g_ptr_array_unref(service->ports);
    service->ports = job->ports ? g_steal_pointer(&job->ports) : g_ptr_array_new_with_free_func(g_free);
    /* A device opened for a name that has since changed, or unplugged while
     * it was being opened, is closed again (job_free). */
    if (job->input && !service->input && radios_midi_input_alive(job->input) &&
        g_str_equal(job->wanted, radios_json_string(config_object(service), "device", ""))) {
        g_clear_pointer(&service->watch, radios_midi_input_close);
        service->input = g_steal_pointer(&job->input); g_free(service->device); service->device = g_strdup(job->wanted);
    }
    /* Listed but held by another application (WinMM input is exclusive per
     * device): said so instead of "takılı değil"; a retry connects once the
     * other application lets it go. */
    if (job->connect) {
        g_free(service->in_use);
        service->in_use = !service->input && g_error_matches(job->error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_BUSY) ? g_strdup(job->wanted) : NULL;
    }
    pages_refresh(service, TRUE);
    if (service->again) { service->again = FALSE; job_start(service); }
}
/* Lists the ports for the settings page and, when the configured device is
 * not connected, connects it if it is listed. */
static void job_start(MidiService *service)
{
    if (service->disposed) return;
    if (service->busy) { service->again = TRUE; return; }
    MidiJob *job = g_new0(MidiJob, 1); job->service = service_ref(service);
    job->context = g_main_context_ref(service->context);
    job->wanted = g_strdup(radios_json_string(config_object(service), "device", ""));
    job->connect = *job->wanted && !service->input;
    service->busy = TRUE;
    GTask *task = g_task_new(NULL, service->cancel, job_done, NULL);
    g_task_set_task_data(task, job, job_free);
    g_task_run_in_thread(task, job_run);
    g_object_unref(task);
}
/* A lost device stops counting as connected ("takılı değil"). Its input has
 * already released the device and stays open only to hear the next MIDI
 * device arrive (a PORT notice), which starts a reconnect at once; it is
 * closed when a device connects or none is configured. */
static gboolean input_check(MidiService *service)
{
    if (!service->input || radios_midi_input_alive(service->input)) return FALSE;
    g_clear_pointer(&service->watch, radios_midi_input_close);
    service->watch = g_steal_pointer(&service->input); g_clear_pointer(&service->device, g_free);
    return TRUE;
}
static gboolean device_wanted(MidiService *service) { return *radios_json_string(config_object(service), "device", "") != '\0'; }
/* The configured device `wanted` is listed, but another application holds it. */
static gboolean device_held(MidiService *service, const gchar *wanted) { return *wanted && !g_strcmp0(service->in_use, wanted); }
static void midi_notice(RadioSMidiNotice notice, gpointer data)
{
    (void)notice; MidiService *service = data; if (service->disposed) return;
    if (input_check(service)) pages_refresh(service, TRUE);
    if (device_wanted(service) && !service->input) job_start(service);
}
/* Follows the configured device; `force` ("Yenile") closes even a live
 * connection and reconnects it. The old input is closed before the job
 * starts: WinMM input is exclusive per device, the new one could not open it
 * otherwise. */
static void connect_device(MidiService *service, gboolean force)
{
    const gchar *wanted = radios_json_string(config_object(service), "device", "");
    input_check(service);
    if (service->input && (force || g_strcmp0(service->device, wanted) != 0)) {
        g_clear_pointer(&service->input, radios_midi_input_close); g_clear_pointer(&service->device, g_free);
    }
    if (!*wanted) g_clear_pointer(&service->watch, radios_midi_input_close);
    if (force || (*wanted && !service->input)) job_start(service);
}
static gboolean reconnect_tick(gpointer data)
{
    MidiService *service = data;
    if (input_check(service)) pages_refresh(service, TRUE);
    if (device_wanted(service) && !service->input) job_start(service);
    return G_SOURCE_CONTINUE;
}
static void document_applied(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; (void)owner; MidiService *service = data;
    if (!config || !json_object_has_member(config, "maps")) return;
    JsonNode *copy = json_node_new(JSON_NODE_OBJECT); json_node_take_object(copy, radios_programme_clone_object(config));
    json_node_free(service->config); service->config = copy;
    bindings_rebuild(service); connect_device(service, FALSE); pages_refresh(service, TRUE);
}
static void service_save(MidiService *service)
{
    bindings_rebuild(service); connect_device(service, FALSE);
    if (service->document) radios_data_document_save(service->document, service->config);
    pages_refresh(service, TRUE);
}
static void service_free(gpointer data)
{
    MidiService *service = data; service->disposed = TRUE;
    if (service->timer) g_source_remove(service->timer);
    service->timer = 0;
    g_cancellable_cancel(service->cancel);
    g_clear_pointer(&service->input, radios_midi_input_close); g_clear_pointer(&service->watch, radios_midi_input_close);
    service_unref(service);
}
static void weak_free(gpointer data) { g_weak_ref_clear(data); g_free(data); }
void radios_midi_ui_bind(RadioSViewContext *ctx, GtkWidget *window)
{
    g_return_if_fail(ctx && GTK_IS_WIDGET(window));
    if (g_object_get_data(G_OBJECT(window), SERVICE_KEY)) return;
    MidiService *service = g_new0(MidiService, 1); service->refs = 1; service->ctx = ctx; service->window = window;
    service->context = g_main_context_ref_thread_default(); service->cancel = g_cancellable_new();
    service->config = json_node_new(JSON_NODE_OBJECT); JsonObject *config = json_object_new();
    json_object_set_string_member(config, "device", ""); json_object_set_array_member(config, "maps", json_array_new());
    json_node_take_object(service->config, config);
    service->bindings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    service->pressed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    service->seen = g_ptr_array_new_with_free_func(g_free); service->pages = g_ptr_array_new_with_free_func(weak_free);
    service->combos = g_ptr_array_new_with_free_func(weak_free);
    g_object_set_data_full(G_OBJECT(window), SERVICE_KEY, service, service_free);
    if (ctx->data) service->document = radios_data_document_new(ctx, window, "midi_mapping", "MIDI eşlemesi", document_applied, service);
    service->timer = g_timeout_add_seconds(5, reconnect_tick, service);
}

/* --- settings page ------------------------------------------------------ */
static MidiService *service_of(RadioSViewContext *ctx)
{ return ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), SERVICE_KEY) : NULL; }
static void page_render(MidiPage *page, gboolean devices)
{
    MidiService *service = page->service; page->updating = TRUE;
    if (devices) {
    const gchar *wanted = radios_json_string(config_object(service), "device", "");
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(page->devices));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(page->devices), "", "Cihaz seçilmedi");
    GPtrArray *ports = service->ports; gboolean present = !*wanted;
    for (guint i = 0; ports && i < ports->len; ++i) {
        RadioSMidiPort *port = g_ptr_array_index(ports, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(page->devices), port->name, port->name);
        present |= g_str_equal(port->name, wanted);
    }
    if (!present) { gchar *text = g_strdup_printf("Bağlı değil · %s", wanted); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(page->devices), wanted, text); g_free(text); }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(page->devices), wanted);
    gchar *status = service->input ? g_strdup_printf("Bağlı: %s", service->device) :
        device_held(service, wanted) ? g_strdup_printf("%s başka bir uygulama tarafından kullanılıyor. Diğer uygulamayı kapatın; bağlantı kendiliğinden kurulur.", wanted) :
        *wanted ? g_strdup_printf("%s takılı değil. Denetleyiciyi takın; bağlantı kendiliğinden kurulur.", wanted) :
        !ports ? g_strdup("MIDI cihazları aranıyor…") :
        ports->len ? g_strdup("Listeden MIDI cihazını seçin.") : g_strdup("MIDI cihazı bulunamadı. Denetleyiciyi takıp Yenile'ye basın.");
    gtk_label_set_text(GTK_LABEL(page->status), status); g_free(status);
    }
    /* Map and keys */
    JsonObject *map = active_map(service);
    gchar *active = map ? g_strdup_printf("Etkin harita: %s", radios_json_string(map, "name", "")) : g_strdup("Etkin harita yok. Harita oluşturun ya da mevcut bir haritayı yükleyin.");
    gtk_label_set_text(GTK_LABEL(page->active), active); g_free(active);
    GPtrArray *keys = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter iter; gpointer key, value; g_hash_table_iter_init(&iter, service->bindings);
    while (g_hash_table_iter_next(&iter, &key, &value)) g_ptr_array_add(keys, g_strdup(key));
    for (guint i = 0; i < service->seen->len; ++i) {
        const gchar *seen = g_ptr_array_index(service->seen, i);
        if (!g_hash_table_contains(service->bindings, seen)) g_ptr_array_add(keys, g_strdup(seen));
    }
    gchar *selected = NULL; GtkTreeModel *model; GtkTreeIter row;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->tree)), &model, &row)) gtk_tree_model_get(model, &row, ROW_KEY, &selected, -1);
    if (service->last_key && page->root && gtk_widget_get_mapped(page->root)) { g_free(selected); selected = g_strdup(service->last_key); }
    gtk_list_store_clear(page->store);
    for (guint i = 0; i < keys->len; ++i) {
        const gchar *control = g_ptr_array_index(keys, i);
        gchar *label = radios_midi_key_label(control), *assignment = radios_midi_ui_action_label(g_hash_table_lookup(service->bindings, control));
        gtk_list_store_insert_with_values(page->store, &row, -1, ROW_KEY, control, ROW_LABEL, label, ROW_ASSIGNMENT, assignment, -1);
        if (selected && g_str_equal(selected, control)) gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->tree)), &row);
        g_free(label); g_free(assignment);
    }
    g_ptr_array_unref(keys); g_free(selected);
    gboolean chosen = gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->tree)), NULL, NULL);
    gtk_widget_set_sensitive(page->assign, map && chosen); gtk_widget_set_sensitive(page->clear, map && chosen);
    page->updating = FALSE;
}
static void combos_refresh(MidiService *service);
static void pages_refresh(MidiService *service, gboolean devices)
{
    for (guint i = 0; i < service->pages->len; ) {
        GtkWidget *root = g_weak_ref_get(g_ptr_array_index(service->pages, i));
        if (!root) { g_ptr_array_remove_index(service->pages, i); continue; }
        MidiPage *page = g_object_get_data(G_OBJECT(root), "radios-midi-page");
        if (page) page_render(page, devices);
        g_object_unref(root); ++i;
    }
    /* Controller presses only change the live lists; maps change with config. */
    if (devices) combos_refresh(service);
}
static void device_changed(GtkComboBox *combo, gpointer data)
{
    MidiPage *page = data; if (page->updating) return;
    const gchar *id = gtk_combo_box_get_active_id(combo); if (!id) return;
    json_object_set_string_member(config_object(page->service), "device", id); service_save(page->service);
}
/* "Yenile": lists the ports again and reconnects the device even when the
 * connection looks alive. */
static void refresh_clicked(GtkButton *button, gpointer data) { (void)button; MidiPage *page = data; connect_device(page->service, TRUE); page_render(page, TRUE); }
static void create_map(GtkButton *button, gpointer data)
{
    (void)button; MidiPage *page = data; MidiService *service = page->service; JsonObject *config = config_object(service);
    const gchar *device = radios_json_string(config, "device", ""); JsonArray *maps = maps_of(service);
    if (json_array_get_length(maps) >= 32) { gtk_label_set_text(GTK_LABEL(page->status), "En fazla 32 harita saklanabilir."); return; }
    gchar *name = NULL;
    for (guint n = 1; !name || map_named(service, name); ++n) {
        g_free(name); name = n == 1 ? g_strdup_printf("%s haritası", *device ? device : "Denetleyici") : g_strdup_printf("%s haritası %u", *device ? device : "Denetleyici", n);
        if (g_utf8_strlen(name, -1) > 80) { g_free(name); name = g_strdup_printf("Harita %u", n); }
    }
    JsonObject *map = json_object_new(); json_object_set_string_member(map, "name", name);
    json_object_set_string_member(map, "device", device); json_object_set_array_member(map, "bindings", json_array_new());
    json_array_add_object_element(maps, map); json_object_set_string_member(config, "active", name); g_free(name);
    service_save(service);
    gtk_label_set_text(GTK_LABEL(page->status), "Yeni harita etkin. Denetleyicide bir tuşa basın, listeden bir eylem seçip Ata'ya tıklayın.");
}
static void activate_map(GtkMenuItem *item, gpointer data)
{
    MidiPage *page = data; const gchar *name = g_object_get_data(G_OBJECT(item), "map-name");
    json_object_set_string_member(config_object(page->service), "active", name); service_save(page->service);
}
static gboolean import_map(MidiService *service, JsonObject *map, GError **error)
{
    JsonArray *bindings = map ? json_object_get_array_member(map, "bindings") : NULL; const gchar *name = radios_json_string(map, "name", "");
    if (!bindings || !*name || g_utf8_strlen(name, -1) > 80) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Harita adı ve eşlemeleri gerekli."); return FALSE; }
    JsonArray *clean = json_array_new(); GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < json_array_get_length(bindings) && i < 512; ++i) {
        JsonObject *binding = json_array_get_object_element(bindings, i);
        const gchar *control = radios_json_string(binding, "control", ""), *action = radios_json_string(binding, "action", "");
        gchar *label = radios_midi_ui_action_label(action); gboolean known = !g_str_equal(label, action); g_free(label);
        if (!radios_midi_key_valid(control) || !known || g_hash_table_contains(seen, control)) continue;
        g_hash_table_add(seen, (gpointer)control);
        JsonObject *row = json_object_new(); json_object_set_string_member(row, "control", control); json_object_set_string_member(row, "action", action);
        json_array_add_object_element(clean, row);
    }
    g_hash_table_unref(seen);
    JsonArray *maps = maps_of(service);
    for (guint i = 0; i < json_array_get_length(maps); ++i)
        if (g_str_equal(radios_json_string(json_array_get_object_element(maps, i), "name", ""), name)) { json_array_remove_element(maps, i); break; }
    JsonObject *copy = json_object_new(); json_object_set_string_member(copy, "name", name);
    json_object_set_string_member(copy, "device", radios_json_string(map, "device", radios_json_string(config_object(service), "device", "")));
    json_object_set_array_member(copy, "bindings", clean); json_array_add_object_element(maps, copy);
    json_object_set_string_member(config_object(service), "active", name); return TRUE;
}
static void load_file(GtkMenuItem *item, gpointer data)
{
    (void)item; MidiPage *page = data;
    GtkWidget *chooser = gtk_file_chooser_dialog_new("MIDI haritası yükle", GTK_WINDOW(gtk_widget_get_toplevel(page->root)), GTK_FILE_CHOOSER_ACTION_OPEN,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Yükle", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileFilter *filter = gtk_file_filter_new(); gtk_file_filter_set_name(filter, "RadioS MIDI haritası (JSON)"); gtk_file_filter_add_pattern(filter, "*.json");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser)); GError *error = NULL; JsonParser *parser = json_parser_new();
        gboolean ok = json_parser_load_from_file(parser, path, &error) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser));
        JsonObject *root = ok ? json_node_get_object(json_parser_get_root(parser)) : NULL;
        JsonArray *maps = root && json_object_has_member(root, "maps") ? json_object_get_array_member(root, "maps") : NULL;
        guint imported = 0;
        if (maps) for (guint i = 0; i < json_array_get_length(maps); ++i) imported += import_map(page->service, json_array_get_object_element(maps, i), NULL);
        else if (root) imported += import_map(page->service, root, &error);
        if (imported) service_save(page->service);
        gchar *message = imported ? g_strdup_printf("%u harita yüklendi.", imported) : g_strdup_printf("Harita yüklenemedi: %s", error ? error->message : "geçerli eşleme yok");
        gtk_label_set_text(GTK_LABEL(page->status), message); g_free(message); g_clear_error(&error); g_object_unref(parser); g_free(path);
    }
    gtk_widget_destroy(chooser);
}
static void load_clicked(GtkButton *button, gpointer data)
{
    MidiPage *page = data; GtkWidget *menu = gtk_menu_new(); JsonArray *maps = maps_of(page->service);
    for (guint i = 0; i < json_array_get_length(maps); ++i) {
        const gchar *name = radios_json_string(json_array_get_object_element(maps, i), "name", "");
        GtkWidget *item = gtk_menu_item_new_with_label(name);
        g_object_set_data_full(G_OBJECT(item), "map-name", g_strdup(name), g_free);
        g_signal_connect(item, "activate", G_CALLBACK(activate_map), page); gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    if (json_array_get_length(maps)) gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *file = gtk_menu_item_new_with_label("Dosyadan yükle…");
    g_signal_connect(file, "activate", G_CALLBACK(load_file), page); gtk_menu_shell_append(GTK_MENU_SHELL(menu), file);
    gtk_widget_show_all(menu); gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(button), GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST, NULL);
}
static void binding_set(MidiPage *page, const gchar *action)
{
    MidiService *service = page->service; JsonObject *map = active_map(service); GtkTreeModel *model; GtkTreeIter row; gchar *control = NULL;
    if (!map || !gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->tree)), &model, &row)) return;
    gtk_tree_model_get(model, &row, ROW_KEY, &control, -1);
    JsonArray *bindings = json_object_get_array_member(map, "bindings");
    for (guint i = 0; i < json_array_get_length(bindings); ++i)
        if (g_str_equal(radios_json_string(json_array_get_object_element(bindings, i), "control", ""), control)) { json_array_remove_element(bindings, i); break; }
    if (action) {
        JsonObject *binding = json_object_new(); json_object_set_string_member(binding, "control", control); json_object_set_string_member(binding, "action", action);
        json_array_add_object_element(bindings, binding);
    }
    g_free(control); service_save(service);
}
static void assign_clicked(GtkButton *button, gpointer data)
{ (void)button; MidiPage *page = data; const gchar *action = gtk_combo_box_get_active_id(GTK_COMBO_BOX(page->action)); if (action) binding_set(page, action); }
static void clear_clicked(GtkButton *button, gpointer data) { (void)button; binding_set(data, NULL); }
static void selection_changed(GtkTreeSelection *selection, gpointer data)
{
    MidiPage *page = data; if (page->updating) return; GtkTreeModel *model; GtkTreeIter row;
    gboolean chosen = gtk_tree_selection_get_selected(selection, &model, &row); gboolean mapped = active_map(page->service) != NULL;
    gtk_widget_set_sensitive(page->assign, mapped && chosen); gtk_widget_set_sensitive(page->clear, mapped && chosen);
    g_clear_pointer(&page->service->last_key, g_free);
    if (chosen) {
        gchar *control = NULL; gtk_tree_model_get(model, &row, ROW_KEY, &control, -1);
        const gchar *action = g_hash_table_lookup(page->service->bindings, control);
        if (action) gtk_combo_box_set_active_id(GTK_COMBO_BOX(page->action), action);
        g_free(control);
    }
}
static GtkWidget *section(const gchar *title, GtkWidget **body)
{
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(frame), "preferences-section");
    GtkWidget *heading = gtk_label_new(title); gtk_label_set_xalign(GTK_LABEL(heading), 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(heading), "preferences-heading");
    gtk_box_pack_start(GTK_BOX(frame), heading, FALSE, FALSE, 0); *body = frame; return frame;
}
static void page_free(gpointer data) { g_free(data); }
GtkWidget *radios_midi_ui_settings_new(RadioSViewContext *ctx)
{
    MidiService *service = service_of(ctx);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_widget_set_name(scroll, "preferences-midi");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_container_set_border_width(GTK_CONTAINER(body), 12);
    gtk_container_add(GTK_CONTAINER(scroll), body);
    if (!service) {
        GtkWidget *note = gtk_label_new("MIDI hizmeti bu pencerede çalışmıyor."); gtk_box_pack_start(GTK_BOX(body), note, FALSE, FALSE, 0); return scroll;
    }
    MidiPage *page = g_new0(MidiPage, 1); page->service = service; page->root = scroll;
    g_object_set_data_full(G_OBJECT(scroll), "radios-midi-page", page, page_free);
    GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, scroll); g_ptr_array_add(service->pages, weak);
    GtkWidget *device_section; gtk_box_pack_start(GTK_BOX(body), section("MIDI cihazı", &device_section), FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    page->devices = gtk_combo_box_text_new(); gtk_widget_set_name(page->devices, "preferences-midi-device"); gtk_widget_set_hexpand(page->devices, TRUE);
    GtkWidget *refresh = gtk_button_new_with_label("Yenile"); gtk_widget_set_name(refresh, "preferences-midi-refresh");
    gtk_box_pack_start(GTK_BOX(row), page->devices, TRUE, TRUE, 0); gtk_box_pack_start(GTK_BOX(row), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(device_section), row, FALSE, FALSE, 0);
    page->status = gtk_label_new(""); gtk_widget_set_name(page->status, "preferences-midi-status"); gtk_label_set_xalign(GTK_LABEL(page->status), 0);
    gtk_label_set_line_wrap(GTK_LABEL(page->status), TRUE); gtk_box_pack_start(GTK_BOX(device_section), page->status, FALSE, FALSE, 0);
    GtkWidget *map_section; gtk_box_pack_start(GTK_BOX(body), section("MIDI eşleme", &map_section), TRUE, TRUE, 0);
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *create = gtk_button_new_with_label("Harita oluştur"); gtk_widget_set_name(create, "preferences-midi-create");
    page->load = gtk_button_new_with_label("Mevcut haritayı yükle ▾"); gtk_widget_set_name(page->load, "preferences-midi-load");
    gtk_box_pack_start(GTK_BOX(actions), create, FALSE, FALSE, 0); gtk_box_pack_end(GTK_BOX(actions), page->load, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(map_section), actions, FALSE, FALSE, 0);
    page->active = gtk_label_new(""); gtk_widget_set_name(page->active, "preferences-midi-active"); gtk_label_set_xalign(GTK_LABEL(page->active), 0);
    gtk_box_pack_start(GTK_BOX(map_section), page->active, FALSE, FALSE, 0);
    page->store = gtk_list_store_new(ROW_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    page->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(page->store)); g_object_unref(page->store);
    gtk_widget_set_name(page->tree, "preferences-midi-keys");
    gtk_tree_view_append_column(GTK_TREE_VIEW(page->tree), gtk_tree_view_column_new_with_attributes("MIDI anahtarı", gtk_cell_renderer_text_new(), "text", ROW_LABEL, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(page->tree), gtk_tree_view_column_new_with_attributes("Atama", gtk_cell_renderer_text_new(), "text", ROW_ASSIGNMENT, NULL));
    GtkWidget *keys = gtk_scrolled_window_new(NULL, NULL); gtk_widget_set_size_request(keys, -1, 240);
    gtk_container_add(GTK_CONTAINER(keys), page->tree); gtk_box_pack_start(GTK_BOX(map_section), keys, TRUE, TRUE, 0);
    GtkWidget *assign_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(assign_row), gtk_label_new("Seçili anahtar:"), FALSE, FALSE, 0);
    page->action = gtk_combo_box_text_new(); gtk_widget_set_name(page->action, "preferences-midi-action"); gtk_widget_set_hexpand(page->action, TRUE);
    for (guint deck = 1; deck <= MIDI_PLAYERS; ++deck)
        for (guint i = 0; i < G_N_ELEMENTS(deck_actions); ++i) {
            gchar *id = g_strdup_printf("deck%u.%s", deck, deck_actions[i]), *text = radios_midi_ui_action_label(id);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(page->action), id, text); g_free(id); g_free(text);
        }
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(page->action), "master.volume", "Master ses seviyesi");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(page->action), "microphone.toggle", "Mikrofon aç / kapat");
    gtk_combo_box_set_active(GTK_COMBO_BOX(page->action), 0);
    page->assign = gtk_button_new_with_label("Ata"); gtk_widget_set_name(page->assign, "preferences-midi-assign");
    page->clear = gtk_button_new_with_label("Atamayı kaldır"); gtk_widget_set_name(page->clear, "preferences-midi-clear");
    gtk_box_pack_start(GTK_BOX(assign_row), page->action, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(assign_row), page->assign, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(assign_row), page->clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(map_section), assign_row, FALSE, FALSE, 0);
    g_signal_connect(page->devices, "changed", G_CALLBACK(device_changed), page);
    g_signal_connect(refresh, "clicked", G_CALLBACK(refresh_clicked), page);
    g_signal_connect(create, "clicked", G_CALLBACK(create_map), page);
    g_signal_connect(page->load, "clicked", G_CALLBACK(load_clicked), page);
    g_signal_connect(page->assign, "clicked", G_CALLBACK(assign_clicked), page);
    g_signal_connect(page->clear, "clicked", G_CALLBACK(clear_clicked), page);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->tree)), "changed", G_CALLBACK(selection_changed), page);
    page_render(page, TRUE);
    job_start(service); /* a fresh device list for the page */
    return scroll;
}

/* --- Technical page ----------------------------------------------------- */
static void combo_render(MidiService *service, GtkWidget *combo)
{
    g_object_set_data(G_OBJECT(combo), "radios-midi-updating", GINT_TO_POINTER(1));
    JsonArray *maps = maps_of(service); JsonObject *map = active_map(service);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "",
        maps && json_array_get_length(maps) ? "Harita seçilmedi" : "Harita yok · Ayarlar › MIDI");
    for (guint i = 0; maps && i < json_array_get_length(maps); ++i) {
        const gchar *name = radios_json_string(json_array_get_object_element(maps, i), "name", "");
        if (*name) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), name, name);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), map ? radios_json_string(map, "name", "") : "");
    const gchar *wanted = radios_json_string(config_object(service), "device", "");
    gchar *tip = service->input ? g_strdup_printf("Bağlı denetleyici: %s. Seçilen harita hemen etkinleşir ve kaydedilir.", service->device) :
        device_held(service, wanted) ? g_strdup_printf("%s başka bir uygulama tarafından kullanılıyor. Seçilen harita hemen etkinleşir ve kaydedilir.", wanted) :
        *wanted ? g_strdup_printf("%s takılı değil. Seçilen harita hemen etkinleşir ve kaydedilir.", wanted) :
        g_strdup("MIDI cihazı Ayarlar › MIDI bölümünden seçilir. Seçilen harita hemen etkinleşir ve kaydedilir.");
    gtk_widget_set_tooltip_text(combo, tip); g_free(tip);
    g_object_set_data(G_OBJECT(combo), "radios-midi-updating", NULL);
}
static void combos_refresh(MidiService *service)
{
    for (guint i = 0; service->combos && i < service->combos->len; ) {
        GtkWidget *combo = g_weak_ref_get(g_ptr_array_index(service->combos, i));
        if (!combo) { g_ptr_array_remove_index(service->combos, i); continue; }
        combo_render(service, combo); g_object_unref(combo); ++i;
    }
}
static void combo_changed(GtkComboBox *combo, gpointer data)
{
    MidiService *service = data;
    if (g_object_get_data(G_OBJECT(combo), "radios-midi-updating")) return;
    const gchar *id = gtk_combo_box_get_active_id(combo); JsonObject *map = active_map(service);
    if (!id || !g_strcmp0(id, map ? radios_json_string(map, "name", "") : "")) return;
    /* The stored document only accepts an existing map name or no active map. */
    if (*id) json_object_set_string_member(config_object(service), "active", id);
    else json_object_remove_member(config_object(service), "active");
    gchar *text = *id ? g_strdup_printf("MIDI haritası etkinleştirildi: %s", id) : g_strdup("MIDI haritası kapatıldı; denetleyici eylem çalıştırmaz.");
    service_save(service);
    if (service->ctx->notify) service->ctx->notify(service->ctx->notify_data, text);
    g_free(text);
}

typedef struct {
    MidiService *service;
    GtkWidget *dialog, *device, *map, *count, *last, *result;
    GWeakRef button;
    guint64 start_events;
    guint timer;
} MidiCheck;
static void label_text(GtkWidget *label, const gchar *text)
{ if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), text)) gtk_label_set_text(GTK_LABEL(label), text); }
static gboolean check_tick(gpointer data)
{
    MidiCheck *check = data; MidiService *service = check->service;
    const gchar *wanted = radios_json_string(config_object(service), "device", "");
    gchar *text = service->input ? g_strdup_printf("Cihaz: %s · bağlı", service->device) :
        device_held(service, wanted) ? g_strdup_printf("Cihaz: %s · başka bir uygulama kullanıyor (bağlantı 5 sn'de bir yeniden denenir)", wanted) :
        *wanted ? g_strdup_printf("Cihaz: %s · takılı değil (bağlantı 5 sn'de bir yeniden denenir)", wanted) :
        g_strdup("Cihaz seçilmedi · Ayarlar › MIDI bölümünden denetleyiciyi seçin");
    label_text(check->device, text); g_free(text);
    JsonObject *map = active_map(service);
    text = map ? g_strdup_printf("Etkin harita: %s · %u atama", radios_json_string(map, "name", ""), g_hash_table_size(service->bindings)) :
        g_strdup("Etkin harita yok · tuşlar bir eyleme atanmamış");
    label_text(check->map, text); g_free(text);
    guint64 received = service->events - check->start_events;
    text = g_strdup_printf("Bu pencere açıldığından beri alınan MIDI mesajı: %" G_GUINT64_FORMAT, received);
    label_text(check->count, text); g_free(text);
    const gchar *action = received && service->last_key ? g_hash_table_lookup(service->bindings, service->last_key) : NULL;
    if (received && service->last_key) {
        gchar *key = radios_midi_key_label(service->last_key), *assignment = radios_midi_ui_action_label(action);
        text = g_strdup_printf("Son kontrol: %s → %s", key, assignment); g_free(key); g_free(assignment);
    } else text = g_strdup("Son kontrol: —");
    label_text(check->last, text); g_free(text);
    label_text(check->result, !service->input ? "✗ Denetleyici bağlı değil." :
        !received ? "Denetleyicide bir tuşa basın ya da bir düğmeyi çevirin…" :
        !map ? "✓ Denetleyici yanıt veriyor. Eylem atamak için Ayarlar › MIDI'de harita oluşturun." :
        action ? "✓ Denetleyici ve eşleme çalışıyor. Bu pencere kapanınca tuşlar eylemlerini yeniden çalıştırır." :
        "✓ Denetleyici yanıt veriyor · son basılan kontrol bu haritada atanmamış.");
    return G_SOURCE_CONTINUE;
}
static void check_destroyed(GtkWidget *dialog, gpointer data)
{
    (void)dialog; MidiCheck *check = data;
    if (check->timer) g_source_remove(check->timer);
    check->timer = 0;
    if (check->service->verifying) --check->service->verifying;
    GtkWidget *button = g_weak_ref_get(&check->button);
    if (button) { g_object_set_data(G_OBJECT(button), "radios-midi-check", NULL); g_object_unref(button); }
}
static void check_free(gpointer data)
{ MidiCheck *check = data; g_weak_ref_clear(&check->button); g_free(check); }
static void check_response(GtkDialog *dialog, gint response, gpointer data)
{
    MidiCheck *check = data;
    if (response == GTK_RESPONSE_APPLY) {
        RadioSViewContext *ctx = check->service->ctx;
        GtkWidget *preferences = radios_preferences_open(ctx);
        GtkWidget *categories = preferences ? find_named(preferences, "preferences-categories") : NULL;
        GtkWidget *row = preferences ? find_named(preferences, "preferences-category-midi") : NULL;
        if (GTK_IS_LIST_BOX(categories) && GTK_IS_LIST_BOX_ROW(row)) gtk_list_box_select_row(GTK_LIST_BOX(categories), GTK_LIST_BOX_ROW(row));
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void check_open(GtkButton *button, gpointer data)
{
    MidiService *service = data; GtkWidget *open = g_object_get_data(G_OBJECT(button), "radios-midi-check");
    if (open) { gtk_window_present(GTK_WINDOW(open)); return; }
    MidiCheck *check = g_new0(MidiCheck, 1); check->service = service; check->start_events = service->events;
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(button));
    check->dialog = gtk_dialog_new_with_buttons("MIDI denetleyici doğrulaması", GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : NULL,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Ayarlar › MIDI", GTK_RESPONSE_APPLY, "Kapat", GTK_RESPONSE_CLOSE, NULL);
    gtk_widget_set_name(check->dialog, "technical-midi-check");
    gtk_style_context_add_class(gtk_widget_get_style_context(check->dialog), "radios");
    gtk_window_set_default_size(GTK_WINDOW(check->dialog), 520, -1);
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(check->dialog));
    gtk_container_set_border_width(GTK_CONTAINER(body), 16); gtk_box_set_spacing(GTK_BOX(body), 8);
    GtkWidget **labels[] = {&check->device, &check->map, &check->count, &check->last, &check->result};
    const gchar *names[] = {"technical-midi-check-device", "technical-midi-check-map", "technical-midi-check-count",
                            "technical-midi-check-last", "technical-midi-check-result"};
    for (guint i = 0; i < G_N_ELEMENTS(labels); ++i) {
        *labels[i] = gtk_label_new(""); gtk_widget_set_name(*labels[i], names[i]);
        gtk_label_set_xalign(GTK_LABEL(*labels[i]), 0); gtk_label_set_line_wrap(GTK_LABEL(*labels[i]), TRUE);
        gtk_box_pack_start(GTK_BOX(body), *labels[i], FALSE, FALSE, 0);
    }
    ++service->verifying;
    g_weak_ref_init(&check->button, button);
    g_object_set_data_full(G_OBJECT(check->dialog), "radios-midi-check-state", check, check_free);
    g_object_set_data(G_OBJECT(button), "radios-midi-check", check->dialog);
    g_signal_connect(check->dialog, "destroy", G_CALLBACK(check_destroyed), check);
    g_signal_connect(check->dialog, "response", G_CALLBACK(check_response), check);
    check->timer = g_timeout_add(150, check_tick, check); check_tick(check);
    gtk_widget_show_all(check->dialog);
}
void radios_midi_ui_bind_technical(RadioSViewContext *ctx, GtkWidget *maps, GtkWidget *verify)
{
    MidiService *service = service_of(ctx);
    if (!service) {
        if (GTK_IS_WIDGET(verify)) gtk_widget_set_tooltip_text(verify, "MIDI hizmeti bu pencerede çalışmıyor.");
        return;
    }
    if (GTK_IS_COMBO_BOX_TEXT(maps)) {
        GWeakRef *weak = g_new0(GWeakRef, 1); g_weak_ref_init(weak, maps); g_ptr_array_add(service->combos, weak);
        combo_render(service, maps); g_signal_connect(maps, "changed", G_CALLBACK(combo_changed), service);
    }
    if (GTK_IS_BUTTON(verify)) {
        gtk_widget_set_sensitive(verify, TRUE);
        gtk_widget_set_tooltip_text(verify, "Bağlı denetleyiciden gelen tuşları ve haritadaki karşılıklarını canlı gösterir; eylem çalıştırmaz.");
        g_signal_connect(verify, "clicked", G_CALLBACK(check_open), service);
    }
}
void radios_midi_ui_technical_refresh(RadioSViewContext *ctx)
{ MidiService *service = service_of(ctx); if (service) combos_refresh(service); }
