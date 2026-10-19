#include "radios/header_navigation.h"
#include "radios/widgets.h"
#include "radios/data.h"
#include "radios/player_workspace.h"
#include "radios/player_scope_execution.h"
#include "radios/automation_scope.h"
#include "radios/automation_scope_ui.h"
#include "radios/broadcast_header.h"
#include "radios/station_time.h"
#include "radios/platform.h"
#include <string.h>

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *buttons[6];
    GtkWidget *programme; /* "Program akışı": saved weekly schemas */
    GtkWidget *override_button; /* "Şema yükle…": a temporary schema inside the main flow */
    JsonArray *records;         /* the saved weekly schemas listed last */
    gchar *override_id, *override_name;
    gchar *signature, *execution_message;
    RadioSAutomationScope execution_scope;
    gboolean tracking;
    guint timer, ticks;
    gboolean disposed, listing, starting;
} HeaderNavigation;

enum { ACTION_PLAY = 1 };

static HeaderNavigation *navigation_get(GtkWidget *owner)
{ return owner ? g_object_get_data(G_OBJECT(owner), "header-navigation-state") : NULL; }
static gboolean navigation_tick(gpointer data);

/* The scope the header Play drives when no broadcast queue can play: the
 * first player cluster, else the first player, that has an assigned weekly
 * schema; without any assignment, the first cluster receives the selection. */
static gboolean schema_scope(HeaderNavigation *state, RadioSAutomationScope *scope, gchar assigned[37])
{
    const RadioSPlayerTopology *topology = state->ctx->players ? radios_player_workspace_topology(state->ctx->players) : NULL;
    memset(scope, 0, sizeof *scope); assigned[0] = '\0';
    if (!topology) return FALSE;
    for (guint i = 0; i < topology->cluster_count; ++i)
        if (*topology->clusters[i].automation_id) {
            scope->kind = RADIOS_AUTOMATION_SCOPE_CLUSTER; g_strlcpy(scope->cluster_id, topology->clusters[i].id, sizeof scope->cluster_id);
            g_strlcpy(assigned, topology->clusters[i].automation_id, 37); return TRUE;
        }
    for (guint deck = 0; deck < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++deck) {
        const RadioSPlayerDefinition *player = &topology->players[deck];
        if (player->enabled && !*player->cluster_id && *player->automation_id) {
            scope->kind = RADIOS_AUTOMATION_SCOPE_PLAYER; scope->deck = deck;
            g_strlcpy(assigned, player->automation_id, 37); return TRUE;
        }
    }
    if (topology->cluster_count) {
        scope->kind = RADIOS_AUTOMATION_SCOPE_CLUSTER; g_strlcpy(scope->cluster_id, topology->clusters[0].id, sizeof scope->cluster_id);
        return TRUE;
    }
    return FALSE;
}

static RadioSAutoDJMode scope_mode(HeaderNavigation *state, const RadioSAutomationScope *scope)
{
    RadioSAutomationBinding binding;
    if (!radios_automation_scope_resolve(radios_player_workspace_topology(state->ctx->players), scope, &binding, NULL) || !binding.count)
        return RADIOS_AUTODJ_TRANSITIONS;
    return binding.autodj_mode == RADIOS_AUTODJ_SEMI || binding.count < 2 ? RADIOS_AUTODJ_SEMI : RADIOS_AUTODJ_TRANSITIONS;
}

/* Play may start the current programme of the schedule when the scope is idle. */
static gboolean schema_playable(HeaderNavigation *state)
{
    RadioSAutomationScope scope; gchar assigned[37];
    const gchar *selected = state->programme ? gtk_combo_box_get_active_id(GTK_COMBO_BOX(state->programme)) : NULL;
    if (state->starting || !radios_data_connected(state->ctx) || !schema_scope(state, &scope, assigned) || (!*assigned && (!selected || !*selected)))
        return FALSE;
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(state->ctx);
    return execution && !radios_player_scope_execution_busy(execution, &scope);
}

static void notify(HeaderNavigation *state, const gchar *message)
{ if (state->ctx->notify) state->ctx->notify(state->ctx->notify_data, message); }

/* Track the actual app-owned operation, never the currently viewed tree row.
 * Notifications change only when its state/event changes. */
static void schema_status(HeaderNavigation *state)
{
    if (!state->tracking) return;
    RadioSPlayerScopeExecutionSnapshot snapshot = {0};
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(state->ctx);
    if (!radios_player_scope_execution_snapshot(execution, &state->execution_scope, &snapshot)) {
        /* The tracked scope can disappear after completion or reconnect. A
         * missing operation must not leave its old preparing notice visible. */
        state->tracking = FALSE;
        g_free(state->execution_message); state->execution_message = g_strdup("Program akışı durduruldu.");
        notify(state, state->execution_message);
        return;
    }
    gchar *event = g_strdup_printf("%s%s", snapshot.event_name[0] ? snapshot.event_name : "Program akışı",
        snapshot.override_active ? " · geçici şema" : "");
    gchar *message = NULL;
    switch (snapshot.state) {
    case RADIOS_SCOPE_EXECUTION_PREPARING:
        message = g_strdup_printf("Program akışı hazırlanıyor: %s", event); break;
    case RADIOS_SCOPE_EXECUTION_PLAYING:
        message = g_strdup_printf("Program akışı yayında: %s", event); break;
    case RADIOS_SCOPE_EXECUTION_PAUSED:
        message = g_strdup_printf("Program akışı duraklatıldı: %s", event); break;
    case RADIOS_SCOPE_EXECUTION_WAITING:
        if (snapshot.starts_at_utc_us > g_get_real_time()) {
            GTimeZone *zone = radios_time_zone_new(radios_station_timezone(state->ctx));
            if (!zone) zone = g_time_zone_new_utc();
            GDateTime *utc = g_date_time_new_from_unix_utc(snapshot.starts_at_utc_us / G_TIME_SPAN_SECOND);
            GDateTime *local = utc ? g_date_time_to_timezone(utc, zone) : NULL;
            gchar *clock = local ? g_date_time_format(local, "%d.%m %H:%M") : g_strdup("program saati");
            message = g_strdup_printf("Program akışı hazır: %s · Başlangıç %s", event, clock);
            g_free(clock); if (local) g_date_time_unref(local); if (utc) g_date_time_unref(utc); g_time_zone_unref(zone);
        } else message = g_strdup_printf("Program akışı bekliyor: %s", event);
        break;
    case RADIOS_SCOPE_EXECUTION_ERROR:
        message = g_strdup(snapshot.error[0] ? snapshot.error : "Program akışı başlatılamadı."); state->tracking = FALSE; break;
    case RADIOS_SCOPE_EXECUTION_IDLE:
        message = g_strdup("Program akışı durduruldu."); state->tracking = FALSE; break;
    }
    if (g_strcmp0(message, state->execution_message)) {
        notify(state, message); g_free(state->execution_message); state->execution_message = g_strdup(message);
    }
    g_free(message); g_free(event);
}

typedef struct { GWeakRef owner; } SchemaCompletion;
static void schema_completion_free(gpointer value)
{ SchemaCompletion *completion = value; g_weak_ref_clear(&completion->owner); g_free(completion); }
static void schema_done(RadioSViewContext *ctx, const RadioSAutomationScope *scope, guint source, const GError *error, gpointer user)
{
    (void)ctx; (void)scope; (void)source; SchemaCompletion *completion = user;
    GtkWidget *owner = g_weak_ref_get(&completion->owner); HeaderNavigation *state = navigation_get(owner);
    if (state && !state->disposed) {
        if (error) { state->tracking = FALSE; notify(state, error->message); }
        else schema_status(state);
    }
    if (owner) g_object_unref(owner);
}

static void schema_start(HeaderNavigation *state, const RadioSAutomationScope *scope)
{
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(state->ctx); GError *error = NULL;
    if (!execution) { notify(state, "Program akışı bağlantısı hazır değil."); return; }
    SchemaCompletion *completion = g_new0(SchemaCompletion, 1);
    g_weak_ref_init(&completion->owner, gtk_widget_get_parent(state->programme));
    state->execution_scope = *scope; state->tracking = TRUE; g_clear_pointer(&state->execution_message, g_free);
    notify(state, "Program akışı hazırlanıyor…");
    if (!radios_player_scope_execution_play_schema(execution, scope, scope_mode(state, scope),
        schema_done, completion, schema_completion_free, &error)) {
        state->tracking = FALSE; notify(state, error ? error->message : "Program akışı başlatılamadı.");
    }
    g_clear_error(&error);
}

typedef struct { RadioSAutomationScope scope; RadioSAutomationBinding binding; } AssignJob;

static void assign_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer user)
{
    (void)ctx; AssignJob *job = user; HeaderNavigation *state = navigation_get(owner);
    if (!state || state->disposed) return;
    state->starting = FALSE;
    GError *failure = NULL;
    if (error || !result) { notify(state, error ? error->message : "Program akışı okunamadı."); return; }
    if (!radios_automation_scope_binding_current(radios_player_workspace_topology(state->ctx->players), &job->binding, &failure)) {
        notify(state, failure ? failure->message : "Oynatıcı kapsamı değişti."); g_clear_error(&failure); return;
    }
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(state->programme));
    if (!id || !radios_player_workspace_assign(state->ctx->players, &job->scope, id, &failure)) {
        notify(state, failure ? failure->message : "Program akışı atanamadı."); g_clear_error(&failure); return;
    }
    schema_start(state, &job->scope);
}

/* Header Play: an available broadcast queue keeps its own behaviour; otherwise
 * the selected (or assigned) weekly schema starts with the programme of now. */
static gboolean play_schema(HeaderNavigation *state)
{
    RadioSAutomationScope scope; gchar assigned[37];
    if (!schema_playable(state) || !schema_scope(state, &scope, assigned)) return FALSE;
    const gchar *selected = gtk_combo_box_get_active_id(GTK_COMBO_BOX(state->programme));
    if (!selected || !*selected || !g_strcmp0(selected, assigned)) { schema_start(state, &scope); return TRUE; }
    AssignJob *job = g_new0(AssignJob, 1); job->scope = scope;
    if (!radios_automation_scope_resolve(radios_player_workspace_topology(state->ctx->players), &scope, &job->binding, NULL)) { g_free(job); return FALSE; }
    GtkWidget *owner = gtk_widget_get_parent(state->programme);
    state->starting = radios_automation_scope_fetch(state->ctx, owner, selected, 0, assign_received, job, g_free);
    if (!state->starting) notify(state, "Program akışı bağlantısı hazır değil.");
    return TRUE;
}

static void programmes_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer user)
{
    (void)ctx; (void)user; HeaderNavigation *state = navigation_get(owner);
    if (!state || state->disposed) return;
    state->listing = FALSE;
    JsonObject *object = !error && result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    JsonArray *records = object && json_object_has_member(object, "records") ? json_object_get_array_member(object, "records") : NULL;
    if (!records) return;
    g_clear_pointer(&state->records, json_array_unref); state->records = json_array_ref(records);
    RadioSAutomationScope scope; gchar assigned[37]; schema_scope(state, &scope, assigned);
    GString *signature = g_string_new(assigned);
    for (guint i = 0; i < json_array_get_length(records); ++i) {
        JsonObject *row = json_array_get_object_element(records, i);
        g_string_append_printf(signature, "|%s=%s", json_object_get_string_member(row, "_id"), json_object_get_string_member(row, "name"));
    }
    if (!g_strcmp0(signature->str, state->signature)) { g_string_free(signature, TRUE); return; }
    g_free(state->signature); state->signature = g_string_free(signature, FALSE);
    const gchar *active = gtk_combo_box_get_active_id(GTK_COMBO_BOX(state->programme));
    gchar *keep = g_strdup(active && *active ? active : assigned);
    GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT(state->programme);
    gtk_combo_box_text_remove_all(combo); gtk_combo_box_text_append(combo, "", "Program akışı");
    for (guint i = 0; i < json_array_get_length(records); ++i) {
        JsonObject *row = json_array_get_object_element(records, i);
        const gchar *id = json_object_get_string_member(row, "_id");
        gchar *label = g_strdup_printf("%s%s", json_object_get_string_member(row, "name"), !g_strcmp0(id, assigned) ? " · atanmış" : "");
        gtk_combo_box_text_append(combo, id, label); g_free(label);
    }
    if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->programme), keep)) gtk_combo_box_set_active(GTK_COMBO_BOX(state->programme), 0);
    g_free(keep);
}

static void programmes_list(HeaderNavigation *state)
{
    if (state->listing || !radios_data_connected(state->ctx) || !radios_data_station_id(state->ctx)) return;
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); JsonObject *o = json_object_new(); json_node_take_object(body, o);
    json_object_set_string_member(o, "station_id", radios_data_station_id(state->ctx));
    json_object_set_string_member(o, "kind", "weekly_programme");
    state->listing = radios_data_request(state->ctx, gtk_widget_get_parent(state->programme), "POST", "/v1/workflows/desktop/list", body,
        programmes_received, NULL, NULL);
    json_node_free(body);
}

static gboolean active_scope(HeaderNavigation *state, RadioSAutomationScope *scope,
    RadioSPlayerScopeExecutionSnapshot *snapshot)
{
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(state->ctx);
    if (!execution) return FALSE;
    if (state->tracking && radios_player_scope_execution_snapshot(execution, &state->execution_scope, snapshot) &&
        snapshot->schema_active && snapshot->state != RADIOS_SCOPE_EXECUTION_IDLE && snapshot->state != RADIOS_SCOPE_EXECUTION_ERROR) {
        *scope = state->execution_scope; return TRUE;
    }
    RadioSBroadcastHeaderSnapshot header = {0};
    radios_broadcast_header_snapshot(state->ctx, &header);
    if (header.current && header.local_deck &&
        radios_player_workspace_running_context(state->ctx->players, header.deck)) {
        *scope = (RadioSAutomationScope){.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = header.deck};
        return radios_player_scope_execution_snapshot(execution, scope, snapshot);
    }
    if (header.current) return FALSE;
    for (guint i = 0; i < RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS; ++i) {
        *scope = (RadioSAutomationScope){.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = i};
        if (radios_player_scope_execution_snapshot(execution, scope, snapshot) && snapshot->source == i &&
            snapshot->schema_active && snapshot->state != RADIOS_SCOPE_EXECUTION_IDLE && snapshot->state != RADIOS_SCOPE_EXECUTION_ERROR) return TRUE;
    }
    return FALSE;
}
static gboolean scope_action_available(HeaderNavigation *state, const RadioSPlayerScopeExecutionSnapshot *snapshot, guint action)
{
    if (action == 1) return snapshot->state == RADIOS_SCOPE_EXECUTION_PAUSED;
    if (action == 2) return snapshot->state == RADIOS_SCOPE_EXECUTION_PLAYING;
    if (action == 3) return TRUE;
    RadioSViewContext *source = radios_player_workspace_source_context(state->ctx->players, snapshot->source);
    RadioSAudioRunSnapshot run = {0};
    if (!source || !radios_playout_ui_navigation_snapshot(source, &run) ||
        (run.state != RADIOS_AUDIO_RUN_PLAYING && run.state != RADIOS_AUDIO_RUN_OVERLAP && run.state != RADIOS_AUDIO_RUN_MANUAL_WAIT)) return FALSE;
    if (action == 0) return run.index > 0;
    if (action == 4) return run.index + 1 < run.count;
    return action == 5 && radios_playout_ui_can_next_playlist(source);
}

/* "Şema yükle…": a saved weekly schema that joins the main flow only inside
 * its own defined slots; the main programme continues outside them. */
static void override_apply(HeaderNavigation *state, const gchar *id, const gchar *name)
{
    RadioSAutomationScope scope; RadioSPlayerScopeExecutionSnapshot snapshot = {0}; gchar assigned[37]; GError *error = NULL;
    gboolean scoped = active_scope(state, &scope, &snapshot);
    if (!scoped && !schema_scope(state, &scope, assigned)) { notify(state, "Şema yüklemek için önce bir oynatıcı kümesi tanımla."); return; }
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(state->ctx);
    if (!execution || !radios_player_scope_execution_set_schema_override(execution, &scope, id, &error)) {
        notify(state, error ? error->message : "Geçici şema uygulanamadı."); g_clear_error(&error); return;
    }
    g_free(state->override_id); g_free(state->override_name);
    state->override_id = g_strdup(id); state->override_name = id ? g_strdup(name ? name : "") : NULL;
    gchar *message = NULL;
    if (!id) message = g_strdup("Geçici şema kaldırıldı; ana program akışı sürüyor.");
    else if (scoped) message = g_strdup_printf("Geçici şema yüklendi: %s · tanımlı saatlerinde ana programın içine girer.", state->override_name);
    else if (schema_playable(state) && play_schema(state))
        message = g_strdup_printf("Geçici şema yüklendi: %s · ana program akışı başlatılıyor.", state->override_name);
    else message = g_strdup_printf("Geçici şema kaydedildi: %s · ana program akışı başlayınca tanımlı saatlerinde devreye girer.", state->override_name);
    notify(state, message); g_free(message);
    navigation_tick(state);
}
static void override_response(GtkDialog *dialog, gint response, gpointer owner)
{
    HeaderNavigation *state = navigation_get(owner);
    if (state && !state->disposed && (response == GTK_RESPONSE_OK || response == GTK_RESPONSE_REJECT)) {
        GtkComboBox *combo = g_object_get_data(G_OBJECT(dialog), "header-schema-combo");
        const gchar *id = response == GTK_RESPONSE_OK ? gtk_combo_box_get_active_id(combo) : NULL;
        gchar *name = response == GTK_RESPONSE_OK ? gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo)) : NULL;
        if (response == GTK_RESPONSE_OK && (!id || !*id)) notify(state, "Yüklenecek bir şema seç.");
        else override_apply(state, id, name);
        g_free(name);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void override_clicked(GtkButton *button, gpointer data)
{
    (void)button; HeaderNavigation *state = data;
    if (state->disposed) return;
    GtkWidget *owner = gtk_widget_get_parent(state->programme);
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Şema yükle", GTK_WINDOW(state->ctx->parent),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL, "Yükle", GTK_RESPONSE_OK, NULL);
    gtk_widget_set_name(dialog, "header-schema-dialog");
    if (state->override_id) gtk_dialog_add_button(GTK_DIALOG(dialog), "Şemayı kaldır", GTK_RESPONSE_REJECT);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12); gtk_box_set_spacing(GTK_BOX(box), 8);
    GtkWidget *text = gtk_label_new("Seçilen şema, tanımlı olduğu saatlerde ana program akışının içine girer; "
        "o aralık bitince ana program kaldığı yerden sürer. Ana program akışı değişmez.");
    gtk_label_set_line_wrap(GTK_LABEL(text), TRUE); gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(text), 60);
    GtkWidget *combo = gtk_combo_box_text_new(); gtk_widget_set_name(combo, "header-schema-choice");
    for (guint i = 0; state->records && i < json_array_get_length(state->records); ++i) {
        JsonObject *row = json_array_get_object_element(state->records, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), json_object_get_string_member(row, "_id"), json_object_get_string_member(row, "name"));
    }
    if (!state->override_id || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), state->override_id))
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(box), combo, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(dialog), "header-schema-combo", combo);
    g_signal_connect_object(dialog, "response", G_CALLBACK(override_response), owner, 0);
    gtk_widget_show_all(dialog);
}
static void override_refresh(HeaderNavigation *state)
{
    RadioSAutomationScope scope; gchar assigned[37];
    gtk_widget_set_sensitive(state->override_button, radios_data_connected(state->ctx) && schema_scope(state, &scope, assigned));
    gchar *wanted = state->override_id ?
        g_strdup_printf("Şema: %s", state->override_name && *state->override_name ? state->override_name : "yüklü") : g_strdup("Şema yükle…");
    if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(state->override_button)), wanted)) gtk_button_set_label(GTK_BUTTON(state->override_button), wanted);
    g_free(wanted);
}

static gboolean navigation_tick(gpointer data)
{
    HeaderNavigation *state = data;
    if (state->disposed) return G_SOURCE_REMOVE;
    schema_status(state);
    override_refresh(state);
    RadioSAutomationScope scope; RadioSPlayerScopeExecutionSnapshot snapshot = {0};
    gboolean scoped = active_scope(state, &scope, &snapshot);
    for (guint action = 0; action < G_N_ELEMENTS(state->buttons); ++action) {
        gboolean available = scoped ? scope_action_available(state, &snapshot, action) :
            radios_broadcast_navigation_available(state->ctx, action);
        if (action == ACTION_PLAY && !scoped && !available) available = schema_playable(state);
        if (state->starting && action == ACTION_PLAY) available = FALSE;
        gtk_widget_set_sensitive(state->buttons[action], available);
    }
    gtk_widget_set_sensitive(state->programme, !scoped && !state->starting);
    /* Refresh the saved schema list every 15 s (and right after connecting). */
    if (state->ticks++ % 150 == 0 || !state->signature) programmes_list(state);
    return G_SOURCE_CONTINUE;
}

static void navigation_clicked(GtkButton *button, gpointer data)
{
    HeaderNavigation *state = data;
    if (state->disposed) return;
    guint action = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "header-navigation-action"));
    GError *error = NULL;
    RadioSAutomationScope scope; RadioSPlayerScopeExecutionSnapshot snapshot = {0};
    if (active_scope(state, &scope, &snapshot)) {
        gboolean accepted = FALSE;
        if (scope_action_available(state, &snapshot, action)) {
            if (action == 5) {
                RadioSViewContext *source = radios_player_workspace_source_context(state->ctx->players, snapshot.source);
                accepted = source && radios_playout_ui_next_playlist(source, &error);
            } else {
                const RadioSPlayoutAction actions[] = {RADIOS_PLAYOUT_PREVIOUS, RADIOS_PLAYOUT_RESUME,
                    RADIOS_PLAYOUT_PAUSE, RADIOS_PLAYOUT_STOP, RADIOS_PLAYOUT_NEXT};
                accepted = radios_player_scope_execution_action(radios_player_scope_execution_get(state->ctx), &scope, actions[action], &error);
            }
        }
        if (!accepted) notify(state, error ? error->message : "Program akışı için bu komut şu an kullanılamıyor.");
        g_clear_error(&error); navigation_tick(state); return;
    }
    if (action == ACTION_PLAY && !radios_broadcast_navigation_queue_active(state->ctx)) {
        /* A programme picked in "Program akışı" (the assigned one by default)
         * starts with its programme of now; "Program akışı" left empty keeps
         * the selected broadcast queue behaviour. With the broadcast page's
         * continuous general queue the assigned programme is that queue: Play
         * starts it there (one flow, its event list follows the transport);
         * only another programme picked here runs as its own scope flow. */
        const gchar *selected = gtk_combo_box_get_active_id(GTK_COMBO_BOX(state->programme));
        gboolean chosen = selected && *selected;
        RadioSAutomationScope scope; gchar assigned[37] = "";
        gboolean general = radios_broadcast_navigation_general_continuous(state->ctx) &&
            (!chosen || (schema_scope(state, &scope, assigned) && !g_strcmp0(selected, assigned)));
        if (!general && (chosen || !radios_broadcast_navigation_queue_playable(state->ctx)) && play_schema(state)) {
            navigation_tick(state); return;
        }
    }
    if (!radios_broadcast_navigation_action(state->ctx, action, &error) && state->ctx->notify)
        state->ctx->notify(state->ctx->notify_data,
            error ? error->message : "Seçili yayın sırası için bu komut şu an kullanılamıyor.");
    g_clear_error(&error);
    navigation_tick(state);
}

/* U3: "Sonraki playlist ▾". The picker lists the playlists of the flow the
 * header drives, in order with the playing one marked; a pick jumps to that
 * playlist's first song through the same owner and bounds as action 5. */
typedef struct { guint64 epoch; guint index; gchar entry_id[37]; } PlaylistChoice;
static RadioSViewContext *playlist_scope_source(HeaderNavigation *state, gboolean *scoped)
{
    RadioSAutomationScope scope; RadioSPlayerScopeExecutionSnapshot snapshot = {0};
    *scoped = active_scope(state, &scope, &snapshot);
    if (!*scoped || !scope_action_available(state, &snapshot, 5)) return NULL;
    return radios_player_workspace_source_context(state->ctx->players, snapshot.source);
}
static void playlist_chosen(GtkButton *item, gpointer data)
{
    HeaderNavigation *state = data;
    const PlaylistChoice *found = g_object_get_data(G_OBJECT(item), "header-next-playlist-choice");
    if (state->disposed || !found) return;
    PlaylistChoice choice = *found;
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(item), GTK_TYPE_POPOVER);
    if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
    GError *error = NULL; gboolean scoped = FALSE;
    RadioSViewContext *source = playlist_scope_source(state, &scoped);
    gboolean accepted = scoped ?
        source && radios_playout_ui_jump_playlist(source, choice.epoch, choice.index, choice.entry_id, &error) :
        radios_broadcast_navigation_jump_playlist(state->ctx, choice.epoch, choice.index, choice.entry_id, &error);
    if (!accepted) notify(state, error ? error->message : "Program akışı için bu komut şu an kullanılamıyor.");
    g_clear_error(&error); navigation_tick(state);
}
static void playlist_item_destroy(GtkWidget *widget, gpointer data)
{ (void)data; gtk_widget_destroy(widget); }
/* The list is rebuilt from the running flow each time ▾ opens. */
static void playlist_choose_toggled(GtkToggleButton *button, gpointer data)
{
    HeaderNavigation *state = data;
    if (state->disposed || !gtk_toggle_button_get_active(button)) return;
    GtkPopover *popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(button));
    GtkWidget *items = gtk_bin_get_child(GTK_BIN(popover));
    gtk_container_foreach(GTK_CONTAINER(items), playlist_item_destroy, NULL);
    gboolean scoped = FALSE; RadioSViewContext *source = playlist_scope_source(state, &scoped);
    JsonArray *choices = scoped ? (source ? radios_playout_ui_playlist_choices(source) : NULL) :
        radios_broadcast_navigation_playlists(state->ctx);
    if (!choices || json_array_get_length(choices) < 2) {
        if (choices) json_array_unref(choices);
        gtk_popover_popdown(popover);
        notify(state, "Çalan sırada geçilebilecek bir sonraki playlist yok.");
        navigation_tick(state); return;
    }
    for (guint i = 0; i < json_array_get_length(choices); ++i) {
        JsonObject *row = json_array_get_object_element(choices, i);
        gboolean current = json_object_get_boolean_member_with_default(row, "current", FALSE);
        GtkWidget *item = gtk_model_button_new();
        g_object_set(item, "text", radios_json_string(row, "title", ""), "role", GTK_BUTTON_ROLE_RADIO, "active", current, NULL);
        gchar *name = g_strdup_printf("header-next-playlist-choice-%u", i);
        gtk_widget_set_name(item, name); g_free(name);
        const gchar *title = radios_json_string(row, "first_title", ""), *artist = radios_json_string(row, "first_artist", "");
        gchar *tip = *artist ? g_strdup_printf("%s – %s", artist, title) : g_strdup(title);
        if (*tip) gtk_widget_set_tooltip_text(item, tip);
        g_free(tip);
        if (current) gtk_widget_set_sensitive(item, FALSE);
        else {
            PlaylistChoice *choice = g_new0(PlaylistChoice, 1);
            choice->epoch = (guint64)radios_json_int(row, "epoch", 0); choice->index = (guint)radios_json_int(row, "index", 0);
            g_strlcpy(choice->entry_id, radios_json_string(row, "entry_id", ""), sizeof choice->entry_id);
            g_object_set_data_full(G_OBJECT(item), "header-next-playlist-choice", choice, g_free);
            g_signal_connect(item, "clicked", G_CALLBACK(playlist_chosen), state);
        }
        gtk_box_pack_start(GTK_BOX(items), item, FALSE, FALSE, 0);
    }
    json_array_unref(choices); gtk_widget_show_all(items);
}

static void navigation_destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget;
    HeaderNavigation *state = data;
    state->disposed = TRUE;
    if (state->timer) {
        g_source_remove(state->timer);
        state->timer = 0;
    }
}

static void navigation_free(gpointer data)
{
    HeaderNavigation *state = data;
    g_clear_pointer(&state->records, json_array_unref);
    g_free(state->override_id); g_free(state->override_name);
    g_free(state->signature); g_free(state->execution_message); g_free(state);
}

GtkWidget *radios_header_navigation_new(RadioSViewContext *ctx)
{
    HeaderNavigation *state = g_new0(HeaderNavigation, 1);
    state->ctx = ctx;
    GtkWidget *section = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "header-navigation");
    GtkWidget *transport = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "header-transport");
    GtkWidget *flow = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "header-programme-flow");
    /* U3: "Sonraki playlist" and its ▾ picker form one split button. */
    GtkWidget *split = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 0, "linked");
    gtk_widget_set_name(split, "header-next-playlist-split");
    gtk_widget_set_name(section, "header-navigation");
    const gchar *names[] = {
        "header-previous", "header-play", "header-pause", "header-stop",
        "header-next-track", "header-next-playlist"
    };
    const gchar *labels[] = {"⏮", "▶", "Ⅱ", "■", "⏭", "Sonraki playlist"};
    const gchar *descriptions[] = {
        "Önceki şarkı", "Seçili yayın sırasını çal / sürdür", "Duraklat", "Durdur",
        "Sonraki şarkı", "Sonraki playlistin ilk şarkısına geç"
    };
    for (guint action = 0; action < G_N_ELEMENTS(names); ++action) {
        GtkWidget *button = radios_ui_button(labels[action]);
        state->buttons[action] = button;
        gtk_widget_set_name(button, names[action]);
        gtk_widget_set_tooltip_text(button, descriptions[action]);
        atk_object_set_name(gtk_widget_get_accessible(button), descriptions[action]);
        g_object_set_data(G_OBJECT(button), "header-navigation-action", GUINT_TO_POINTER(action));
        g_signal_connect(button, "clicked", G_CALLBACK(navigation_clicked), state);
        gtk_widget_set_sensitive(button, FALSE);
        gtk_box_pack_start(GTK_BOX(action < 5 ? transport : split), button, TRUE, TRUE, 0);
    }
    /* ▾ lists the playlists of the flow the header drives, the playing one
     * marked; it follows "Sonraki playlist", so it is inactive without one. */
    GtkWidget *choose = gtk_menu_button_new(), *picker = gtk_popover_new(choose);
    GtkWidget *picks = radios_ui_box(GTK_ORIENTATION_VERTICAL, 2, NULL);
    gtk_button_set_label(GTK_BUTTON(choose), "▾");
    gtk_style_context_add_class(gtk_widget_get_style_context(choose), "ld-btn");
    gtk_widget_set_name(choose, "header-next-playlist-choose");
    gtk_widget_set_tooltip_text(choose, "Playlist seç");
    atk_object_set_name(gtk_widget_get_accessible(choose), "Playlist seç");
    gtk_widget_set_name(picker, "header-next-playlist-menu");
    gtk_container_set_border_width(GTK_CONTAINER(picks), 6); gtk_container_add(GTK_CONTAINER(picker), picks);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(choose), picker);
    g_object_bind_property(state->buttons[5], "sensitive", choose, "sensitive", G_BINDING_SYNC_CREATE);
    g_signal_connect(choose, "toggled", G_CALLBACK(playlist_choose_toggled), state);
    gtk_box_pack_start(GTK_BOX(split), choose, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(flow), split, TRUE, TRUE, 0);
    /* Program akışı: pick the saved weekly schema; with no playable broadcast
     * queue, Play starts its programme of now (Yayın › Şema assignment). */
    state->programme = gtk_combo_box_text_new();
    gtk_widget_set_name(state->programme, "header-programme");
    gtk_widget_set_tooltip_text(state->programme,
        "Program akışı: seçili yayın sırası çalınabilir değilse ▶ bu haftalık programın o saatteki kuşağını otomatik yükleyip başlatır.");
    atk_object_set_name(gtk_widget_get_accessible(state->programme), "Program akışı");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->programme), "", "Program akışı");
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->programme), 0);
    gtk_box_pack_start(GTK_BOX(flow), state->programme, TRUE, TRUE, 0);
    gtk_box_reorder_child(GTK_BOX(flow), state->programme, 0);
    /* Şema yükle: a temporary schema that joins the main flow inside its slots. */
    state->override_button = radios_ui_button("Şema yükle…");
    gtk_widget_set_name(state->override_button, "header-schema-load");
    gtk_widget_set_tooltip_text(state->override_button,
        "Geçici şema yükle: seçilen şema tanımlı olduğu saatlerde ana program akışının içine girer; o aralık bitince ana program kaldığı yerden sürer.");
    atk_object_set_name(gtk_widget_get_accessible(state->override_button), "Şema yükle");
    g_signal_connect(state->override_button, "clicked", G_CALLBACK(override_clicked), state);
    gtk_widget_set_sensitive(state->override_button, FALSE);
    gtk_box_pack_start(GTK_BOX(flow), state->override_button, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(section), transport, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(section), flow, FALSE, FALSE, 0);
    gtk_widget_set_hexpand(section, TRUE);
    g_object_set_data_full(G_OBJECT(section), "header-navigation-state", state, navigation_free);
    g_object_set_data(G_OBJECT(flow), "header-navigation-state", state);
    g_signal_connect(section, "destroy", G_CALLBACK(navigation_destroyed), state);
    state->timer = g_timeout_add(100, navigation_tick, state);
    navigation_tick(state);
    return section;
}
