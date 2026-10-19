#include "radios/widgets.h"
#include "radios/editors.h"
#include "radios/data.h"
#include "radios/media_ui.h"
#include "radios/audio_ui.h"
#include "radios/broadcast_ui.h"
#include "radios/playout_ui.h"
#include <math.h>
#include <errno.h>
#include <string.h>

typedef struct {
    gchar *values[9], *file;
    gchar *id, *binding[4];
    gint kind;
} EmergencyItem;

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *page, *queue_stack, *settings_stack, *lists[2], *queue_pages[2];
    GtkWidget *fields[9], *kind, *file, *heading, *error, *wave, *duration_total;
    GtkWidget *log_list, *item_dialog, *file_dialog, *takeover_dialog, *export_dialog, *transfer_dialog;
    GtkListBoxRow *editing;
    GString *log;
    GKeyFile *preparation;
    guint marker;
    gboolean dragging, disposed, loading;
    RadioSDocument *document;
    gboolean settings_dirty;
    GtkWidget *runtime_status, *restore, *abort;
    guint runtime_timer;
    guint cue_intent;
    gboolean cue_pending;
    gboolean transfer_pending;
    guint64 transfer_intent;
} EmergencyState;

static const gchar *const binding_keys[] = {"media_id", "version_id", "media_file_id", "media_sha256"};

static const gchar *const kinds[] = { "Müzik", "Anons", "Jingle · kimlik", "Arıza anonsu", "Reklam", NULL };
static void queue_updated(EmergencyState *state);
static void editor_load(EmergencyState *state, EmergencyItem *item, GtkListBoxRow *row);
static void owned_dialog_destroyed(GtkWidget *dialog, gpointer data);
static void update_contact_count(EmergencyState *state);
static void emergency_save(EmergencyState *state);
static void emergency_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data);
static void persist_saved_rows(EmergencyState *state);
static JsonNode *emergency_configuration(EmergencyState *state);
static gchar *format_time(double seconds);
static void preparation_save(GtkButton *button, gpointer data);

static void
item_free(gpointer data)
{
    EmergencyItem *item = data;
    for (guint i = 0; i < G_N_ELEMENTS(item->values); ++i)
        g_free(item->values[i]);
    g_free(item->file);
    g_free(item->id);
    for (guint i = 0; i < G_N_ELEMENTS(item->binding); ++i) g_free(item->binding[i]);
    g_free(item);
}

static gchar *
clean_entry(GtkWidget *entry_widget)
{
    gchar *text = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry_widget)));
    return g_strstrip(text);
}

static gboolean
parse_time(const gchar *text, double *value)
{
    if (text == NULL || *text == '\0') {
        *value = 0;
        return TRUE;
    }
    gchar **parts = g_strsplit(text, ":", -1);
    gboolean valid = g_strv_length(parts) == 2 && *parts[0] != '\0' && *parts[1] != '\0';
    gchar *end = NULL;
    if (valid) {
        for (const gchar *p = parts[0]; *p != '\0'; ++p)
            if (!g_ascii_isdigit(*p))
                valid = FALSE;
        errno = 0;
        double minutes = g_ascii_strtod(parts[0], &end);
        valid = valid && *end == '\0' && errno == 0 && minutes <= 999;
        errno = 0;
        double seconds = g_ascii_strtod(parts[1], &end);
        valid = valid && *end == '\0' && errno == 0 && isfinite(seconds) && seconds >= 0 && seconds < 60;
        for (const gchar *p = parts[1]; *p != '\0'; ++p)
            if (!g_ascii_isdigit(*p) && *p != '.')
                valid = FALSE;
        *value = minutes * 60 + seconds;
    }
    g_strfreev(parts);
    return valid;
}

static void
record_draft(EmergencyState *state, const gchar *message)
{
    GDateTime *now = g_date_time_new_now_utc();
    gchar *time = g_date_time_format(now, "%H:%M:%S UTC");
    gchar *line = g_strdup_printf("%s · Taslak · %s", time, message);
    g_string_append_printf(state->log, "%s\n", line);
    if (state->log_list != NULL) {
        gtk_container_add(GTK_CONTAINER(state->log_list), radios_ui_label(line, "em-log-row"));
        gtk_widget_show_all(state->log_list);
    }
    if (state->ctx->notify != NULL)
        state->ctx->notify(state->ctx->notify_data, state->ctx->data ? "Acil hazırlık taslağı değişti; kalıcı kayıt sonucu durum çubuğunda gösterilir." : "Oturum taslağı güncellendi; DB ve ses motoru henüz bağlı değil.");
    g_free(line);
    g_free(time);
    g_date_time_unref(now);
    persist_saved_rows(state);
}

static void
pack(GtkWidget *box, GtkWidget *widget, gboolean expand)
{
    gtk_box_pack_start(GTK_BOX(box), widget, expand, expand, 0);
}

static GtkWidget *
button_new(RadioSViewContext *ctx, const gchar *name)
{
    GtkWidget *button = radios_ui_button(name);
    radios_ui_bind_notice(button, ctx, "Bu işlem için gerçek ses motoru/acil yayın bağlantısı gerekiyor.");
    return button;
}

static GtkWidget *
field(const gchar *label, GtkWidget *control)
{
    GtkWidget *box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "em-field");
    pack(box, radios_ui_label(label, "em-field-label"), FALSE);
    pack(box, control, FALSE);
    gtk_widget_set_hexpand(box, TRUE);
    g_object_set_data_full(G_OBJECT(box), "em-field-name", g_strdup(label), g_free);
    g_object_set_data(G_OBJECT(box), "em-field-control", control);
    return box;
}

static GtkWidget *
entry(const gchar *placeholder)
{
    return radios_ui_entry(placeholder);
}

static GtkWidget *
numeric(const gchar *placeholder)
{
    GtkWidget *widget = entry(placeholder);
    gtk_entry_set_input_purpose(GTK_ENTRY(widget), GTK_INPUT_PURPOSE_NUMBER);
    return widget;
}

static GtkWidget *
text_area(gint rows)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *text = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_size_request(scroll, -1, rows * 24);
    gtk_container_add(GTK_CONTAINER(scroll), text);
    return scroll;
}

static GtkWidget *
fields_grid(void)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(grid), "em-fields");
    return grid;
}

static void
add_field(GtkWidget *grid, guint index, guint columns, const gchar *name, GtkWidget *control)
{
    gtk_grid_attach(GTK_GRID(grid), field(name, control), index % columns, index / columns, 1, 1);
}

static GtkWidget *
choices(const gchar *a, const gchar *b, const gchar *c, gint active)
{
    const gchar *items[] = { a, b, c, NULL };
    return radios_ui_combo(items, active);
}

static GtkWidget *
outputs_new(gboolean all)
{
    const gchar *names[] = { "Stüdyo", "İnternet", "Karasal" };
    GtkWidget *outputs = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 12, "em-checks");
    pack(outputs, radios_ui_label("Etkilenecek çıkışlar", "em-field-label"), FALSE);
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *check = gtk_check_button_new_with_label(names[i]);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), all || i == 0);
        pack(outputs, check, FALSE);
    }
    return outputs;
}

static GtkWidget *
facts_new(const gchar *const *names, guint count, guint columns)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(grid), "em-facts");
    gtk_grid_set_column_spacing(GTK_GRID(grid), 1);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 1);
    for (guint i = 0; i < count; ++i) {
        GtkWidget *fact = radios_ui_box(GTK_ORIENTATION_VERTICAL, 3, "em-fact");
        pack(fact, radios_ui_label(names[i], "em-field-label"), FALSE);
        pack(fact, radios_ui_label("—", "em-fact-value"), FALSE);
        gtk_widget_set_hexpand(fact, TRUE);
        gtk_grid_attach(GTK_GRID(grid), fact, i % columns, i / columns, 1, 1);
    }
    return grid;
}

static GtkWidget *
sub(const gchar *name)
{
    return radios_ui_label(name, "em-sub");
}

static GtkWidget *
find_button(GtkWidget *root, const gchar *label)
{
    if (GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), label) == 0)
        return root;
    if (!GTK_IS_CONTAINER(root))
        return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *result = NULL;
    for (GList *item = children; item != NULL && result == NULL; item = item->next)
        result = find_button(item->data, label);
    g_list_free(children);
    return result;
}

static GtkWidget *
find_field(GtkWidget *root, const gchar *name)
{
    const gchar *field_name = g_object_get_data(G_OBJECT(root), "em-field-name");
    if (g_strcmp0(field_name, name) == 0)
        return g_object_get_data(G_OBJECT(root), "em-field-control");
    if (!GTK_IS_CONTAINER(root))
        return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *result = NULL;
    for (GList *item = children; item != NULL && result == NULL; item = item->next)
        result = find_field(item->data, name);
    g_list_free(children);
    return result;
}

static void
replace_action(GtkWidget *button, GCallback callback, gpointer data, const gchar *name)
{
    guint signal = g_signal_lookup("clicked", GTK_TYPE_BUTTON);
    g_signal_handlers_disconnect_matched(button, G_SIGNAL_MATCH_ID, signal, 0, NULL, NULL, NULL);
    gtk_widget_set_name(button, name);
    gtk_widget_set_tooltip_text(button, "Hazırlık kaydı; gerçek yayın veya fiziksel yetki işlemi yapmaz.");
    g_signal_connect(button, "clicked", callback, data);
}

static gchar *
control_text(GtkWidget *widget)
{
    if (GTK_IS_ENTRY(widget))
        return clean_entry(widget);
    if (GTK_IS_COMBO_BOX_TEXT(widget))
        return gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widget));
    if (GTK_IS_SCROLLED_WINDOW(widget))
        widget = gtk_bin_get_child(GTK_BIN(widget));
    if (GTK_IS_TEXT_VIEW(widget)) {
        GtkTextIter start, end;
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        return g_strstrip(text);
    }
    return g_strdup("");
}

static void
clear_controls(GtkWidget *root)
{
    if (GTK_IS_ENTRY(root))
        gtk_entry_set_text(GTK_ENTRY(root), "");
    else if (GTK_IS_TEXT_VIEW(root))
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(root)), "", -1);
    else if (GTK_IS_CONTAINER(root)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(root));
        for (GList *item = children; item != NULL; item = item->next)
            clear_controls(item->data);
        g_list_free(children);
    }
}

typedef struct {
    EmergencyState *owner;
    GtkWidget *section, *list, *error;
    const gchar *action;
    const gchar *const *required;
    GtkListBoxRow *editing;
} DraftAction;

typedef struct {
    DraftAction *action;
    GtkWidget *row, *reason;
} DraftCancellation;

static void
snapshot_fields(GtkWidget *root, GHashTable *values)
{
    const gchar *name = g_object_get_data(G_OBJECT(root), "em-field-name");
    GtkWidget *control = g_object_get_data(G_OBJECT(root), "em-field-control");
    if (name != NULL && control != NULL) {
        if (*name != '\0')
            g_hash_table_replace(values, g_strdup(name), control_text(control));
        return;
    }
    if (GTK_IS_TOGGLE_BUTTON(root)) {
        const gchar *label = gtk_button_get_label(GTK_BUTTON(root));
        if (label != NULL)
            g_hash_table_replace(values, g_strdup(label), g_strdup(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(root)) ? "1" : "0"));
    }
    if (GTK_IS_CONTAINER(root)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(root));
        for (GList *item = children; item != NULL; item = item->next)
            snapshot_fields(item->data, values);
        g_list_free(children);
    }
}

static void
restore_fields(GtkWidget *root, GHashTable *values)
{
    const gchar *name = g_object_get_data(G_OBJECT(root), "em-field-name");
    GtkWidget *control = g_object_get_data(G_OBJECT(root), "em-field-control");
    if (name != NULL && control != NULL) {
        const gchar *value = g_hash_table_lookup(values, name);
        if (value == NULL)
            return;
        if (GTK_IS_ENTRY(control))
            gtk_entry_set_text(GTK_ENTRY(control), value);
        else if (GTK_IS_COMBO_BOX_TEXT(control)) {
            GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(control));
            GtkTreeIter iter;
            gboolean found = gtk_tree_model_get_iter_first(model, &iter);
            gint index = 0;
            while (found) {
                gchar *text = NULL;
                gtk_tree_model_get(model, &iter, 0, &text, -1);
                gboolean matches = g_strcmp0(text, value) == 0;
                g_free(text);
                if (matches) {
                    gtk_combo_box_set_active(GTK_COMBO_BOX(control), index);
                    break;
                }
                ++index;
                found = gtk_tree_model_iter_next(model, &iter);
            }
        } else {
            GtkWidget *text = GTK_IS_SCROLLED_WINDOW(control) ? gtk_bin_get_child(GTK_BIN(control)) : control;
            if (GTK_IS_TEXT_VIEW(text))
                gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)), value, -1);
        }
        return;
    }
    if (GTK_IS_TOGGLE_BUTTON(root)) {
        const gchar *label = gtk_button_get_label(GTK_BUTTON(root));
        const gchar *value = label != NULL ? g_hash_table_lookup(values, label) : NULL;
        if (value != NULL)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(root), g_strcmp0(value, "1") == 0);
    }
    if (GTK_IS_CONTAINER(root)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(root));
        for (GList *item = children; item != NULL; item = item->next)
            restore_fields(item->data, values);
        g_list_free(children);
    }
}

static void preparation_changed(GtkWidget *control, gpointer data)
{
    (void)control; EmergencyState *state = data;
    if (state->loading || state->disposed) return;
    state->settings_dirty = TRUE; radios_data_document_dirty(state->document, TRUE);
}

static void preparation_buffer_changed(GtkTextBuffer *buffer, gpointer data)
{
    (void)buffer; GtkWidget *page = data;
    EmergencyState *state = g_object_get_data(G_OBJECT(page), "em-state");
    if (state) preparation_changed(NULL, state);
}

static void track_preparation(GtkWidget *root, EmergencyState *state)
{
    if (GTK_IS_ENTRY(root)) g_signal_connect(root, "changed", G_CALLBACK(preparation_changed), state);
    else if (GTK_IS_COMBO_BOX(root)) g_signal_connect(root, "changed", G_CALLBACK(preparation_changed), state);
    else if (GTK_IS_TOGGLE_BUTTON(root)) g_signal_connect(root, "toggled", G_CALLBACK(preparation_changed), state);
    else if (GTK_IS_TEXT_VIEW(root))
        g_signal_connect_object(gtk_text_view_get_buffer(GTK_TEXT_VIEW(root)), "changed", G_CALLBACK(preparation_buffer_changed),
                                state->page, 0);
    if (GTK_IS_CONTAINER(root)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(root));
        for (GList *child = children; child; child = child->next) track_preparation(child->data, state);
        g_list_free(children);
    }
}

static void
disconnect_tree(GtkWidget *root, gpointer data)
{
    g_signal_handlers_disconnect_by_data(root, data);
    if (GTK_IS_CONTAINER(root)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(root));
        for (GList *item = children; item != NULL; item = item->next)
            disconnect_tree(item->data, data);
        g_list_free(children);
    }
}

static void
draft_action_destroyed(GtkWidget *section, gpointer data)
{
    disconnect_tree(section, data);
}

static void
cancel_row(DraftAction *action, GtkWidget *row, const gchar *reason)
{
    GHashTable *values = g_object_get_data(G_OBJECT(row), "draft-values");
    GtkWidget *label = g_object_get_data(G_OBJECT(row), "draft-text-label");
    gchar *text = g_strdup_printf("İptal · Taslak · %s · %s", reason, gtk_label_get_text(GTK_LABEL(label)));
    gtk_label_set_text(GTK_LABEL(label), text);
    g_free(text);
    g_hash_table_replace(values, g_strdup("Taslak durumu"), g_strdup("İptal"));
    g_hash_table_replace(values, g_strdup("İptal gerekçesi"), g_strdup(reason));
    gtk_widget_set_sensitive(row, FALSE);
    if (action->editing == GTK_LIST_BOX_ROW(row))
        action->editing = NULL;
    record_draft(action->owner, "Yerel taslak kaydı iptal edildi; yayın / USB yetkisi değişmedi.");
}

static void
cancellation_free(gpointer data)
{
    DraftCancellation *cancel = data;
    g_object_unref(cancel->row);
    g_free(cancel);
}

static void
cancellation_destroyed(GtkWidget *dialog, gpointer data)
{
    DraftCancellation *cancel = data;
    if (cancel->action->owner->item_dialog == dialog)
        cancel->action->owner->item_dialog = NULL;
}

static void
cancellation_response(GtkDialog *dialog, gint response, gpointer data)
{
    DraftCancellation *cancel = data;
    if (response == GTK_RESPONSE_ACCEPT && !cancel->action->owner->disposed && gtk_widget_get_parent(cancel->row) != NULL) {
        gchar *reason = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(cancel->reason));
        cancel_row(cancel->action, cancel->row, reason);
        g_free(reason);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
mini_remove(GtkButton *button, gpointer data)
{
    DraftAction *action = data;
    GtkWidget *row = g_object_get_data(G_OBJECT(button), "draft-row");
    if (g_strcmp0(action->action, "Anahtar ekle") == 0) {
        cancel_row(action, row, "Anahtar kaydı iptal edildi");
        return;
    }
    if (g_strcmp0(action->action, "Duyuru taslağı") == 0) {
        EmergencyState *state = action->owner;
        if (state->item_dialog != NULL) {
            gtk_window_present(GTK_WINDOW(state->item_dialog));
            return;
        }
        GtkWidget *dialog = gtk_dialog_new_with_buttons("Duyuru taslağını iptal et", state->ctx->parent,
            GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL, "İptal et", GTK_RESPONSE_ACCEPT, NULL);
        GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
        const gchar *const reasons[] = { "Yanlış alarm", "Süresi doldu", "Kaynak iptal etti", "Yenisi yayımlandı", NULL };
        DraftCancellation *cancel = g_new0(DraftCancellation, 1);
        cancel->action = action;
        cancel->row = g_object_ref(row);
        cancel->reason = radios_ui_combo(reasons, 0);
        state->item_dialog = dialog;
        gtk_widget_set_name(dialog, "em-alert-cancel-dialog");
        gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "ops-dialog");
        gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
        gtk_widget_set_name(cancel->reason, "em-alert-cancel-reason");
        gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
        gtk_container_set_border_width(GTK_CONTAINER(body), 18);
        pack(body, radios_ui_label("İptal gerekçesi", "em-field-label"), FALSE);
        pack(body, cancel->reason, FALSE);
        g_object_set_data_full(G_OBJECT(dialog), "draft-cancellation", cancel, cancellation_free);
        g_signal_connect(dialog, "response", G_CALLBACK(cancellation_response), cancel);
        g_signal_connect(dialog, "destroy", G_CALLBACK(cancellation_destroyed), cancel);
        gtk_widget_show_all(dialog);
        return;
    }
    if (action->editing == GTK_LIST_BOX_ROW(row))
        action->editing = NULL;
    gtk_widget_destroy(row);
    record_draft(action->owner, "Yerel taslak satırı kaldırıldı.");
    update_contact_count(action->owner);
}

static void
mini_edit(GtkButton *button, gpointer data)
{
    DraftAction *action = data;
    GtkWidget *row = g_object_get_data(G_OBJECT(button), "draft-row");
    GHashTable *values = g_object_get_data(G_OBJECT(row), "draft-values");
    restore_fields(action->section, values);
    action->editing = GTK_LIST_BOX_ROW(row);
    gtk_label_set_text(GTK_LABEL(action->error), "Seçili oturum taslağını düzenle; kaydet düğmesi bu satırı günceller.");
}

static void
mini_up(GtkButton *button, gpointer data)
{
    EmergencyState *state = data;
    GtkWidget *row = g_object_get_data(G_OBJECT(button), "draft-row");
    GtkWidget *list = gtk_widget_get_parent(row);
    gint index = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(row));
    if (index > 0) {
        g_object_ref(row);
        gtk_list_box_unselect_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(row));
        gtk_container_remove(GTK_CONTAINER(list), row);
        gtk_list_box_insert(GTK_LIST_BOX(list), row, index - 1);
        g_object_unref(row);
        gtk_widget_show_all(row);
        record_draft(state, "Yerel taslak sırası değiştirildi.");
    }
}

static void
mini_add(DraftAction *action, const gchar *text)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "em-mini-row");
    GtkWidget *up = radios_ui_button("↑");
    GtkWidget *edit = radios_ui_button("Düzenle");
    GtkWidget *remove = radios_ui_button(g_strcmp0(action->action, "Anahtar ekle") == 0 || g_strcmp0(action->action, "Duyuru taslağı") == 0 ? "İptal et" : "×");
    GtkWidget *label = radios_ui_label(text, "em-mini-row-text");
    pack(box, label, TRUE);
    g_object_set_data(G_OBJECT(row), "draft-text-label", label);
    g_object_set_data_full(G_OBJECT(row), "draft-action-name", g_strdup(action->action), g_free);
    if (g_strcmp0(action->action, "İletişim ekle") == 0) {
        g_object_set_data(G_OBJECT(up), "draft-row", row);
        g_signal_connect(up, "clicked", G_CALLBACK(mini_up), action->owner);
        pack(box, up, FALSE);
    } else
        gtk_widget_destroy(up);
    g_object_set_data(G_OBJECT(remove), "draft-row", row);
    g_signal_connect(remove, "clicked", G_CALLBACK(mini_remove), action);
    g_object_set_data(G_OBJECT(edit), "draft-row", row);
    g_signal_connect(edit, "clicked", G_CALLBACK(mini_edit), action);
    pack(box, edit, FALSE);
    pack(box, remove, FALSE);
    gtk_container_add(GTK_CONTAINER(row), box);
    GHashTable *values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    snapshot_fields(action->section, values);
    g_object_set_data_full(G_OBJECT(row), "draft-values", values, (GDestroyNotify)g_hash_table_unref);
    if (action->editing != NULL) {
        gtk_widget_destroy(GTK_WIDGET(action->editing));
        action->editing = NULL;
    }
    gtk_container_add(GTK_CONTAINER(action->list), row);
    gtk_widget_show_all(row);
    update_contact_count(action->owner);
}

static gboolean
date_valid(const gchar *text)
{
    gchar *iso = g_strdup_printf("%s:00Z", text);
    if (strlen(iso) > 10 && iso[10] == ' ')
        iso[10] = 'T';
    GDateTime *date = g_date_time_new_from_iso8601(iso, NULL);
    g_free(iso);
    if (date != NULL)
        g_date_time_unref(date);
    return date != NULL;
}

static void
draft_save(GtkButton *button, gpointer data)
{
    DraftAction *action = data;
    GString *summary = g_string_new(action->action);
    gboolean valid = TRUE;
    for (guint i = 0; action->required[i] != NULL; ++i) {
        GtkWidget *input = find_field(action->section, action->required[i]);
        gchar *text = input != NULL ? control_text(input) : g_strdup("");
        if (*text == '\0') {
            gchar *message = g_strdup_printf("%s gerekli.", action->required[i]);
            gtk_label_set_text(GTK_LABEL(action->error), message);
            g_free(message);
            if (input != NULL)
                gtk_widget_grab_focus(input);
            valid = FALSE;
        } else if ((g_strcmp0(action->required[i], "Başlangıç") == 0 || g_strcmp0(action->required[i], "Bitiş") == 0 ||
                    g_strcmp0(action->required[i], "Tarih") == 0) && !date_valid(text)) {
            gtk_label_set_text(GTK_LABEL(action->error), "Tarih biçimi YYYY-AA-GG SS:DD olmalı; geçerli bir UTC tarih gir.");
            valid = FALSE;
        } else if (strstr(action->required[i], "· sn") != NULL || strstr(action->required[i], "· ms") != NULL ||
                   g_strcmp0(action->required[i], "Dakika") == 0 || g_strcmp0(action->required[i], "Tekrar · dk") == 0) {
            gchar *end = NULL;
            double number = g_ascii_strtod(text, &end);
            gboolean quiet = g_strcmp0(action->required[i], "Dakika") == 0;
            if (*end != '\0' || !isfinite(number) || number < (quiet ? 1 : 0) || number > (quiet ? 240 : 1000000) || (quiet && floor(number) != number)) {
                gtk_label_set_text(GTK_LABEL(action->error), quiet ? "Planlı sessizlik 1–240 dakika arasında bir tam sayı olmalı." : "Süre alanlarına 0–1000000 aralığında sayı gir.");
                valid = FALSE;
            }
        }
        if (valid)
            g_string_append_printf(summary, " · %s: %s", action->required[i], text);
        g_free(text);
        if (!valid)
            break;
    }
    if (valid && g_strcmp0(action->action, "Duyuru taslağı") == 0) {
        gchar *begin = control_text(find_field(action->section, "Başlangıç"));
        gchar *end = control_text(find_field(action->section, "Bitiş"));
        if (g_strcmp0(end, begin) <= 0) {
            gtk_label_set_text(GTK_LABEL(action->error), "Duyuru bitişi başlangıçtan sonra olmalı.");
            valid = FALSE;
        }
        g_free(begin);
        g_free(end);
        gchar *speech = control_text(find_field(action->section, "Okunacak metin · TTS"));
        gchar *media = control_text(find_field(action->section, "Ses dosyası / medya kimliği"));
        if (*speech == '\0' && *media == '\0') {
            gtk_label_set_text(GTK_LABEL(action->error), "Okunacak metin veya ses dosyası / medya kimliği gir.");
            valid = FALSE;
        }
        GHashTable *values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        snapshot_fields(action->section, values);
        if (g_strcmp0(g_hash_table_lookup(values, "Stüdyo"), "1") != 0 &&
            g_strcmp0(g_hash_table_lookup(values, "İnternet"), "1") != 0 &&
            g_strcmp0(g_hash_table_lookup(values, "Karasal"), "1") != 0) {
            gtk_label_set_text(GTK_LABEL(action->error), "En az bir hedef çıkış seç.");
            valid = FALSE;
        }
        gchar *repeat = control_text(find_field(action->section, "Tekrar · dk"));
        gchar *end_ptr = NULL;
        double minutes = *repeat != '\0' ? g_ascii_strtod(repeat, &end_ptr) : 0;
        if ((*repeat != '\0' && *end_ptr != '\0') || !isfinite(minutes) || minutes < 0 || minutes > 1440 || floor(minutes) != minutes) {
            gtk_label_set_text(GTK_LABEL(action->error), "Tekrar süresi 0–1440 dakika arasında tam sayı olmalı.");
            valid = FALSE;
        }
        g_free(repeat);
        gchar *codes = control_text(find_field(action->section, "Bölge kodları"));
        gchar **parts = g_strsplit_set(codes, " ,;\t\n", -1);
        GRegex *pattern = g_regex_new("^[A-Za-z0-9_.:-]{1,100}$", 0, 0, NULL);
        for (guint i = 0; parts[i] != NULL; ++i)
            if (*parts[i] != '\0' && !g_regex_match(pattern, parts[i], 0, NULL)) {
                gtk_label_set_text(GTK_LABEL(action->error), "Bölge kodlarında yalnız harf, sayı, _, ., :, - kullan; her kod en çok 100 karakter olmalı.");
                valid = FALSE;
                break;
            }
        g_regex_unref(pattern);
        g_strfreev(parts);
        g_free(codes);
        gchar *language = control_text(find_field(action->section, "Metin dili"));
        GRegex *language_pattern = g_regex_new("^[A-Za-z]{2,3}(?:-[A-Za-z0-9]{2,8}){0,3}$", 0, 0, NULL);
        if (!g_regex_match(language_pattern, language, 0, NULL)) {
            gtk_label_set_text(GTK_LABEL(action->error), "Metin dili tr-TR veya en-US gibi geçerli bir dil etiketi olmalı.");
            valid = FALSE;
        }
        g_regex_unref(language_pattern);
        g_free(language);
        g_hash_table_unref(values);
        g_free(speech);
        g_free(media);
    }
    if (valid) {
        mini_add(action, summary->str);
        record_draft(action->owner, action->action);
        gtk_label_set_text(GTK_LABEL(action->error), action->owner->ctx->data ?
            "Hazırlık satırı eklendi; veritabanı sonucu durum çubuğundadır. Yayın / USB doğrulaması yapılmadı." :
            "Oturum taslağı eklendi. Yayın / USB doğrulaması yapılmadı.");
        g_object_set_data_full(G_OBJECT(button), "last-draft", g_strdup(summary->str), g_free);
    }
    g_string_free(summary, TRUE);
}

static void
draft_new(GtkButton *button, gpointer data)
{
    DraftAction *action = data;
    (void)button;
    clear_controls(action->section);
    action->editing = NULL;
    gtk_label_set_text(GTK_LABEL(action->error), "Yeni duyuru taslağı. Yayına gönderim henüz bağlı değil.");
    GtkWidget *language = find_field(action->section, "Metin dili");
    if (language != NULL)
        gtk_entry_set_text(GTK_ENTRY(language), "tr-TR");
}

static DraftAction *
bind_draft(EmergencyState *state, GtkWidget *section, const gchar *button_label,
           const gchar *action_name, const gchar *const *required, const gchar *widget_name)
{
    GtkWidget *button = find_button(section, button_label);
    if (button == NULL)
        return NULL;
    DraftAction *action = g_new0(DraftAction, 1);
    action->owner = state;
    action->section = section;
    action->action = action_name;
    action->required = required;
    action->list = gtk_list_box_new();
    action->error = radios_ui_label("", "em-form-status");
    gtk_style_context_add_class(gtk_widget_get_style_context(action->list), "em-mini-list");
    pack(section, action->error, FALSE);
    pack(section, action->list, FALSE);
    g_object_set_data_full(G_OBJECT(button), "em-draft-action", action, g_free);
    g_object_set_data(G_OBJECT(button), "em-draft-list", action->list);
    g_signal_connect(section, "destroy", G_CALLBACK(draft_action_destroyed), action);
    replace_action(button, G_CALLBACK(draft_save), action, widget_name);
    return action;
}

static GtkWidget *
takeover_new(void)
{
    GtkWidget *page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "em-panel");
    GtkWidget *grid = fields_grid();
    const gchar *ownership[] = { "Aktif / yedek motor", "Sahip · epoch", "Yedekleme durumu", "Son yoklama", "Hata / eşik", "Bekleme bitişi", "Operatör gerekli", "Son hata" };
    const gchar *adverts[] = { "Kaçan kuşak", "Çift çalma şüphesi", "Telafi bekleyen" };
    pack(page, outputs_new(FALSE), FALSE);
    add_field(grid, 0, 2, "Devralacak oynatıcı", choices("Oynatıcı seç", NULL, NULL, 0));
    add_field(grid, 1, 2, "Devralma davranışı", choices("Acil listeyi başlat", "Sıradaki olaya geç", "Çalan öğeyi sürdür", 0));
    add_field(grid, 2, 2, "Acil liste adı", entry(NULL));
    GtkWidget *repeat = choices("Operatörü bekle", "Listeyi tekrarla", NULL, 0);
    gtk_widget_set_name(repeat, "emergency-repeat-policy");
    gtk_widget_set_tooltip_text(repeat, "Listeyi tekrarla: acil sıra, Normal yayına dön komutuna kadar yeniden çalar.");
    add_field(grid, 3, 2, "Liste bitince", repeat);
    add_field(grid, 4, 2, "Normale dönüş", choices("Operatör kontrollü", "Öğe sonunda hazırla", NULL, 0));
    add_field(grid, 5, 2, "Yedek zincir anahtarı · GPO", entry(NULL));
    pack(page, grid, FALSE);
    pack(page, sub("Sahiplik ve yalıtım"), FALSE);
    pack(page, facts_new(ownership, G_N_ELEMENTS(ownership), 2), FALSE);
    pack(page, sub("Reklam etkisi"), FALSE);
    pack(page, facts_new(adverts, G_N_ELEMENTS(adverts), 3), FALSE);
    return page;
}

static void
update_contact_count(EmergencyState *state)
{
    GtkWidget *handover = gtk_stack_get_child_by_name(GTK_STACK(state->settings_stack), "handover");
    GtkWidget *silence = gtk_stack_get_child_by_name(GTK_STACK(state->settings_stack), "silence");
    if (handover == NULL || silence == NULL)
        return;
    GtkWidget *button = find_button(handover, "Ekle");
    GtkWidget *list = button != NULL ? g_object_get_data(G_OBJECT(button), "em-draft-list") : NULL;
    GtkWidget *label = g_object_get_data(G_OBJECT(silence), "em-contact-count");
    if (list != NULL && label != NULL) {
        GList *rows = gtk_container_get_children(GTK_CONTAINER(list));
        gchar *text = g_strdup_printf("3 · İletişim zinciri · %u kişi · Taslak", g_list_length(rows));
        gtk_label_set_text(GTK_LABEL(label), text);
        g_free(text);
        g_list_free(rows);
    }
}

static GtkWidget *
silence_new(RadioSViewContext *ctx)
{
    GtkWidget *page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "em-panel");
    GtkWidget *grid = fields_grid();
    GtkWidget *ladder = fields_grid();
    const gchar *monitors[] = { "Program çıkışı", "İnternet dönüşü", "Karasal dönüş", "Stüdyo dönüşü", NULL };
    GtkWidget *enable = gtk_check_button_new_with_label("Sessizlikte otomatik devralma");
    const gchar *observations[] = { "Son gözlem", "Çıkış", "Ölçüm zamanı", "Süre" };
    pack(page, enable, FALSE);
    add_field(grid, 0, 3, "İzlenen nokta", radios_ui_combo(monitors, 0));
    add_field(grid, 1, 3, "Eşik · dB", numeric(NULL));
    add_field(grid, 2, 3, "Süre · sn", numeric(NULL));
    pack(page, grid, FALSE);
    add_field(ladder, 0, 3, "1 · Süre dolunca", choices("İkinci deck · yedek oynatıcı", "Acil liste", "Yalnız alarm", 0));
    add_field(ladder, 1, 3, "2 · Sürerse · sn", numeric(NULL));
    add_field(ladder, 2, 3, "Sonra", choices("Acil liste", "Yalnız alarm", "Ek işlem yok", 0));
    pack(page, ladder, FALSE);
    GtkWidget *contact_count = radios_ui_label("3 · İletişim zinciri · 0 kişi", "em-field-label");
    g_object_set_data(G_OBJECT(page), "em-contact-count", contact_count);
    pack(page, contact_count, FALSE);
    GtkWidget *level_meter = radios_meter_new(300, 34, FALSE); gtk_widget_set_name(level_meter, "em-silence-meter"); pack(page, level_meter, FALSE);
    GtkWidget *level_label = radios_ui_label("— dB", "em-fact-value"); gtk_widget_set_name(level_label, "em-silence-level"); pack(page, level_label, FALSE);
    pack(page, facts_new(observations, G_N_ELEMENTS(observations), 4), FALSE);
    grid = fields_grid();
    add_field(grid, 0, 2, "Ses dönünce eşik · dB", numeric(NULL));
    add_field(grid, 1, 2, "Dönüş süresi · sn", numeric(NULL));
    add_field(grid, 2, 2, "Sessiz sayılması", choices("İki kanal da", "Herhangi bir kanal", NULL, 0));
    add_field(grid, 3, 2, "Ölçüm", choices("RMS", "Tepe", NULL, 0));
    add_field(grid, 4, 2, "Harici dedektör · GPI", entry(NULL));
    GtkWidget *test = button_new(ctx, "Çıkış provası");
    gtk_widget_set_sensitive(test, FALSE);
    add_field(grid, 5, 2, "", test);
    pack(page, grid, FALSE);
    pack(page, sub("Planlı sessizlik"), FALSE);
    grid = fields_grid();
    add_field(grid, 0, 3, "Başlangıç", entry("YYYY-AA-GG SS:DD"));
    add_field(grid, 1, 3, "Dakika", numeric(NULL));
    add_field(grid, 2, 3, "Gerekçe", entry(NULL));
    pack(page, grid, FALSE);
    pack(page, button_new(ctx, "Ekle"), FALSE);
    return page;
}

static GtkWidget *
alert_new(RadioSViewContext *ctx)
{
    GtkWidget *page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "em-panel");
    GtkWidget *purpose = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 12, "em-checks");
    GtkWidget *test = gtk_radio_button_new_with_label(NULL, "Test");
    GtkWidget *actual = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(test), "Gerçek");
    GtkWidget *grid = fields_grid();
    GtkWidget *buttons = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "em-row");
    pack(purpose, radios_ui_label("Amaç", "em-field-label"), FALSE);
    pack(purpose, test, FALSE); pack(purpose, actual, FALSE);
    pack(page, purpose, FALSE);
    add_field(grid, 0, 2, "Başlangıç", entry("YYYY-AA-GG SS:DD"));
    add_field(grid, 1, 2, "Bitiş", entry("YYYY-AA-GG SS:DD"));
    add_field(grid, 2, 2, "Bölge kodları", entry("TR-34, TR-06"));
    add_field(grid, 3, 2, "Tekrar · dk", numeric("0"));
    pack(page, grid, FALSE);
    pack(page, outputs_new(TRUE), FALSE);
    pack(page, field("Okunacak metin · TTS", text_area(3)), FALSE);
    grid = fields_grid();
    GtkWidget *language = entry(NULL);
    gtk_entry_set_text(GTK_ENTRY(language), "tr-TR");
    add_field(grid, 0, 2, "Metin dili", language);
    add_field(grid, 1, 2, "Ses dosyası / medya kimliği", entry(NULL));
    pack(page, grid, FALSE);
    pack(buttons, button_new(ctx, "Taslağı kaydet"), FALSE);
    pack(buttons, button_new(ctx, "Yeni"), FALSE);
    GtkWidget *send = button_new(ctx, "Yayına gönder");
    gtk_widget_set_sensitive(send, FALSE);
    pack(buttons, send, FALSE);
    pack(page, buttons, FALSE);
    pack(page, sub("Haber flaşı · zorunlu kesme"), FALSE);
    grid = fields_grid();
    add_field(grid, 0, 3, "Kesme", choices("Kısa fade", "Anında", "Öğe bitince", 0));
    add_field(grid, 1, 3, "Fade · ms", numeric(NULL));
    add_field(grid, 2, 3, "Sonra", choices("Akışa dön", "Operatörü bekle", NULL, 0));
    pack(page, grid, FALSE);
    return page;
}

static GtkWidget *
access_new(RadioSViewContext *ctx)
{
    GtkWidget *page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "em-panel");
    GtkWidget *grid = fields_grid();
    add_field(grid, 0, 2, "Kilitli mod", choices("Onaylı pencere", "Parola", "USB anahtarı", 0));
    add_field(grid, 1, 2, "Devralma yetkisi", choices("Vardiya sorumlusu ve üstü", "Yalnız teknik yönetici", NULL, 0));
    add_field(grid, 2, 2, "Ayar yetkisi", choices("Teknik yönetici", "Vardiya sorumlusu ve üstü", NULL, 0));
    add_field(grid, 3, 2, "Bu oturum", radios_ui_label("—", "em-fact-value"));
    pack(page, grid, FALSE);
    pack(page, sub("USB anahtarları"), FALSE);
    grid = fields_grid();
    add_field(grid, 0, 2, "Etiket", entry(NULL));
    add_field(grid, 1, 2, "Sahibi", entry(NULL));
    pack(page, grid, FALSE);
    pack(page, button_new(ctx, "Anahtar ekle"), FALSE);
    pack(page, sub("Kilit açılamazsa"), FALSE);
    grid = fields_grid();
    add_field(grid, 0, 2, "İkinci yetkili", entry(NULL));
    add_field(grid, 1, 2, "Mühürlü kod yeri", entry(NULL));
    pack(page, grid, FALSE);
    return page;
}

static GtkWidget *
handover_new(RadioSViewContext *ctx)
{
    GtkWidget *page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "em-panel");
    GtkWidget *grid = fields_grid();
    add_field(grid, 0, 2, "Devreden", entry(NULL));
    add_field(grid, 1, 2, "Devralan", entry(NULL));
    pack(page, grid, FALSE);
    grid = fields_grid();
    add_field(grid, 0, 3, "Son durum", text_area(2));
    add_field(grid, 1, 3, "Yapılan işlem", text_area(2));
    add_field(grid, 2, 3, "Kalan risk", text_area(2));
    pack(page, grid, FALSE);
    pack(page, button_new(ctx, "Devri kaydet"), FALSE);
    pack(page, sub("İletişim zinciri"), FALSE);
    grid = fields_grid();
    add_field(grid, 0, 3, "Ad", entry(NULL));
    add_field(grid, 1, 3, "Görev", entry(NULL));
    add_field(grid, 2, 3, "Telefon", entry(NULL));
    pack(page, grid, FALSE);
    GtkWidget *contacts = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 12, "em-row");
    pack(contacts, gtk_check_button_new_with_label("Mesai dışı"), FALSE);
    pack(contacts, button_new(ctx, "Ekle"), FALSE);
    pack(page, contacts, FALSE);
    pack(page, sub("Tatbikat"), FALSE);
    const gchar *last[] = { "Son tatbikat", "Senaryo", "Sonuç", "Toplam" };
    pack(page, facts_new(last, G_N_ELEMENTS(last), 4), FALSE);
    const gchar *scenarios[] = { "Sessizlik", "Motor arızası", "Veritabanı kaybı", "Ağ kaybı", "Ses çıkışı arızası", "Elektrik kesintisi", NULL };
    grid = fields_grid();
    add_field(grid, 0, 4, "Tarih", entry("YYYY-AA-GG SS:DD"));
    add_field(grid, 1, 4, "Senaryo", radios_ui_combo(scenarios, 0));
    add_field(grid, 2, 4, "Çift ses", choices("Yok", "Var", NULL, 0));
    add_field(grid, 3, 4, "Sonuç", choices("Başarılı", "Kısmi", "Başarısız", 0));
    const gchar *times[] = { "Algılama · sn", "Karar · sn", "Devralma · sn", "Toparlanma · sn", "Boşluk · ms" };
    for (guint i = 0; i < G_N_ELEMENTS(times); ++i)
        add_field(grid, i + 4, 4, times[i], numeric(NULL));
    add_field(grid, 9, 4, "Not", entry(NULL));
    pack(page, grid, FALSE);
    pack(page, button_new(ctx, "Tatbikatı kaydet"), FALSE);
    pack(page, sub("Kurtarma hedefleri"), FALSE);
    const gchar *targets[] = { "Oynatma RTO · sn", "Yayın günlüğü RPO · dk", "Katalog RPO · dk", "Ses arşivi RPO · sa" };
    grid = fields_grid();
    for (guint i = 0; i < G_N_ELEMENTS(targets); ++i)
        add_field(grid, i, 4, targets[i], numeric(NULL));
    pack(page, grid, FALSE);
    pack(page, sub("Lisans"), FALSE);
    grid = fields_grid();
    add_field(grid, 0, 2, "Yedek makinede", radios_ui_label("—", "em-fact-value"));
    add_field(grid, 1, 2, "Taşıma / destek dışı saat", text_area(2));
    pack(page, grid, FALSE);
    return page;
}

static void
row_render(GtkListBoxRow *row)
{
    EmergencyItem *item = g_object_get_data(G_OBJECT(row), "em-item");
    GtkWidget *label = g_object_get_data(G_OBJECT(row), "em-item-label");
    gchar *text = g_strdup_printf("%s\n%s%s%s · %s · %s · %s", item->values[0],
        item->values[1], *item->values[1] != '\0' && *item->values[2] != '\0' ? " / " : "",
        item->values[2], kinds[CLAMP(item->kind, 0, 4)], *item->values[3] != '\0' ? item->values[3] : "Süre yok",
        item->binding[0] ? "Doğrulanmış medya bağlı" : "Taslak · medya seçilmedi");
    gtk_label_set_text(GTK_LABEL(label), text);
    g_free(text);
}

static GtkListBoxRow *
queue_append(EmergencyState *state, guint index, EmergencyItem *item)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *label = radios_ui_label("", "em-track-title");
    gtk_container_set_border_width(GTK_CONTAINER(row), 8);
    gtk_container_add(GTK_CONTAINER(row), label);
    g_object_set_data_full(G_OBJECT(row), "em-item", item, item_free);
    g_object_set_data(G_OBJECT(row), "em-item-label", label);
    row_render(GTK_LIST_BOX_ROW(row));
    gtk_container_add(GTK_CONTAINER(state->lists[index]), row);
    gtk_widget_show_all(row);
    queue_updated(state);
    return GTK_LIST_BOX_ROW(row);
}

static void
queue_updated(EmergencyState *state)
{
    double total = 0;
    guint unknown = 0;
    for (guint i = 0; i < 2; ++i) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(state->lists[i]));
        gchar *title = g_strdup_printf("%s  %u", i == 0 ? "Acil liste" : "Geçici sıra", g_list_length(children));
        gtk_container_child_set(GTK_CONTAINER(state->queue_stack), state->queue_pages[i], "title", title, NULL);
        g_free(title);
        if (i == 0) {
            for (GList *node = children; node != NULL; node = node->next) {
                EmergencyItem *item = g_object_get_data(G_OBJECT(node->data), "em-item");
                double duration = 0;
                if (*item->values[3] == '\0' || !parse_time(item->values[3], &duration))
                    ++unknown;
                else
                    total += duration;
            }
        }
        g_list_free(children);
    }
    if (state->duration_total != NULL) {
        gchar *text = g_strdup_printf("%02u:%02u%s", (guint)total / 60, (guint)total % 60, unknown != 0 ? " + bilinmeyen" : "");
        gtk_label_set_text(GTK_LABEL(state->duration_total), text);
        g_free(text);
    }
    if (!state->loading) emergency_save(state);
}

static void
queue_selected(GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
    EmergencyState *state = data;
    (void)list;
    if (!state->disposed && row != NULL)
        editor_load(state, g_object_get_data(G_OBJECT(row), "em-item"), row);
}

static guint
active_queue(EmergencyState *state)
{
    return g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(state->queue_stack)), "queue") == 0 ? 1 : 0;
}

static void
queue_action(GtkButton *button, gpointer data)
{
    EmergencyState *state = data;
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "queue-index"));
    const gchar *action = g_object_get_data(G_OBJECT(button), "queue-action");
    GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->lists[index]));
    if (g_strcmp0(action, "new") == 0) {
        editor_load(state, NULL, NULL);
        gtk_widget_grab_focus(state->fields[0]);
        gtk_label_set_text(GTK_LABEL(state->error), "Yeni öğenin alanlarını doldur; Sıraya ekle ile bu oturumun taslağına ekle.");
        return;
    }
    if (g_strcmp0(action, "copy") == 0) {
        GtkListBoxRow *other = gtk_list_box_get_selected_row(GTK_LIST_BOX(state->lists[1 - index]));
        EmergencyItem *source = other != NULL ? g_object_get_data(G_OBJECT(other), "em-item") : NULL;
        if (source == NULL) {
            gtk_label_set_text(GTK_LABEL(state->error), "Diğer listeden bir taslak öğesi seç; katalog bağlantısı henüz yok.");
            return;
        }
        EmergencyItem *copy = g_new0(EmergencyItem, 1);
        for (guint i = 0; i < G_N_ELEMENTS(copy->values); ++i)
            copy->values[i] = g_strdup(source->values[i]);
        copy->kind = source->kind;
        copy->file = g_strdup(source->file);
        copy->id = source->id ? g_uuid_string_random() : NULL;
        for (guint i = 0; i < G_N_ELEMENTS(copy->binding); ++i) copy->binding[i] = g_strdup(source->binding[i]);
        GtkListBoxRow *row = queue_append(state, index, copy);
        gtk_list_box_select_row(GTK_LIST_BOX(state->lists[index]), row);
        record_draft(state, "Seçili öğe liste taslağına kopyalandı.");
        return;
    }
    if (selected == NULL) {
        gtk_label_set_text(GTK_LABEL(state->error), "Önce bir sıra öğesi seç.");
        return;
    }
    if (g_strcmp0(action, "edit") == 0) {
        editor_load(state, g_object_get_data(G_OBJECT(selected), "em-item"), selected);
        gtk_widget_grab_focus(state->fields[0]);
    } else if (g_strcmp0(action, "remove") == 0) {
        if (state->editing == selected)
            editor_load(state, NULL, NULL);
        gtk_widget_destroy(GTK_WIDGET(selected));
        queue_updated(state);
        record_draft(state, "Sıra taslağından öğe kaldırıldı.");
    } else {
        gint position = gtk_list_box_row_get_index(selected);
        gint next = position + (g_strcmp0(action, "up") == 0 ? -1 : 1);
        GList *children = gtk_container_get_children(GTK_CONTAINER(state->lists[index]));
        gint count = (gint)g_list_length(children);
        g_list_free(children);
        if (next < 0 || next >= count)
            return;
        g_object_ref(selected);
        /* GTK keeps a removed row's selected flag. Clear it before reparenting
         * so selecting the reinserted row also restores list->selected_row. */
        gtk_list_box_unselect_row(GTK_LIST_BOX(state->lists[index]), selected);
        gtk_container_remove(GTK_CONTAINER(state->lists[index]), GTK_WIDGET(selected));
        gtk_list_box_insert(GTK_LIST_BOX(state->lists[index]), GTK_WIDGET(selected), next);
        g_object_unref(selected);
        gtk_list_box_select_row(GTK_LIST_BOX(state->lists[index]), selected);
        record_draft(state, "Sıra taslağındaki öğe taşındı.");
    }
}

static GtkWidget *
queue_button(EmergencyState *state, guint index, const gchar *label, const gchar *action)
{
    GtkWidget *button = radios_ui_button(label);
    gchar *name = g_strdup_printf("em-%s-%s", index == 0 ? "list" : "queue", action);
    gtk_widget_set_name(button, name);
    g_free(name);
    g_object_set_data(G_OBJECT(button), "queue-index", GUINT_TO_POINTER(index));
    g_object_set_data(G_OBJECT(button), "queue-action", (gpointer)action);
    g_signal_connect(button, "clicked", G_CALLBACK(queue_action), state);
    gtk_widget_set_tooltip_text(button, "Yerel liste taslağı; kalıcı kayıt ve yayın motoru henüz bağlı değil.");
    return button;
}

static GtkWidget *
queue_new(EmergencyState *state, gboolean temporary)
{
    static const gchar *const targets[] = { "30 dk", "1 sa", "2 sa", "4 sa", "8 sa", NULL };
    GtkWidget *page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 10, "em-left-panel");
    GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "em-source-row");
    GtkWidget *list = gtk_list_box_new();
    GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "em-row");
    GtkWidget *metrics = fields_grid();
    guint index = temporary ? 1 : 0;
    state->lists[index] = list;
    state->queue_pages[index] = page;
    if (temporary) {
        pack(row, queue_button(state, index, "+ Seçili öğe", "copy"), FALSE);
        pack(row, queue_button(state, index, "+ Yeni öğe", "new"), FALSE);
    } else {
        pack(row, choices("Program / playlist", NULL, NULL, 0), TRUE);
        GtkWidget *add = button_new(state->ctx, "Ekle");
        gtk_widget_set_sensitive(add, FALSE);
        pack(row, add, FALSE);
        pack(row, queue_button(state, index, "+ Seçili parça", "copy"), FALSE);
    }
    pack(page, row, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(list), "em-track-list");
    gtk_widget_set_size_request(list, -1, 170);
    pack(page, list, TRUE);
    gtk_widget_set_name(list, temporary ? "em-queue-items" : "em-list-items");
    g_signal_connect(list, "row-selected", G_CALLBACK(queue_selected), state);
    pack(actions, queue_button(state, index, "↑", "up"), FALSE);
    pack(actions, queue_button(state, index, "↓", "down"), FALSE);
    pack(actions, queue_button(state, index, temporary ? "Düzenle" : "+ Yeni öğe", temporary ? "edit" : "new"), FALSE);
    pack(actions, queue_button(state, index, "Kaldır", "remove"), FALSE);
    pack(page, actions, FALSE);
    if (temporary)
        pack(page, button_new(state->ctx, "Program taslağına aktar"), FALSE);
    else {
        state->duration_total = radios_ui_label("—", "em-fact-value");
        add_field(metrics, 0, 4, "Süre", state->duration_total);
        add_field(metrics, 1, 4, "Şimdi devralınırsa", radios_ui_label("—", "em-fact-value"));
        add_field(metrics, 2, 4, "Doğrulanan dosya", radios_ui_label("—", "em-fact-value"));
        add_field(metrics, 3, 4, "Hedef", radios_ui_combo(targets, 2));
        pack(page, metrics, FALSE);
    }
    return page;
}

static void
editor_load(EmergencyState *state, EmergencyItem *item, GtkListBoxRow *row)
{
    ++state->cue_intent; state->cue_pending = FALSE;
    radios_audio_ui_preview_stop(state->ctx, state->page);
    state->editing = row;
    for (guint i = 0; i < G_N_ELEMENTS(state->fields); ++i)
        gtk_entry_set_text(GTK_ENTRY(state->fields[i]), item != NULL ? item->values[i] : "");
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->kind), item != NULL ? item->kind : 0);
    g_object_set_data_full(G_OBJECT(state->file), "selected-file", item != NULL ? g_strdup(item->file) : NULL, g_free);
    JsonObject *binding = NULL;
    if (item && item->binding[0]) {
        binding = json_object_new();
        for (guint i = 0; i < G_N_ELEMENTS(binding_keys); ++i)
            json_object_set_string_member(binding, binding_keys[i], item->binding[i]);
    }
    g_object_set_data_full(G_OBJECT(state->file), "em-media-binding", binding, (GDestroyNotify)json_object_unref);
    gtk_widget_set_sensitive(state->fields[3], binding == NULL);
    gchar *basename = item != NULL && item->file != NULL ? g_path_get_basename(item->file) : NULL;
    gtk_button_set_label(GTK_BUTTON(state->file), binding ? "Doğrulanmış medya seçildi" : "Kütüphaneden medya seç");
    gtk_label_set_text(GTK_LABEL(state->heading), item != NULL ? "Seçili öğe · Taslak" : "Yeni öğe");
    gtk_label_set_text(GTK_LABEL(state->error), binding ? "Medya kimliği ve SHA-256 kayıtlı. Devralma sırasında kaynak yeniden doğrulanır." : "Çalınabilir acil öğe için kütüphaneden doğrulanmış medya seçin.");
    gtk_widget_queue_draw(state->wave);
    g_free(basename);
}

static void
editor_save(GtkButton *button, gpointer data)
{
    EmergencyState *state = data;
    EmergencyItem *item = g_new0(EmergencyItem, 1);
    gboolean valid = TRUE;
    double duration = 0, previous = -1;
    (void)button;
    for (guint i = 0; i < G_N_ELEMENTS(item->values); ++i)
        item->values[i] = clean_entry(state->fields[i]);
    item->kind = gtk_combo_box_get_active(GTK_COMBO_BOX(state->kind));
    item->file = g_strdup(g_object_get_data(G_OBJECT(state->file), "selected-file"));
    EmergencyItem *old = state->editing ? g_object_get_data(G_OBJECT(state->editing), "em-item") : NULL;
    item->id = old && old->id ? g_strdup(old->id) : g_uuid_string_random();
    JsonObject *binding = g_object_get_data(G_OBJECT(state->file), "em-media-binding");
    for (guint i = 0; binding && i < G_N_ELEMENTS(binding_keys); ++i)
        item->binding[i] = g_strdup(radios_json_string(binding, binding_keys[i], ""));
    if (*item->values[0] == '\0') {
        gtk_label_set_text(GTK_LABEL(state->error), "İçerik adı gerekli.");
        gtk_widget_grab_focus(state->fields[0]);
        valid = FALSE;
    } else if (!parse_time(item->values[3], &duration)) {
        gtk_label_set_text(GTK_LABEL(state->error), "Süre için dk:sn.mmm biçimini kullan; saniye 0–59 olmalı.");
        valid = FALSE;
    }
    for (guint i = 4; valid && i < 8; ++i) {
        double mark = 0;
        if (*item->values[i] == '\0')
            continue;
        if (!parse_time(item->values[i], &mark) || duration <= 0 || mark > duration || mark < previous) {
            gtk_label_set_text(GTK_LABEL(state->error), "Cue noktaları A ≤ V ≤ S ≤ B sırasında ve toplam süre içinde olmalı.");
            gtk_widget_grab_focus(state->fields[i]);
            valid = FALSE;
        }
        previous = mark;
    }
    if (valid && *item->values[8] != '\0') {
        gchar *end = NULL;
        double fade = g_ascii_strtod(item->values[8], &end);
        if (*end != '\0' || !isfinite(fade) || fade < 0 || fade > 20000) {
            gtk_label_set_text(GTK_LABEL(state->error), "Fade 0–20000 ms aralığında olmalı.");
            valid = FALSE;
        }
    }
    if (!valid) {
        item_free(item);
        return;
    }
    GtkListBoxRow *row = state->editing;
    if (row != NULL) {
        g_object_set_data_full(G_OBJECT(row), "em-item", item, item_free);
        row_render(row);
        queue_updated(state);
    } else
        row = queue_append(state, active_queue(state), item);
    state->editing = row;
    gtk_list_box_select_row(GTK_LIST_BOX(gtk_widget_get_parent(GTK_WIDGET(row))), row);
    gtk_label_set_text(GTK_LABEL(state->error), binding ? "Doğrulanmış medya acil sıraya bağlandı; kalıcı kayıt sonucu durum çubuğunda gösterilir." : "Metadata taslağı kaydedildi; devralmada kullanılabilmesi için doğrulanmış medya seçin.");
    record_draft(state, "Sıra öğesi taslağı kaydedildi.");
}

static gchar *
format_time(double seconds)
{
    guint milliseconds = (guint)llround(MAX(0, seconds) * 1000);
    return g_strdup_printf("%02u:%02u.%03u", milliseconds / 60000, milliseconds / 1000 % 60, milliseconds % 1000);
}

static gboolean
wave_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    EmergencyState *state = data;
    double width = gtk_widget_get_allocated_width(widget);
    double height = gtk_widget_get_allocated_height(widget);
    cairo_set_source_rgb(cr, 0.02, 0.02, 0.02); cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.10, 0.10, 0.10); cairo_set_line_width(cr, 1);
    for (guint i = 1; i < 10; ++i) {
        cairo_move_to(cr, i * width / 10, 0); cairo_line_to(cr, i * width / 10, height); cairo_stroke(cr);
    }
    cairo_move_to(cr, 0, height / 2); cairo_line_to(cr, width, height / 2); cairo_stroke(cr);
    double duration = 0;
    if (parse_time(gtk_entry_get_text(GTK_ENTRY(state->fields[3])), &duration) && duration > 0) {
        const gchar *names[] = { "A", "V", "S", "B" };
        for (guint i = 0; i < 4; ++i) {
            const gchar *text = gtk_entry_get_text(GTK_ENTRY(state->fields[i + 4]));
            double position = 0;
            if (*text == '\0' || !parse_time(text, &position) || position > duration)
                continue;
            double x = 8 + position / duration * MAX(1, width - 16);
            cairo_set_source_rgb(cr, i == state->marker ? 0.85 : 0.45, 0.65, 0.25);
            cairo_move_to(cr, x, 2); cairo_line_to(cr, x, height - 2); cairo_stroke(cr);
            cairo_move_to(cr, CLAMP(x + 3, 8, width - 15), 15); cairo_show_text(cr, names[i]);
        }
    }
    cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    cairo_move_to(cr, 10, height - 10);
    cairo_show_text(cr, "Zaman cizgisi taslagi / waveform analizi bekleniyor");
    return FALSE;
}

static void
wave_field_changed(GtkEditable *editable, gpointer data)
{
    EmergencyState *state = data;
    (void)editable;
    gtk_widget_queue_draw(state->wave);
}

static void
wave_move(EmergencyState *state, double x)
{
    double duration = 0;
    if (!parse_time(gtk_entry_get_text(GTK_ENTRY(state->fields[3])), &duration) || duration <= 0) {
        gtk_label_set_text(GTK_LABEL(state->error), "Marker düzenlemek için önce geçerli toplam süre gir.");
        return;
    }
    double width = gtk_widget_get_allocated_width(state->wave);
    gchar *text = format_time(CLAMP((x - 8) / MAX(1, width - 16), 0, 1) * duration);
    gtk_entry_set_text(GTK_ENTRY(state->fields[state->marker + 4]), text);
    g_free(text);
}

static gboolean
wave_pressed(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    EmergencyState *state = data;
    if (event->button != 1)
        return FALSE;
    gtk_widget_grab_focus(widget);
    double duration = 0, closest = 12;
    parse_time(gtk_entry_get_text(GTK_ENTRY(state->fields[3])), &duration);
    if (duration > 0)
        for (guint i = 0; i < 4; ++i) {
            double position = 0;
            const gchar *text = gtk_entry_get_text(GTK_ENTRY(state->fields[i + 4]));
            if (*text != '\0' && parse_time(text, &position)) {
                double distance = fabs(8 + position / duration * MAX(1, gtk_widget_get_allocated_width(widget) - 16) - event->x);
                if (distance < closest) {
                    closest = distance;
                    state->marker = i;
                }
            }
        }
    state->dragging = TRUE;
    gtk_grab_add(widget);
    wave_move(state, event->x);
    return TRUE;
}

static gboolean
wave_motion(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    EmergencyState *state = data;
    (void)widget;
    if (!state->dragging)
        return FALSE;
    wave_move(state, event->x);
    return TRUE;
}

static gboolean
wave_released(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    EmergencyState *state = data;
    if (event->button != 1 || !state->dragging)
        return FALSE;
    state->dragging = FALSE;
    if (gtk_widget_has_grab(widget))
        gtk_grab_remove(widget);
    return TRUE;
}

static gboolean
wave_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    EmergencyState *state = data;
    (void)widget;
    const guint keys[] = { GDK_KEY_a, GDK_KEY_v, GDK_KEY_s, GDK_KEY_b };
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i)
        if (gdk_keyval_to_lower(event->keyval) == keys[i]) {
            state->marker = i;
            gtk_widget_queue_draw(state->wave);
            return TRUE;
        }
    if (event->keyval != GDK_KEY_Left && event->keyval != GDK_KEY_Right)
        return FALSE;
    double duration = 0, position = 0;
    if (!parse_time(gtk_entry_get_text(GTK_ENTRY(state->fields[3])), &duration) || duration <= 0)
        return TRUE;
    parse_time(gtk_entry_get_text(GTK_ENTRY(state->fields[state->marker + 4])), &position);
    position += (event->keyval == GDK_KEY_Left ? -1 : 1) * ((event->state & GDK_SHIFT_MASK) != 0 ? 1 : 0.1);
    gchar *text = format_time(CLAMP(position, 0, duration));
    gtk_entry_set_text(GTK_ENTRY(state->fields[state->marker + 4]), text);
    g_free(text);
    return TRUE;
}

static void
emergency_media_picked(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail, gpointer data)
{
    (void)owner; EmergencyState *state = data;
    if (state->disposed) return;
    JsonObject *media = json_object_get_object_member(detail, "media");
    JsonObject *version = json_object_get_object_member(detail, "version");
    if (!media || !version || !g_str_equal(radios_json_string(media, "station_id", ""), radios_data_station_id(ctx)) ||
        !json_object_get_boolean_member(version, "verified") || !json_object_get_boolean_member(version, "object_available")) {
        gtk_label_set_text(GTK_LABEL(state->error), "Erişilebilir ve doğrulanmış kütüphane medyası seçin."); return;
    }
    JsonObject *binding = json_object_new();
    const gchar *values[] = {radios_json_string(media, "_id", ""), radios_json_string(version, "_id", ""),
        radios_json_string(version, "media_file_id", ""), radios_json_string(version, "sha256", "")};
    for (guint i = 0; i < G_N_ELEMENTS(binding_keys); ++i) json_object_set_string_member(binding, binding_keys[i], values[i]);
    ++state->cue_intent; state->cue_pending = FALSE;
    radios_audio_ui_preview_stop(state->ctx, state->page);
    g_object_set_data_full(G_OBJECT(state->file), "em-media-binding", binding, (GDestroyNotify)json_object_unref);
    g_object_set_data_full(G_OBJECT(state->file), "selected-file", g_strdup(""), g_free);
    const gchar *metadata[] = {"title", "artist", "album"};
    for (guint i = 0; i < G_N_ELEMENTS(metadata); ++i)
        gtk_entry_set_text(GTK_ENTRY(state->fields[i]), radios_json_string(media, metadata[i], ""));
    gchar *duration = format_time(radios_json_int(version, "duration_ms", 0) / 1000.0);
    gtk_entry_set_text(GTK_ENTRY(state->fields[3]), duration); g_free(duration);
    gtk_widget_set_sensitive(state->fields[3], FALSE);
    const gchar *mapped[] = {"music", "announcement", "jingle", "announcement", "advertisement"};
    for (guint i = 0; i < G_N_ELEMENTS(mapped); ++i)
        if (g_str_equal(radios_json_string(media, "kind", ""), mapped[i])) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(state->kind), (gint)i); break;
        }
    gtk_button_set_label(GTK_BUTTON(state->file), "Doğrulanmış medya seçildi");
    gtk_label_set_text(GTK_LABEL(state->error), "SHA-256 ile doğrulanmış medya seçildi. Sıraya ekle ile acil listeye bağlayın.");
}

static void
file_clicked(GtkButton *button, gpointer data)
{
    (void)button; EmergencyState *state = data;
    if (!state->disposed) radios_media_library_pick(state->ctx, state->page, emergency_media_picked, state);
}

typedef struct { EmergencyState *state; guint intent; guint64 loss_epoch; } EmergencyCue;

static void
emergency_cue_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    EmergencyCue *request = data; EmergencyState *state = request->state;
    if (state->disposed || state->cue_intent != request->intent) return;
    state->cue_pending = FALSE;
    if (request->loss_epoch != radios_audio_ui_loss_epoch(ctx)) {
        gtk_label_set_text(GTK_LABEL(state->error), "Ses bağlantısı kesildi; CUE için yeniden önizleyin."); return;
    }
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        gtk_label_set_text(GTK_LABEL(state->error), error ? error->message : "Medya önizlemesi hazırlanamadı."); return;
    }
    JsonObject *binding = g_object_get_data(G_OBJECT(state->file), "em-media-binding");
    JsonObject *detail = json_node_get_object(result);
    JsonObject *version = json_object_get_object_member(detail, "version");
    if (!binding || !version || !g_str_equal(radios_json_string(binding, "version_id", ""), radios_json_string(version, "_id", ""))) return;
    if (!g_str_equal(radios_json_string(binding, "media_file_id", ""), radios_json_string(version, "media_file_id", "")) ||
        !g_str_equal(radios_json_string(binding, "media_sha256", ""), radios_json_string(version, "sha256", ""))) {
        gtk_label_set_text(GTK_LABEL(state->error), "Kayıtlı medya kimliği değişti; kütüphaneden yeniden seçin."); return;
    }
    double start = 0, end = 0;
    parse_time(gtk_entry_get_text(GTK_ENTRY(state->fields[4])), &start);
    parse_time(gtk_entry_get_text(GTK_ENTRY(state->fields[7])), &end);
    GError *failure = NULL;
    if (!radios_audio_ui_preview(ctx, owner, detail, (guint64)llround(start * 1000), (guint64)llround(end * 1000), &failure)) {
        gtk_label_set_text(GTK_LABEL(state->error), failure ? failure->message : "CUE başlatılamadı."); g_clear_error(&failure);
    } else gtk_label_set_text(GTK_LABEL(state->error), "Doğrulanmış medya CUE çıkışında önizleniyor.");
}

static void
emergency_cue(GtkButton *button, gpointer data)
{
    (void)button; EmergencyState *state = data;
    ++state->cue_intent;
    if (state->cue_pending || radios_audio_ui_preview_owned(state->ctx, state->page)) {
        state->cue_pending = FALSE;
        radios_audio_ui_preview_stop(state->ctx, state->page);
        gtk_label_set_text(GTK_LABEL(state->error), "CUE önizlemesi iptal edildi."); return;
    }
    JsonObject *binding = g_object_get_data(G_OBJECT(state->file), "em-media-binding");
    if (!binding) {
        gtk_label_set_text(GTK_LABEL(state->error), "CUE için önce doğrulanmış medya seçin."); return;
    }
    EmergencyCue *request = g_new0(EmergencyCue, 1); request->state = state; request->intent = state->cue_intent;
    request->loss_epoch = radios_audio_ui_loss_epoch(state->ctx);
    state->cue_pending = radios_media_library_get(state->ctx, state->page, radios_json_string(binding, "media_id", ""), emergency_cue_loaded, request, g_free);
}

static GtkWidget *
editor_new(EmergencyState *state)
{
    GtkWidget *card = radios_ui_box(GTK_ORIENTATION_VERTICAL, 10, "em-card em-editor");
    GtkWidget *grid = fields_grid();
    GtkWidget *wave = gtk_drawing_area_new();
    GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "em-editor-actions");
    GtkWidget *file = radios_ui_button("Kütüphaneden medya seç");
    state->file = file;
    state->wave = wave;
    state->heading = radios_ui_label("Yeni öğe", "em-card-title");
    state->error = radios_ui_label("", "em-form-status");
    for (guint i = 0; i < 9; ++i)
        state->fields[i] = i < 3 ? entry(NULL) : numeric(i == 8 ? "0" : "mm:ss.mmm");
    state->kind = radios_ui_combo(kinds, 0);
    gtk_entry_set_max_length(GTK_ENTRY(state->fields[0]), 160);
    gtk_entry_set_max_length(GTK_ENTRY(state->fields[1]), 120);
    gtk_entry_set_max_length(GTK_ENTRY(state->fields[2]), 120);
    gtk_widget_set_name(state->fields[0], "em-editor-title");
    gtk_widget_set_name(state->fields[3], "em-editor-duration");
    gtk_widget_set_name(wave, "em-wave-canvas");
    gtk_widget_set_name(file, "em-editor-file");
    pack(card, state->heading, FALSE);
    gtk_grid_attach(GTK_GRID(grid), field("İçerik adı", state->fields[0]), 0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), field("Sanatçı / kaynak", state->fields[1]), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), field("Albüm / program", state->fields[2]), 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), field("Tür", state->kind), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), field("Süre", state->fields[3]), 1, 1, 1, 1);
    pack(card, grid, FALSE);
    gtk_widget_set_size_request(wave, -1, 112);
    gtk_widget_set_can_focus(wave, TRUE);
    gtk_widget_add_events(wave, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK);
    g_signal_connect(wave, "draw", G_CALLBACK(wave_draw), state);
    g_signal_connect(wave, "button-press-event", G_CALLBACK(wave_pressed), state);
    g_signal_connect(wave, "button-release-event", G_CALLBACK(wave_released), state);
    g_signal_connect(wave, "motion-notify-event", G_CALLBACK(wave_motion), state);
    g_signal_connect(wave, "key-press-event", G_CALLBACK(wave_key), state);
    gtk_widget_set_tooltip_text(wave, "Toplam süre gir. A/V/S/B tuşuyla marker seç; sürükle veya ←/→ (Shift: 1 sn) ile düzenle. Ses analizi bağlı değil.");
    pack(card, wave, FALSE);
    pack(card, radios_ui_label("00:00", "em-field-label"), FALSE);
    grid = fields_grid();
    const gchar *cues[] = { "A · Cue başlangıcı", "V · Vokal girişi", "S · Sonraki başlangıç", "B · Çıkış noktası", "Fade · ms" };
    for (guint i = 0; i < G_N_ELEMENTS(cues); ++i) {
        add_field(grid, i, 5, cues[i], state->fields[i + 4]);
        gchar *name = g_strdup_printf("em-editor-marker-%u", i);
        gtk_widget_set_name(state->fields[i + 4], name);
        g_free(name);
    }
    for (guint i = 3; i < 8; ++i)
        g_signal_connect(state->fields[i], "changed", G_CALLBACK(wave_field_changed), state);
    pack(card, grid, FALSE);
    g_signal_connect(file, "clicked", G_CALLBACK(file_clicked), state);
    pack(actions, file, FALSE);
    GtkWidget *cue = radios_ui_button("CUE ▶");
    gtk_widget_set_name(cue, "em-editor-cue");
    g_signal_connect(cue, "clicked", G_CALLBACK(emergency_cue), state);
    pack(actions, cue, FALSE);
    GtkWidget *gain = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -60, 6, 0.5);
    gtk_range_set_value(GTK_RANGE(gain), -12);
    gtk_widget_set_size_request(gain, 140, -1);
    pack(actions, field("Kulaklık", gain), TRUE);
    GtkWidget *save = radios_ui_button("Sıraya ekle");
    gtk_widget_set_name(save, "em-editor-save");
    g_signal_connect(save, "clicked", G_CALLBACK(editor_save), state);
    pack(actions, save, FALSE);
    pack(card, actions, FALSE);
    pack(card, state->error, FALSE);
    g_object_set_data(G_OBJECT(card), "em-editor-fields", state->fields);
    return card;
}

static void
chip_clicked(GtkButton *button, gpointer data)
{
    GtkStack *stack = data;
    GtkRevealer *chain = g_object_get_data(G_OBJECT(button), "em-chain");
    const gchar *name = g_object_get_data(G_OBJECT(button), "em-page");
    if (chain != NULL) {
        gtk_revealer_set_reveal_child(chain, !gtk_revealer_get_reveal_child(chain));
        return;
    }
    gtk_stack_set_visible_child_name(stack, name);
}

static void
takeover_destroyed(GtkWidget *dialog, gpointer data)
{
    EmergencyState *state = data;
    if (state->takeover_dialog == dialog)
        state->takeover_dialog = NULL;
}

static void
owned_dialog_destroyed(GtkWidget *dialog, gpointer data)
{
    EmergencyState *state = data;
    if (state->file_dialog == dialog)
        state->file_dialog = NULL;
    if (state->export_dialog == dialog)
        state->export_dialog = NULL;
    if (state->transfer_dialog == dialog) {
        state->transfer_dialog = NULL;
        state->transfer_pending = FALSE;
        ++state->transfer_intent;
    }
}

static void
takeover_response(GtkDialog *dialog, gint response, gpointer data)
{
    EmergencyState *state = data;
    if (!state->disposed && response == GTK_RESPONSE_APPLY) {
        GtkWidget *confirmed = g_object_get_data(G_OBJECT(dialog), "em-confirmed");
        GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "em-status");
        GtkWidget *strategy = g_object_get_data(G_OBJECT(dialog), "em-strategy");
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(confirmed))) {
            gtk_label_set_text(GTK_LABEL(status), "MAIN çıkışında devralmayı başlatmak için çıkış onayını işaretleyin."); return;
        }
        gint64 revision = radios_data_document_revision(state->document);
        if (state->settings_dirty || revision <= 0) {
            gtk_label_set_text(GTK_LABEL(status), "Acil listeyi ve hazırlık değişikliklerini önce kaydedin; kayıt sonucunu bekleyin."); return;
        }
        GError *error = NULL;
        gboolean accepted = radios_broadcast_ui_emergency_start(state->ctx, state->page,
            radios_data_document_id(state->document), revision,
            (guint)gtk_combo_box_get_active(GTK_COMBO_BOX(strategy)), &error);
        if (!accepted) {
            gtk_label_set_text(GTK_LABEL(status), error ? error->message : "Acil yayın hazırlanamadı."); g_clear_error(&error); return;
        }
        gtk_label_set_text(GTK_LABEL(state->error), "Kayıtlı acil sıra yeniden doğrulanıyor; hazırlık tamamlanınca MAIN devralınacak.");
        gtk_widget_destroy(GTK_WIDGET(dialog)); return;
    }
    if (!state->disposed && response == GTK_RESPONSE_ACCEPT) {
        GtkWidget *strategy = g_object_get_data(G_OBJECT(dialog), "em-strategy");
        GtkWidget *name = g_object_get_data(G_OBJECT(dialog), "em-name");
        GtkWidget *confirmed = g_object_get_data(G_OBJECT(dialog), "em-confirmed");
        GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "em-status");
        gchar *text = clean_entry(name);
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(confirmed)) ||
            (gtk_combo_box_get_active(GTK_COMBO_BOX(strategy)) == 0 && *text == '\0')) {
            gtk_label_set_text(GTK_LABEL(status), "Hedef çıkış onayını işaretle; acil liste davranışı için liste adı gerekli.");
            g_free(text);
            return;
        }
        GtkWidget *panel = gtk_stack_get_child_by_name(GTK_STACK(state->settings_stack), "takeover");
        gtk_combo_box_set_active(GTK_COMBO_BOX(find_field(panel, "Devralma davranışı")), 0);
        gtk_entry_set_text(GTK_ENTRY(find_field(panel, "Acil liste adı")), text);
        preparation_save(NULL, state);
        g_free(text);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
takeover_open(GtkButton *button, gpointer data)
{
    EmergencyState *state = data;
    (void)button;
    if (state->takeover_dialog != NULL) {
        gtk_window_present(GTK_WINDOW(state->takeover_dialog));
        return;
    }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Acil yayın · Devralma", state->ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL,
        "Hazırlık taslağını kaydet", GTK_RESPONSE_ACCEPT, "Devralmayı başlat", GTK_RESPONSE_APPLY, NULL);
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    const gchar *names[] = { "Hedef çıkış", "Aktif / yedek motor", "Doğrulanmış yerel ses / plan", "Çevrimdışı çalışma süresi", "Son geri dönüş testi" };
    GtkWidget *panel = gtk_stack_get_child_by_name(GTK_STACK(state->settings_stack), "takeover");
    const gchar *lists[] = {"Acil listeyi başlat", "Geçici sırayı başlat", NULL};
    GtkWidget *strategy = radios_ui_combo(lists, (gint)active_queue(state));
    GtkWidget *name = entry("Hazırlanacak acil listenin adı");
    GtkWidget *confirmed = gtk_check_button_new_with_label("Devralmanın hedef çıkışı etkileyeceğini anlıyorum.");
    GtkWidget *status = radios_ui_label("", "ops-help");
    gtk_widget_set_name(dialog, "emergency-dialog");
    gtk_widget_set_name(strategy, "emergency-strategy");
    gtk_widget_set_name(name, "emergency-list-name");
    gtk_widget_set_name(confirmed, "emergency-confirmed");
    gtk_entry_set_max_length(GTK_ENTRY(name), 100);
    gtk_entry_set_text(GTK_ENTRY(name), gtk_entry_get_text(GTK_ENTRY(find_field(panel, "Acil liste adı"))));
    gtk_container_set_border_width(GTK_CONTAINER(body), 18);
    gtk_box_set_spacing(GTK_BOX(body), 10);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 480, -1);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "ops-dialog");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
    pack(body, facts_new(names, G_N_ELEMENTS(names), 2), FALSE);
    pack(body, field("Devralınacak sıra", strategy), FALSE);
    pack(body, field("Acil liste adı", name), FALSE);
    pack(body, confirmed, FALSE);
    pack(body, status, FALSE);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_APPLY, radios_data_connected(state->ctx));
    g_object_set_data(G_OBJECT(dialog), "em-strategy", strategy);
    g_object_set_data(G_OBJECT(dialog), "em-name", name);
    g_object_set_data(G_OBJECT(dialog), "em-confirmed", confirmed);
    g_object_set_data(G_OBJECT(dialog), "em-status", status);
    state->takeover_dialog = dialog;
    g_signal_connect(dialog, "response", G_CALLBACK(takeover_response), state);
    g_signal_connect(dialog, "destroy", G_CALLBACK(takeover_destroyed), state);
    gtk_widget_show_all(dialog);
}

static void
export_response(GtkDialog *dialog, gint response, gpointer data)
{
    EmergencyState *state = data;
    if (!state->disposed && response == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        const gchar *contents = g_object_get_data(G_OBJECT(dialog), "export-contents");
        GError *error = NULL;
        if (filename != NULL && g_file_set_contents(filename, contents, -1, &error))
            record_draft(state, "Yerel taslak dosyaya dışa aktarıldı.");
        else {
            gtk_label_set_text(GTK_LABEL(state->error), error != NULL ? error->message : "Dışa aktarım yolu seçilemedi.");
            g_clear_error(&error);
        }
        g_free(filename);
    }
    state->export_dialog = NULL;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
snapshot_saved_rows(GtkWidget *root, GKeyFile *file, const gchar *page, guint *index)
{
    GHashTable *values = g_object_get_data(G_OBJECT(root), "draft-values");
    if (values != NULL) {
        gchar *group = g_strdup_printf("%s draft %u", page, ++*index);
        g_key_file_set_string(file, group, "draft-kind", g_object_get_data(G_OBJECT(root), "draft-action-name"));
        GHashTableIter iterator;
        gpointer key, value;
        g_hash_table_iter_init(&iterator, values);
        while (g_hash_table_iter_next(&iterator, &key, &value))
            g_key_file_set_string(file, group, key, value != NULL ? value : "");
        g_free(group);
        return;
    }
    if (GTK_IS_CONTAINER(root)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(root));
        for (GList *item = children; item != NULL; item = item->next)
            snapshot_saved_rows(item->data, file, page, index);
        g_list_free(children);
    }
}

static GKeyFile *
preparation_snapshot(EmergencyState *state)
{
    GKeyFile *file = g_key_file_new();
    const gchar *pages[] = { "takeover", "silence", "alert", "access", "handover" };
    g_key_file_set_string(file, "RadioS", "mode", "Emergency preparation snapshot; playback requires a separate command");
    g_key_file_set_string(file, "RadioS", "actual-broadcast-state", "Unknown");
    for (guint i = 0; i < G_N_ELEMENTS(pages); ++i) {
        GHashTable *values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        GtkWidget *panel = gtk_stack_get_child_by_name(GTK_STACK(state->settings_stack), pages[i]);
        snapshot_fields(panel, values);
        GHashTableIter iterator;
        gpointer key, value;
        g_hash_table_iter_init(&iterator, values);
        while (g_hash_table_iter_next(&iterator, &key, &value))
            g_key_file_set_string(file, pages[i], key, value != NULL ? value : "");
        g_hash_table_unref(values);
        guint saved_index = 0;
        snapshot_saved_rows(panel, file, pages[i], &saved_index);
    }
    const gchar *names[] = { "List", "Queue" };
    const gchar *keys[] = { "title", "artist", "album", "duration", "cue-start", "vocal", "next", "out", "fade-ms" };
    for (guint i = 0; i < 2; ++i) {
        GList *rows = gtk_container_get_children(GTK_CONTAINER(state->lists[i]));
        guint index = 0;
        for (GList *node = rows; node != NULL; node = node->next) {
            EmergencyItem *item = g_object_get_data(G_OBJECT(node->data), "em-item");
            gchar *group = g_strdup_printf("%s item %u", names[i], ++index);
            for (guint k = 0; k < G_N_ELEMENTS(keys); ++k)
                g_key_file_set_string(file, group, keys[k], item->values[k]);
            g_key_file_set_integer(file, group, "type", item->kind);
            g_key_file_set_string(file, group, "file-reference", item->file != NULL ? item->file : "");
            g_free(group);
        }
        g_list_free(rows);
    }
    g_key_file_set_string(file, "RadioS", "draft-log", state->log->str);
    return file;
}

static void persist_saved_rows(EmergencyState *state)
{
    if (state->loading || !radios_data_document_ready(state->document)) return;
    if (!state->preparation) state->preparation = g_key_file_new();
    gsize group_count = 0; gchar **groups = g_key_file_get_groups(state->preparation, &group_count);
    for (gsize i = 0; i < group_count; ++i)
        if (strstr(groups[i], " draft ")) g_key_file_remove_group(state->preparation, groups[i], NULL);
    g_strfreev(groups);
    const gchar *pages[] = {"takeover", "silence", "alert", "access", "handover"};
    for (guint i = 0; i < G_N_ELEMENTS(pages); ++i) {
        guint count = 0;
        snapshot_saved_rows(gtk_stack_get_child_by_name(GTK_STACK(state->settings_stack), pages[i]), state->preparation, pages[i], &count);
    }
    g_key_file_set_string(state->preparation, "RadioS", "mode", "Database preparation; playback requires a separate command");
    g_key_file_set_string(state->preparation, "RadioS", "actual-broadcast-state", "Unknown");
    g_key_file_set_string(state->preparation, "RadioS", "draft-log", state->log->str);
    g_object_set_data(G_OBJECT(state->page), "em-preparation-draft", state->preparation);
    emergency_save(state);
}

static void collect_draft_actions(GtkWidget *root, GPtrArray *actions)
{
    DraftAction *action = g_object_get_data(G_OBJECT(root), "em-draft-action");
    if (action) g_ptr_array_add(actions, action);
    if (GTK_IS_CONTAINER(root)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(root));
        for (GList *child = children; child; child = child->next) collect_draft_actions(child->data, actions);
        g_list_free(children);
    }
}

static void restore_saved_rows(EmergencyState *state, GtkWidget *panel, const gchar *page)
{
    GPtrArray *actions = g_ptr_array_new(); collect_draft_actions(panel, actions);
    for (guint i = 0; i < actions->len; ++i) {
        DraftAction *action = g_ptr_array_index(actions, i); action->editing = NULL;
        GList *rows = gtk_container_get_children(GTK_CONTAINER(action->list));
        for (GList *row = rows; row; row = row->next) gtk_widget_destroy(row->data);
        g_list_free(rows);
    }
    for (guint i = 1; i <= 500; ++i) {
        gchar *group = g_strdup_printf("%s draft %u", page, i);
        if (!g_key_file_has_group(state->preparation, group)) { g_free(group); break; }
        gchar *kind = g_key_file_get_string(state->preparation, group, "draft-kind", NULL);
        for (guint j = 0; kind && j < actions->len; ++j) {
            DraftAction *action = g_ptr_array_index(actions, j);
            if (!g_str_equal(action->action, kind)) continue;
            GHashTable *values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
            gsize count = 0; gchar **keys = g_key_file_get_keys(state->preparation, group, &count, NULL);
            for (gsize k = 0; k < count; ++k)
                g_hash_table_insert(values, g_strdup(keys[k]), g_key_file_get_string(state->preparation, group, keys[k], NULL));
            restore_fields(action->section, values); GString *summary = g_string_new(kind);
            for (guint k = 0; action->required[k]; ++k) {
                const gchar *value = g_hash_table_lookup(values, action->required[k]);
                g_string_append_printf(summary, " · %s: %s", action->required[k], value ? value : "");
            }
            gboolean cancelled = g_strcmp0(g_hash_table_lookup(values, "Taslak durumu"), "İptal") == 0;
            if (cancelled) g_string_prepend(summary, "İptal · ");
            mini_add(action, summary->str);
            GList *rows = gtk_container_get_children(GTK_CONTAINER(action->list));
            GtkWidget *row = g_list_last(rows)->data;
            g_object_set_data_full(G_OBJECT(row), "draft-values", g_hash_table_ref(values), (GDestroyNotify)g_hash_table_unref);
            gtk_widget_set_sensitive(row, !cancelled);
            g_list_free(rows); g_string_free(summary, TRUE); g_hash_table_unref(values); g_strfreev(keys); break;
        }
        g_free(kind); g_free(group);
    }
    g_ptr_array_unref(actions);
}

static void
preparation_save(GtkButton *button, gpointer data)
{
    EmergencyState *state = data;
    (void)button;
    const gchar *names[] = { "Eşik · dB", "Süre · sn", "2 · Sürerse · sn", "Ses dönünce eşik · dB", "Dönüş süresi · sn", "Fade · ms", "Oynatma RTO · sn", "Yayın günlüğü RPO · dk", "Katalog RPO · dk", "Ses arşivi RPO · sa" };
    const double minima[] = { -90, 1, 5, -90, 1, 0, 1, 0, 0, 0 };
    const double maxima[] = { 0, 600, 3600, 0, 600, 10000, 86400, 1440, 10080, 720 };
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *input = find_field(state->settings_stack, names[i]);
        gchar *text = control_text(input);
        if (*text != '\0') {
            gchar *end = NULL;
            double number = g_ascii_strtod(text, &end);
            if (*end != '\0' || !isfinite(number) || number < minima[i] || number > maxima[i]) {
                gchar *message = g_strdup_printf("%s %.0f–%.0f aralığında olmalı.", names[i], minima[i], maxima[i]);
                gtk_label_set_text(GTK_LABEL(state->error), message);
                gtk_widget_grab_focus(input);
                g_free(message);
                g_free(text);
                return;
            }
        }
        g_free(text);
    }
    GtkWidget *silence = gtk_stack_get_child_by_name(GTK_STACK(state->settings_stack), "silence");
    GtkWidget *enabled = find_button(silence, "Sessizlikte otomatik devralma");
    GtkWidget *threshold = find_field(silence, "Eşik · dB");
    GtkWidget *seconds = find_field(silence, "Süre · sn");
    GtkWidget *restore = find_field(silence, "Ses dönünce eşik · dB");
    const gchar *level_text = gtk_entry_get_text(GTK_ENTRY(threshold));
    const gchar *restore_text = gtk_entry_get_text(GTK_ENTRY(restore));
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enabled)) &&
        (*level_text == '\0' || *gtk_entry_get_text(GTK_ENTRY(seconds)) == '\0')) {
        gtk_stack_set_visible_child_name(GTK_STACK(state->settings_stack), "silence");
        gtk_label_set_text(GTK_LABEL(state->error), "Otomatik devralma taslağı için sessizlik eşiği ve süre gerekli.");
        gtk_widget_grab_focus(*level_text == '\0' ? threshold : seconds);
        return;
    }
    if (*level_text != '\0' && *restore_text != '\0' && g_ascii_strtod(restore_text, NULL) <= g_ascii_strtod(level_text, NULL)) {
        gtk_stack_set_visible_child_name(GTK_STACK(state->settings_stack), "silence");
        gtk_label_set_text(GTK_LABEL(state->error), "Dönüş eşiği sessizlik eşiğinden yüksek olmalı.");
        gtk_widget_grab_focus(restore);
        return;
    }
    g_clear_pointer(&state->preparation, g_key_file_unref);
    state->preparation = preparation_snapshot(state);
    state->settings_dirty = FALSE;
    g_object_set_data(G_OBJECT(state->page), "em-preparation-draft", state->preparation);
    gtk_label_set_text(GTK_LABEL(state->error), state->ctx->data ? "Acil hazırlık veritabanına gönderiliyor; gerçek yayın komutu gönderilmedi." :
        "Acil hazırlık bu oturumun taslağına kaydedildi; gerçek yayın komutu gönderilmedi.");
    record_draft(state, "Acil hazırlık oturum taslağı kaydedildi.");
    emergency_save(state);
}

typedef struct {
    EmergencyState *state;
    guint64 intent;
    gchar *source_id, *target_id, *store_id, *station_id, *user_id;
    gint64 source_revision, target_revision;
    JsonNode *source_config;
} EmergencyTransfer;

static void transfer_free(gpointer value)
{
    EmergencyTransfer *transfer = value;
    g_free(transfer->source_id); g_free(transfer->target_id);
    g_free(transfer->store_id); g_free(transfer->station_id); g_free(transfer->user_id);
    json_node_free(transfer->source_config); g_free(transfer);
}

static void transfer_completed(RadioSViewContext *ctx, GtkWidget *dialog, JsonNode *result,
    const GError *error, gpointer value)
{
    EmergencyTransfer *transfer = value; EmergencyState *state = transfer->state;
    if (state->disposed || state->transfer_intent != transfer->intent || state->transfer_dialog != dialog) return;
    state->transfer_pending = FALSE;
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, TRUE);
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "em-transfer-status");
    if (error) { gtk_label_set_text(GTK_LABEL(status), error->message); return; }
    RadioSDocument *target = radios_broadcast_programme_document(ctx);
    RadioSDataClient *client = radios_data_client_acquire(ctx);
    JsonNode *current = emergency_configuration(state);
    gboolean unchanged = radios_data_connected(ctx) && client &&
        !g_strcmp0(radios_data_client_store_id(client), transfer->store_id) &&
        !g_strcmp0(radios_data_client_user_id(client), transfer->user_id) &&
        !g_strcmp0(radios_data_station_id(ctx), transfer->station_id) &&
        !g_strcmp0(radios_data_document_id(state->document), transfer->source_id) &&
        radios_data_document_revision(state->document) == transfer->source_revision &&
        target && !g_strcmp0(radios_data_document_id(target), transfer->target_id) &&
        radios_data_document_revision(target) == transfer->target_revision &&
        json_node_equal(current, transfer->source_config);
    json_node_free(current); if (client) radios_data_client_unref(client);
    if (!unchanged) {
        gtk_label_set_text(GTK_LABEL(status), "Kaynak sıra, Mevcut program veya veri kimliği değişti. Hiçbir öğe aktarılmadı; güncel kayıtlar hazır olunca tekrar deneyin."); return;
    }
    JsonObject *object = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    JsonNode *playback_node = object ? json_object_get_member(object, "playback") : NULL;
    JsonObject *playback = playback_node && JSON_NODE_HOLDS_OBJECT(playback_node) ? json_node_get_object(playback_node) : NULL;
    JsonNode *tracks_node = playback ? json_object_get_member(playback, "tracks") : NULL;
    if (!object || g_strcmp0(radios_json_string(object, "document_id", ""), transfer->source_id) ||
        radios_json_int(object, "revision", 0) != transfer->source_revision ||
        radios_json_int(object, "queue_index", -1) != 1 || !tracks_node || !JSON_NODE_HOLDS_ARRAY(tracks_node)) {
        gtk_label_set_text(GTK_LABEL(status), "Doğrulama yanıtı kayıtlı geçici sırayla eşleşmedi; program değiştirilmedi."); return;
    }
    GError *failure = NULL;
    if (!radios_broadcast_append_playlist(ctx, json_node_get_array(tracks_node), &failure)) {
        gtk_label_set_text(GTK_LABEL(status), failure ? failure->message : "Program taslağına aktarım başlatılamadı.");
        g_clear_error(&failure); return;
    }
    gtk_label_set_text(GTK_LABEL(state->error), "Doğrulanan geçici sıra Mevcut program taslağının sonuna eklendi; veritabanı kayıt sonucu bekleniyor. Yayın görünümündeki kayıt durumunu izleyin.");
    if (ctx->notify) ctx->notify(ctx->notify_data, "Mevcut program kaydı gönderildi; kalıcı kayıt sonucu henüz bekleniyor.");
    gtk_widget_destroy(dialog);
}

static void transfer_response(GtkDialog *dialog, gint response, gpointer value)
{
    EmergencyState *state = value;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    if (state->disposed || state->transfer_pending) return;
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "em-transfer-status");
    RadioSDocument *target = radios_broadcast_programme_document(state->ctx);
    gint64 source_revision = radios_data_document_revision(state->document);
    gint64 target_revision = radios_data_document_revision(target);
    if (!radios_data_connected(state->ctx) || source_revision <= 0 || target_revision <= 0) {
        gtk_label_set_text(GTK_LABEL(status), "Önce acil hazırlık sırasını ve Yayın görünümündeki Mevcut program taslağını kaydedin; iki kaydın da onayı tamamlanmalı."); return;
    }
    RadioSDataClient *client = radios_data_client_acquire(state->ctx);
    if (!client) { gtk_label_set_text(GTK_LABEL(status), "Veri kimliği hazır değil; sıra korunuyor."); return; }
    EmergencyTransfer *transfer = g_new0(EmergencyTransfer, 1);
    transfer->state = state; transfer->intent = ++state->transfer_intent;
    transfer->source_id = g_strdup(radios_data_document_id(state->document));
    transfer->target_id = g_strdup(radios_data_document_id(target));
    transfer->source_revision = source_revision; transfer->target_revision = target_revision;
    transfer->store_id = g_strdup(radios_data_client_store_id(client));
    transfer->user_id = g_strdup(radios_data_client_user_id(client));
    transfer->station_id = g_strdup(radios_data_station_id(state->ctx));
    transfer->source_config = emergency_configuration(state);
    radios_data_client_unref(client);
    JsonNode *body = radios_data_body(state->ctx); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "document_id", transfer->source_id);
    json_object_set_int_member(object, "expected_revision", source_revision);
    json_object_set_int_member(object, "queue_index", 1);
    state->transfer_pending = TRUE;
    gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, FALSE);
    if (!radios_data_request(state->ctx, GTK_WIDGET(dialog), "POST", "/v1/workflows/desktop-broadcast/emergency-prepare",
            body, transfer_completed, transfer, transfer_free)) {
        state->transfer_pending = FALSE;
        gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, TRUE);
        gtk_label_set_text(GTK_LABEL(status), "Medya doğrulaması başlatılamadı; kaynak sıra ve program korunuyor.");
    } else gtk_label_set_text(GTK_LABEL(status), "Kayıtlı geçici sıranın medya sürümleri doğrulanıyor…");
    json_node_free(body);
}

static void programme_transfer_clicked(GtkButton *button, gpointer value)
{
    EmergencyState *state = value; (void)button;
    if (state->disposed) return;
    if (state->transfer_dialog) { gtk_window_present(GTK_WINDOW(state->transfer_dialog)); return; }
    GList *rows = gtk_container_get_children(GTK_CONTAINER(state->lists[1]));
    gboolean empty = !rows; g_list_free(rows);
    if (empty) { gtk_label_set_text(GTK_LABEL(state->error), "Geçici sıra boş; önce doğrulanmış medya ekleyin."); return; }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Geçici sırayı programa aktar", state->ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL, "Aktar", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(dialog, "em-programme-transfer-dialog");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *status = radios_ui_label("", "em-error");
    gtk_widget_set_name(status, "em-programme-transfer-status");
    gtk_label_set_line_wrap(GTK_LABEL(status), TRUE); pack(body, status, FALSE);
    g_object_set_data(G_OBJECT(dialog), "em-transfer-status", status);
    state->transfer_dialog = dialog;
    g_signal_connect(dialog, "response", G_CALLBACK(transfer_response), state);
    g_signal_connect(dialog, "destroy", G_CALLBACK(owned_dialog_destroyed), state);
    gtk_widget_show_all(dialog);
}

static void
export_clicked(GtkButton *button, gpointer data)
{
    EmergencyState *state = data;
    (void)button;
    if (state->export_dialog != NULL) {
        gtk_window_present(GTK_WINDOW(state->export_dialog));
        return;
    }
    GKeyFile *snapshot = preparation_snapshot(state);
    gchar *contents = g_key_file_to_data(snapshot, NULL, NULL);
    g_key_file_unref(snapshot);
    state->export_dialog = gtk_file_chooser_dialog_new("İşlem taslağını indir", state->ctx->parent,
        GTK_FILE_CHOOSER_ACTION_SAVE, "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(state->export_dialog), TRUE);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(state->export_dialog), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(state->export_dialog), "RadioS-acil-hazirlik.ini");
    if (state->ctx->config != NULL && state->ctx->config->project_root != NULL)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(state->export_dialog), state->ctx->config->project_root);
    g_object_set_data_full(G_OBJECT(state->export_dialog), "export-contents", contents, g_free);
    g_signal_connect(state->export_dialog, "response", G_CALLBACK(export_response), state);
    g_signal_connect(state->export_dialog, "destroy", G_CALLBACK(owned_dialog_destroyed), state);
    gtk_widget_show(state->export_dialog);
}

static void
emergency_destroyed(GtkWidget *page, gpointer data)
{
    EmergencyState *state = data;
    (void)page;
    state->disposed = TRUE;
    if (state->runtime_timer) { g_source_remove(state->runtime_timer); state->runtime_timer = 0; }
    radios_audio_ui_preview_stop(state->ctx, page);
    disconnect_tree(page, state);
    if (state->dragging && gtk_widget_has_grab(state->wave))
        gtk_grab_remove(state->wave);
    ++state->transfer_intent;
    GtkWidget **dialogs[] = { &state->item_dialog, &state->file_dialog, &state->takeover_dialog, &state->export_dialog, &state->transfer_dialog };
    for (guint i = 0; i < G_N_ELEMENTS(dialogs); ++i)
        if (*dialogs[i] != NULL) {
            g_signal_handlers_disconnect_by_data(*dialogs[i], state);
            gtk_widget_destroy(*dialogs[i]);
            *dialogs[i] = NULL;
        }
}

static gboolean
emergency_runtime_tick(gpointer data)
{
    EmergencyState *state = data;
    if (state->disposed) return G_SOURCE_REMOVE;
    gchar *text = radios_broadcast_ui_emergency_status(state->ctx);
    gtk_label_set_text(GTK_LABEL(state->runtime_status), text ? text : "Normal yayın");
    g_free(text);
    return G_SOURCE_CONTINUE;
}

static void
emergency_return(GtkButton *button, gpointer data)
{
    (void)button; EmergencyState *state = data; GError *error = NULL;
    if (!radios_broadcast_ui_emergency_return(state->ctx, &error)) {
        gtk_label_set_text(GTK_LABEL(state->error), error ? error->message : "Acil akış durdurulamadı."); g_clear_error(&error);
    } else gtk_label_set_text(GTK_LABEL(state->error), "Acil akıştan dönüş komutu kabul edildi; sonucu yayın durumunda izleyin.");
}

static void
emergency_free(gpointer data)
{
    EmergencyState *state = data;
    g_string_free(state->log, TRUE);
    g_clear_pointer(&state->preparation, g_key_file_unref);
    g_free(state);
}

GtkWidget *
radios_emergency_new(RadioSViewContext *ctx)
{
    EmergencyState *model = g_new0(EmergencyState, 1);
    const gchar *tabs[] = { "Devralma", "Sessizlik", "Duyuru", "Erişim", "Devir" };
    const gchar *ids[] = { "takeover", "silence", "alert", "access", "handover" };
    GtkWidget *page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "emergency-view task-view");
    GtkWidget *command = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 24, "em-command");
    GtkWidget *state = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "em-command-state");
    GtkWidget *middle = radios_ui_box(GTK_ORIENTATION_VERTICAL, 24, "em-command-behavior");
    GtkWidget *steps = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 0, "em-steps");
    GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "em-command-actions");
    GtkWidget *takeover = radios_ui_button("Devralmayı başlat…");
    GtkWidget *chips = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "em-chips");
    GtkWidget *chain = gtk_revealer_new();
    GtkWidget *desk = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 12, "em-desk");
    GtkWidget *main = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "em-main");
    GtkWidget *side = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "em-side");
    GtkWidget *settings = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "em-card em-settings");
    GtkWidget *settings_stack = gtk_stack_new();
    GtkWidget *settings_switcher = gtk_stack_switcher_new();
    GtkWidget *queue = radios_ui_box(GTK_ORIENTATION_VERTICAL, 10, "em-card em-queue-card");
    GtkWidget *queue_stack = gtk_stack_new();
    GtkWidget *queue_switcher = gtk_stack_switcher_new();
    model->ctx = ctx;
    model->page = page;
    model->queue_stack = queue_stack;
    model->settings_stack = settings_stack;
    model->log = g_string_new("RadioS · Acil yayın hazırlığı ve operatör işlem kaydı.\n");
    pack(state, radios_ui_label("ACİL YAYIN", "task-eyebrow"), FALSE);
    model->runtime_status = radios_ui_label("Normal yayın", "em-state");
    gtk_widget_set_name(model->runtime_status, "em-runtime-status");
    gtk_label_set_line_wrap(GTK_LABEL(model->runtime_status), TRUE);
    pack(state, model->runtime_status, FALSE);
    gtk_widget_set_valign(state, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(state, 230, -1);
    pack(command, state, FALSE);
    const gchar *step_names[] = { "Normal", "Hazır", "Acil yayında", "Dönüş" };
    for (guint i = 0; i < G_N_ELEMENTS(step_names); ++i)
    {
        GtkWidget *frame = gtk_frame_new(NULL);
        GtkWidget *label = radios_ui_label(step_names[i], "em-step");
        gtk_widget_set_margin_start(label, 12);
        gtk_widget_set_margin_end(label, 12);
        gtk_widget_set_margin_top(label, 5);
        gtk_widget_set_margin_bottom(label, 5);
        gtk_container_add(GTK_CONTAINER(frame), label);
        pack(steps, frame, FALSE);
    }
    pack(middle, steps, FALSE);
    gtk_widget_set_valign(middle, GTK_ALIGN_CENTER);
    pack(command, middle, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(takeover), "em-takeover");
    gtk_widget_set_name(takeover, "em-takeover-open");
    g_signal_connect(takeover, "clicked", G_CALLBACK(takeover_open), model);
    pack(actions, takeover, FALSE);
    GtkWidget *restore = radios_ui_button("Normal yayına dön");
    GtkWidget *abort = radios_ui_button("Devralmayı iptal et");
    model->restore = restore; model->abort = abort;
    gtk_widget_set_name(restore, "em-takeover-return");
    gtk_widget_set_name(abort, "em-takeover-abort");
    g_signal_connect(restore, "clicked", G_CALLBACK(emergency_return), model);
    g_signal_connect(abort, "clicked", G_CALLBACK(emergency_return), model);
    pack(actions, restore, FALSE); pack(actions, abort, FALSE);
    pack(command, actions, FALSE);
    pack(page, command, FALSE);
    const gchar *chip_names[] = { "Sahiplik", "Yerel kapsam", "Sessizlik", "Yayın zinciri", "Duyuru", "Tatbikat", "Son bilgi" };
    const gchar *chip_values[] = { "—", "Liste boş", "Kapalı", "—", "—", "Yok", "—" };
    const gchar *chip_pages[] = { "takeover", "takeover", "silence", "takeover", "alert", "handover", "takeover" };
    for (guint i = 0; i < G_N_ELEMENTS(chip_names); ++i) {
        GtkWidget *button = gtk_button_new();
        GtkWidget *content = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 9, "em-chip-content");
        GtkWidget *text = radios_ui_box(GTK_ORIENTATION_VERTICAL, 1, "em-chip-text");
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "em-chip");
        if (i == 1)
            gtk_style_context_add_class(gtk_widget_get_style_context(button), "em-critical");
        else if (i == 2 || i == 5)
            gtk_style_context_add_class(gtk_widget_get_style_context(button), "em-warning");
        pack(content, radios_ui_label(i == 1 ? "×" : (i == 2 || i == 5 ? "!" : "−"), "em-chip-icon"), FALSE);
        pack(text, radios_ui_label(chip_names[i], "em-chip-name"), FALSE);
        pack(text, radios_ui_label(chip_values[i], "em-chip-value"), FALSE);
        pack(content, text, TRUE);
        gtk_container_add(GTK_CONTAINER(button), content);
        g_object_set_data(G_OBJECT(button), "em-page", (gpointer)chip_pages[i]);
        if (i == 3)
            g_object_set_data(G_OBJECT(button), "em-chain", chain);
        g_signal_connect(button, "clicked", G_CALLBACK(chip_clicked), settings_stack);
        pack(chips, button, TRUE);
    }
    pack(page, chips, FALSE);
    const gchar *chain_names[] = { "Motor süreci", "Zamanlayıcı", "Oynatıcı ilerlemesi", "Ses çıkışı", "Encoder / akış", "Dış dönüş", "Yayın kaydı", "Veritabanı", "Medya", "Disk", "Bağlantı", "MIDI / GPIO" };
    gtk_style_context_add_class(gtk_widget_get_style_context(chain), "em-chain");
    gtk_container_add(GTK_CONTAINER(chain), facts_new(chain_names, G_N_ELEMENTS(chain_names), 6));
    pack(page, chain, FALSE);
    gtk_stack_set_homogeneous(GTK_STACK(queue_stack), FALSE);
    gtk_stack_add_titled(GTK_STACK(queue_stack), queue_new(model, FALSE), "list", "Acil liste  0");
    gtk_stack_add_titled(GTK_STACK(queue_stack), queue_new(model, TRUE), "queue", "Geçici sıra  0");
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(queue_switcher), GTK_STACK(queue_stack));
    gtk_style_context_add_class(gtk_widget_get_style_context(queue_switcher), "em-seg");
    pack(queue, queue_switcher, FALSE);
    pack(queue, queue_stack, TRUE);
    pack(main, queue, FALSE);
    pack(main, editor_new(model), FALSE);
    GtkWidget *panels[] = { takeover_new(), silence_new(ctx), alert_new(ctx), access_new(ctx), handover_new(ctx) };
    gtk_stack_set_homogeneous(GTK_STACK(settings_stack), FALSE);
    for (guint i = 0; i < G_N_ELEMENTS(tabs); ++i)
        gtk_stack_add_titled(GTK_STACK(settings_stack), panels[i], ids[i], tabs[i]);
    static const gchar *const quiet_required[] = { "Başlangıç", "Dakika", "Gerekçe", NULL };
    static const gchar *const alert_required[] = { "Başlangıç", "Bitiş", "Metin dili", NULL };
    static const gchar *const key_required[] = { "Etiket", "Sahibi", NULL };
    static const gchar *const handover_required[] = { "Devreden", "Devralan", "Son durum", NULL };
    static const gchar *const contact_required[] = { "Ad", "Görev", "Telefon", NULL };
    static const gchar *const drill_required[] = { "Tarih", "Senaryo", "Sonuç", "Algılama · sn", "Karar · sn", "Devralma · sn", "Toparlanma · sn", "Boşluk · ms", NULL };
    bind_draft(model, panels[1], "Ekle", "Planlı sessizlik", quiet_required, "em-quiet-add");
    DraftAction *alert_action = bind_draft(model, panels[2], "Taslağı kaydet", "Duyuru taslağı", alert_required, "em-alert-save");
    replace_action(find_button(panels[2], "Yeni"), G_CALLBACK(draft_new), alert_action, "em-alert-new");
    bind_draft(model, panels[3], "Anahtar ekle", "Anahtar ekle", key_required, "em-key-add");
    bind_draft(model, panels[4], "Devri kaydet", "Devir taslağı", handover_required, "em-handover-save");
    bind_draft(model, panels[4], "Ekle", "İletişim ekle", contact_required, "em-contact-add");
    bind_draft(model, panels[4], "Tatbikatı kaydet", "Tatbikat taslağı", drill_required, "em-drill-save");
    replace_action(find_button(model->queue_pages[1], "Program taslağına aktar"), G_CALLBACK(programme_transfer_clicked), model, "em-queue-export");
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(settings_switcher), GTK_STACK(settings_stack));
    gtk_style_context_add_class(gtk_widget_get_style_context(settings_switcher), "em-seg");
    pack(settings, settings_switcher, FALSE);
    pack(settings, settings_stack, FALSE);
    pack(side, settings, FALSE);
    GtkWidget *log = radios_ui_box(GTK_ORIENTATION_VERTICAL, 10, "em-card em-log");
    GtkWidget *log_head = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "em-card-head");
    pack(log_head, radios_ui_label("İşlem kaydı", "em-card-title"), TRUE);
    GtkWidget *save_log = radios_ui_button("Kaydet");
    GtkWidget *download_log = radios_ui_button("İndir");
    gtk_widget_set_name(save_log, "em-log-save");
    gtk_widget_set_name(download_log, "em-log-download");
    g_signal_connect(save_log, "clicked", G_CALLBACK(preparation_save), model);
    g_signal_connect(download_log, "clicked", G_CALLBACK(export_clicked), model);
    pack(log_head, save_log, FALSE);
    pack(log_head, download_log, FALSE);
    pack(log, log_head, FALSE);
    GtkWidget *log_list = gtk_list_box_new();
    model->log_list = log_list;
    gtk_widget_set_size_request(log_list, -1, 120);
    pack(log, log_list, FALSE);
    pack(side, log, FALSE);
    pack(desk, main, TRUE);
    pack(desk, side, TRUE);
    pack(page, desk, FALSE);
    g_object_set_data(G_OBJECT(page), "em-settings-stack", settings_stack);
    g_object_set_data(G_OBJECT(page), "em-queue-stack", queue_stack);
    g_object_set_data_full(G_OBJECT(page), "em-state", model, emergency_free);
    g_object_set_data(G_OBJECT(page), "em-list", model->lists[0]);
    g_object_set_data(G_OBJECT(page), "em-queue", model->lists[1]);
    g_object_set_data(G_OBJECT(page), "em-editor-title", model->fields[0]);
    g_object_set_data(G_OBJECT(page), "em-editor-duration", model->fields[3]);
    g_object_set_data(G_OBJECT(page), "em-editor-save", find_button(page, "Sıraya ekle"));
    g_signal_connect(page, "destroy", G_CALLBACK(emergency_destroyed), model);
    model->document = radios_data_document_new(ctx, page, "emergency", "Acil hazırlık", emergency_apply, model);
    track_preparation(settings_stack, model);
    model->runtime_timer = g_timeout_add(250, emergency_runtime_tick, model);
    return page;
}

static JsonNode *emergency_configuration(EmergencyState *state)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(node, object);
    JsonArray *queues = json_array_new();
    for (guint i = 0; i < 2; ++i) {
        JsonArray *queue = json_array_new(); GList *rows = gtk_container_get_children(GTK_CONTAINER(state->lists[i]));
        for (GList *row = rows; row; row = row->next) {
            EmergencyItem *item = g_object_get_data(G_OBJECT(row->data), "em-item"); JsonObject *entry = json_object_new(); JsonArray *values = json_array_new();
            for (guint field_index = 0; field_index < 9; ++field_index) json_array_add_string_element(values, item->values[field_index] ? item->values[field_index] : "");
            json_object_set_int_member(entry, "kind", item->kind); json_object_set_array_member(entry, "values", values);
            json_object_set_string_member(entry, "file", item->file ? item->file : "");
            if (item->id) json_object_set_string_member(entry, "id", item->id);
            for (guint k = 0; item->binding[0] && k < G_N_ELEMENTS(binding_keys); ++k)
                json_object_set_string_member(entry, binding_keys[k], item->binding[k]);
            json_array_add_object_element(queue, entry);
        }
        g_list_free(rows); json_array_add_array_element(queues, queue);
    }
    json_object_set_array_member(object, "queues", queues); JsonObject *preparation = json_object_new();
    if (state->preparation) {
        gsize count = 0; gchar **groups = g_key_file_get_groups(state->preparation, &count);
        for (gsize i = 0; i < count; ++i) {
            if (g_str_has_prefix(groups[i], "List") || g_str_has_prefix(groups[i], "Queue")) continue;
            gsize key_count = 0; gchar **keys = g_key_file_get_keys(state->preparation, groups[i], &key_count, NULL);
            for (gsize j = 0; j < key_count; ++j) {
                gchar *key = g_strdup_printf("%s/%s", groups[i], keys[j]); gchar *value = g_key_file_get_string(state->preparation, groups[i], keys[j], NULL);
                json_object_set_string_member(preparation, key, value ? value : ""); g_free(key); g_free(value);
            }
            g_strfreev(keys);
        }
        g_strfreev(groups);
    }
    json_object_set_object_member(object, "preparation", preparation); return node;
}
static void emergency_save(EmergencyState *state)
{
    if (!state->document || state->loading || !radios_data_document_ready(state->document)) return;
    JsonNode *config = emergency_configuration(state); radios_data_document_save(state->document, config); json_node_free(config);
}
static void emergency_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; (void)owner; EmergencyState *state = data;
    JsonNode *current = emergency_configuration(state), *incoming = json_node_new(JSON_NODE_OBJECT); json_node_set_object(incoming, config);
    gboolean identical = json_node_equal(current, incoming); json_node_free(current); json_node_free(incoming);
    if (identical && !g_object_get_data(G_OBJECT(owner), "radios-data-explicit-reload")) {
        if (state->settings_dirty) radios_data_document_dirty(state->document, TRUE);
        gtk_label_set_text(GTK_LABEL(state->error), "Acil hazırlık veritabanında kayıtlı; yayın motoru başlatılmadı."); return;
    }
    state->loading = TRUE; state->editing = NULL;
    JsonArray *queues = json_object_get_array_member(config, "queues");
    for (guint i = 0; i < 2; ++i) {
        GList *rows = gtk_container_get_children(GTK_CONTAINER(state->lists[i]));
        for (GList *row = rows; row; row = row->next) gtk_widget_destroy(row->data);
        g_list_free(rows); JsonArray *queue = json_array_get_array_element(queues, i);
        for (guint j = 0; j < json_array_get_length(queue); ++j) {
            JsonObject *entry = json_array_get_object_element(queue, j); JsonArray *values = json_object_get_array_member(entry, "values");
            EmergencyItem *item = g_new0(EmergencyItem, 1); item->kind = (gint)radios_json_int(entry, "kind", 0);
            for (guint k = 0; k < 9; ++k) item->values[k] = g_strdup(json_array_get_string_element(values, k));
            item->file = g_strdup(radios_json_string(entry, "file", ""));
            if (json_object_has_member(entry, "id")) item->id = g_strdup(radios_json_string(entry, "id", ""));
            for (guint k = 0; k < G_N_ELEMENTS(binding_keys); ++k)
                if (json_object_has_member(entry, binding_keys[k])) item->binding[k] = g_strdup(radios_json_string(entry, binding_keys[k], ""));
            queue_append(state, i, item);
        }
    }
    JsonObject *preparation = json_object_get_object_member(config, "preparation"); GList *keys = json_object_get_members(preparation);
    g_clear_pointer(&state->preparation, g_key_file_unref); state->preparation = g_key_file_new();
    for (GList *key = keys; key; key = key->next) {
        gchar **parts = g_strsplit(key->data, "/", 2);
        if (parts[0] && parts[1]) g_key_file_set_string(state->preparation, parts[0], parts[1], radios_json_string(preparation, key->data, ""));
        g_strfreev(parts);
    }
    g_list_free(keys);
    const gchar *pages[] = {"takeover", "silence", "alert", "access", "handover"};
    for (guint i = 0; i < G_N_ELEMENTS(pages); ++i) {
        GtkWidget *panel = gtk_stack_get_child_by_name(GTK_STACK(state->settings_stack), pages[i]);
        restore_saved_rows(state, panel, pages[i]);
        GHashTable *values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free); gsize count = 0;
        gchar **names = g_key_file_get_keys(state->preparation, pages[i], &count, NULL);
        for (gsize j = 0; j < count; ++j) g_hash_table_insert(values, g_strdup(names[j]), g_key_file_get_string(state->preparation, pages[i], names[j], NULL));
        restore_fields(panel, values);
        g_hash_table_unref(values); g_strfreev(names);
    }
    g_string_assign(state->log, radios_json_string(preparation, "RadioS/draft-log", ""));
    GList *log_rows = gtk_container_get_children(GTK_CONTAINER(state->log_list));
    for (GList *row = log_rows; row; row = row->next) gtk_widget_destroy(row->data);
    g_list_free(log_rows);
    gchar **lines = g_strsplit(state->log->str, "\n", -1);
    for (guint i = 0; lines[i]; ++i) if (*lines[i]) gtk_container_add(GTK_CONTAINER(state->log_list), radios_ui_label(lines[i], "em-log-row"));
    g_strfreev(lines); gtk_widget_show_all(state->log_list);
    queue_updated(state); state->loading = FALSE; state->settings_dirty = FALSE;
    g_object_set_data(G_OBJECT(state->page), "em-preparation-draft", state->preparation);
    gtk_label_set_text(GTK_LABEL(state->error), "Acil hazırlık veritabanından yüklendi; gerçek yayın durumu bilinmiyor.");
}
