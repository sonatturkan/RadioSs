#include "radios/workflow_hub.h"
#include "radios/data.h"
#include "radios/widgets.h"
#include <errno.h>
#include <math.h>
#include <string.h>

typedef struct Hub Hub;
typedef struct Field Field;
struct Field {
    Hub *hub;
    GtkWidget *root, *enabled, *null_value, *control, *body, *variant;
    JsonObject *schema;
    JsonNode *value;
    JsonArray *variants;
    GCancellable *cancel;
    GPtrArray *children;
    gchar *name, *label, *kind;
    gboolean required, generated;
    guint depth;
    guint file_generation;
};
struct Hub {
    RadioSViewContext *ctx;
    GtkWidget *root, *family, *action, *form, *submit, *status, *results, *refresh, *inspect, *download;
    JsonNode *catalogue, *retry;
    JsonObject *history;
    gchar *retry_path;
    GPtrArray *fields;
    RadioSDataClient *identity;
    guint generation;
    gboolean closed, applying, busy, loading;
};
typedef struct {
    guint generation;
    RadioSDataClient *identity;
    GWeakRef field;
    gchar *cursor;
    gchar *record_id;
    gint64 revision;
} Job;

static Field *field_new(Hub *, const gchar *, const gchar *, JsonObject *, gboolean, guint);
static JsonNode *field_value(Field *, GError **);
static void action_selected(GtkComboBox *, gpointer);
static void catalogue_refresh(RadioSViewContext *, GtkWidget *);
static void reference_load(Field *, const gchar *);
static gchar *display_value(JsonNode *);
static void named(GtkWidget *widget, const gchar *name) { gtk_widget_set_name(widget, name); }
static JsonObject *object(JsonObject *parent, const gchar *key)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array(JsonObject *parent, const gchar *key)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static gboolean flag(JsonObject *parent, const gchar *key)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}
static JsonNode *copy(JsonNode *node)
{
    if (!node) return NULL;
    gchar *text = json_to_string(node, FALSE); JsonParser *parser = json_parser_new();
    gboolean ok = json_parser_load_from_data(parser, text, -1, NULL); g_free(text);
    JsonNode *result = ok ? json_parser_steal_root(parser) : NULL; g_object_unref(parser); return result;
}
static void pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static GtkWidget *caption(const gchar *text)
{
    GtkWidget *widget = radios_ui_label(text, "small");
    gtk_label_set_xalign(GTK_LABEL(widget), 0); gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE); return widget;
}
static void clear(GtkWidget *container)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(container));
    for (GList *item = children; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(children);
}
static void status(Hub *hub, const gchar *message) { gtk_label_set_text(GTK_LABEL(hub->status), message); }
static Hub *get(GtkWidget *widget) { return g_object_get_data(G_OBJECT(widget), "radios-workflow-hub"); }
static gboolean current(Hub *hub, Job *job)
{
    if (!hub || hub->closed || job->generation != hub->generation) return FALSE;
    RadioSDataClient *identity = radios_data_client_acquire(hub->ctx);
    gboolean same = radios_data_client_same_identity(identity, job->identity);
    g_clear_pointer(&identity, radios_data_client_unref); return same;
}
static Job *job_new(Hub *hub)
{
    Job *job = g_new0(Job, 1); job->generation = hub->generation;
    job->identity = radios_data_client_acquire(hub->ctx); g_weak_ref_init(&job->field, NULL); return job;
}
static void job_free(gpointer data)
{
    Job *job = data; g_clear_pointer(&job->identity, radios_data_client_unref);
    g_weak_ref_clear(&job->field); g_free(job->cursor); g_free(job->record_id); g_free(job);
}
static JsonObject *selected_action(Hub *hub)
{
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(hub->action));
    JsonArray *families = hub->catalogue ? array(json_node_get_object(hub->catalogue), "families") : NULL;
    for (guint i = 0; id && families && i < json_array_get_length(families); ++i) {
        JsonArray *actions = array(json_array_get_object_element(families, i), "actions");
        for (guint j = 0; actions && j < json_array_get_length(actions); ++j) {
            JsonObject *action = json_array_get_object_element(actions, j);
            if (!g_strcmp0(id, radios_json_string(action, "id", ""))) return action;
        }
    }
    return NULL;
}
static void field_free(gpointer data)
{
    Field *field = data; json_object_unref(field->schema); g_clear_pointer(&field->value, json_node_free);
    if (field->cancel) g_cancellable_cancel(field->cancel);
    g_clear_object(&field->cancel);
    g_clear_pointer(&field->variants, json_array_unref);
    g_ptr_array_unref(field->children); g_free(field->name); g_free(field->label); g_free(field->kind); g_free(field);
}
static void field_toggled(GtkToggleButton *button, gpointer data)
{
    (void)button; Field *field = data;
    gtk_widget_set_sensitive(field->body, (!field->enabled || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field->enabled))) &&
        (!field->null_value || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field->null_value))));
}
static void invalid(Field *field, GError **error, const gchar *reason)
{ g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "%s: %s", field->label, reason); }
static void select_revision(Field *field, const gchar *source, const gchar *id)
{
    JsonNode *records = g_object_get_data(G_OBJECT(field->control), "workflow-records");
    JsonArray *rows = records ? array(json_node_get_object(records), "records") : NULL;
    const gchar *selected = NULL; guint matches = 0;
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i); const gchar *row_id = radios_json_string(row, "_id", "");
        if (!g_strcmp0(row_id, id) || !g_strcmp0(radios_json_string(row, source, ""), id)) { selected = row_id; ++matches; }
    }
    if (matches == 1) gtk_combo_box_set_active_id(GTK_COMBO_BOX(field->control), selected);
}
static void reference_chosen(GtkComboBox *combo, gpointer data)
{
    Field *field = data; Hub *hub = field->hub;
    if (hub->applying) return;
    const gchar *id = gtk_combo_box_get_active_id(combo);
    JsonNode *records = g_object_get_data(G_OBJECT(field->control), "workflow-records");
    JsonArray *rows = records && JSON_NODE_HOLDS_OBJECT(records) ? array(json_node_get_object(records), "records") : NULL;
    JsonObject *chosen = NULL;
    for (guint i = 0; id && rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (!g_strcmp0(id, radios_json_string(row, "_id", ""))) { chosen = row; break; }
    }
    if (!chosen) return;
    for (guint i = 0; i < hub->fields->len; ++i) {
        Field *other = g_ptr_array_index(hub->fields, i);
        const gchar *source = radios_json_string(other->schema, "x-radios-revision-of", "");
        if (!g_strcmp0(source, field->name) || (!*source && !g_strcmp0(other->name, "expected_revision"))) {
            if (GTK_IS_COMBO_BOX(other->control) && !g_strcmp0(radios_json_string(other->schema, "x-radios-reference-value", ""), "_rev")) {
                select_revision(other, field->name, id);
                if (other->enabled) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(other->enabled), TRUE);
            } else if (GTK_IS_ENTRY(other->control)) {
                gchar *revision = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(chosen, "_rev", 0));
                gtk_entry_set_text(GTK_ENTRY(other->control), revision); g_free(revision);
                if (other->enabled) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(other->enabled), TRUE);
            }
        }
    }
}
static void records_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; Job *job = data; Hub *hub = get(owner); if (!current(hub, job)) return;
    GtkWidget *widget = g_weak_ref_get(&job->field);
    Field *field = widget ? g_object_get_data(G_OBJECT(widget), "workflow-field") : NULL;
    if (!field || !g_strcmp0(field->kind, "preset") || !field->control) { g_clear_object(&widget); return; }
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        gtk_widget_set_tooltip_text(field->control, error ? error->message : "Kayıt listesi alınamadı.");
        status(hub, error ? error->message : "Kayıt listesi alınamadı."); g_object_unref(widget); return;
    }
    JsonArray *rows = array(json_node_get_object(result), "records");
    gchar *selected = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field->control)));
    hub->applying = TRUE;
    if (!job->cursor) gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(field->control));
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i); const gchar *id = radios_json_string(row, "_id", "");
        if (!g_uuid_string_is_valid(id)) continue;
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(field->control), id, radios_json_string(row, "label", id));
    }
    if (selected) gtk_combo_box_set_active_id(GTK_COMBO_BOX(field->control), selected);
    hub->applying = FALSE; g_free(selected);
    JsonNode *previous = g_object_get_data(G_OBJECT(field->control), "workflow-records");
    if (job->cursor && previous) {
        JsonArray *stored = array(json_node_get_object(previous), "records");
        for (guint i = 0; stored && rows && i < json_array_get_length(rows); ++i) json_array_add_element(stored, copy(json_array_get_element(rows, i)));
    } else g_object_set_data_full(G_OBJECT(field->control), "workflow-records", copy(result), (GDestroyNotify)json_node_free);
    const gchar *cursor = radios_json_string(json_node_get_object(result), "next_cursor", "");
    if (*cursor) reference_load(field, cursor);
    if (!job->cursor && !g_strcmp0(field->name, "station_id") && !gtk_combo_box_get_active_id(GTK_COMBO_BOX(field->control)))
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(field->control), radios_data_station_id(hub->ctx));
    const gchar *source = radios_json_string(field->schema, "x-radios-revision-of", "");
    for (guint i = 0; *source && i < hub->fields->len; ++i) {
        Field *parent = g_ptr_array_index(hub->fields, i);
        if (!g_strcmp0(parent->name, source) && GTK_IS_COMBO_BOX(parent->control)) {
            const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(parent->control));
            if (id) select_revision(field, source, id);
        }
    }
    g_object_unref(widget);
}
static void reference_load(Field *field, const gchar *cursor)
{
    Hub *hub = field->hub; if (hub->closed) return;
    const gchar *collection = radios_json_string(field->schema, "x-radios-reference", "");
    if (!*collection) return;
    JsonNode *body = radios_data_body(hub->ctx); JsonObject *request = json_node_get_object(body);
    json_object_set_string_member(request, "collection", collection); json_object_set_int_member(request, "limit", 100);
    if (cursor && *cursor) json_object_set_string_member(request, "cursor", cursor);
    Job *job = job_new(hub); job->cursor = g_strdup(cursor); g_weak_ref_set(&job->field, field->root);
    radios_data_request(hub->ctx, hub->root, "POST", "/v1/workflows/desktop-workflows/records", body, records_received, job, job_free);
    json_node_free(body);
}
static void reference_refresh(GtkButton *button, gpointer data) { (void)button; reference_load(data, NULL); }
static JsonObject *concrete_schema(JsonObject *schema)
{
    JsonArray *variants = array(schema, "anyOf");
    for (guint i = 0; variants && i < json_array_get_length(variants); ++i) {
        JsonObject *variant = json_array_get_object_element(variants, i);
        if (g_strcmp0(radios_json_string(variant, "type", ""), "null")) return variant;
    }
    return schema;
}
static void field_remove(GtkButton *button, gpointer data)
{
    Field *parent = data; GtkWidget *row = g_object_get_data(G_OBJECT(button), "workflow-row");
    Field *child = g_object_get_data(G_OBJECT(row), "workflow-field");
    g_ptr_array_remove(parent->children, child); gtk_widget_destroy(row);
}
static void append_child(Field *field, Field *child)
{
    g_ptr_array_add(field->children, child);
    GtkWidget *remove = radios_ui_button("Kaldır");
    g_object_set_data(G_OBJECT(remove), "workflow-row", child->root);
    g_signal_connect(remove, "clicked", G_CALLBACK(field_remove), field); pack(child->root, remove, FALSE);
    pack(field->body, child->root, FALSE); gtk_widget_show_all(child->root);
}
static void item_add(GtkButton *button, gpointer data)
{
    (void)button; Field *field = data; if (field->children->len >= 100) { status(field->hub, "Bu listede en fazla 100 satır düzenlenebilir."); return; }
    JsonObject *items = object(field->schema, "items"); JsonObject *fallback = NULL;
    if (!items) { fallback = json_object_new(); json_object_set_string_member(fallback, "type", "string"); items = fallback; }
    Field *child = field_new(field->hub, "item", "Liste girdisi", items, TRUE, field->depth + 1); append_child(field, child);
    if (fallback) json_object_unref(fallback);
}
static void property_add(GtkButton *button, gpointer data)
{
    (void)button; Field *field = data;
    GtkWidget *entry = g_object_get_data(G_OBJECT(field->root), "workflow-property-name");
    GtkWidget *type = g_object_get_data(G_OBJECT(field->root), "workflow-property-type");
    const gchar *name = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!*name || strlen(name) > 1024 || field->children->len >= 100) { status(field->hub, "Alan adını tamamlayın; en fazla 100 alan eklenebilir."); return; }
    for (guint i = 0; i < field->children->len; ++i) if (!g_strcmp0(((Field *)g_ptr_array_index(field->children, i))->name, name)) { status(field->hub, "Bu alan zaten mevcut."); return; }
    JsonObject *additional = object(concrete_schema(field->schema), "additionalProperties");
    JsonObject *schema = additional && json_object_get_size(additional) ? json_object_ref(additional) : json_object_new();
    if (!additional || !json_object_get_size(additional)) json_object_set_string_member(schema, "type", gtk_combo_box_get_active_id(GTK_COMBO_BOX(type)));
    Field *child = field_new(field->hub, name, name, schema, TRUE, field->depth + 1); json_object_unref(schema); append_child(field, child); gtk_entry_set_text(GTK_ENTRY(entry), "");
}
typedef struct { GWeakRef field; RadioSDataClient *identity; guint generation; gsize maximum; } FileRead;
static void file_read_free(gpointer data)
{
    FileRead *read = data; g_weak_ref_clear(&read->field); g_clear_pointer(&read->identity, radios_data_client_unref); g_free(read);
}
static void file_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    FileRead *read = data; GError *error = NULL; GFileInputStream *stream = g_file_read(G_FILE(source), cancel, &error);
    if (!stream) { g_task_return_error(task, error); return; }
    GByteArray *bytes = g_byte_array_new(); guint8 buffer[16384]; gssize count = 0;
    while ((count = g_input_stream_read(G_INPUT_STREAM(stream), buffer, sizeof(buffer), cancel, &error)) > 0) {
        if (bytes->len + (gsize)count > read->maximum) { g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Kaynak dosya izin verilen boyutu aşıyor."); break; }
        g_byte_array_append(bytes, buffer, (guint)count);
    }
    g_object_unref(stream);
    if (error) { g_byte_array_unref(bytes); g_task_return_error(task, error); return; }
    gchar *encoded = g_base64_encode(bytes->data, bytes->len); g_byte_array_unref(bytes); g_task_return_pointer(task, encoded, g_free);
}
static void file_loaded(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source; (void)data; GTask *task = G_TASK(result); FileRead *read = g_task_get_task_data(task); GError *error = NULL;
    gchar *encoded = g_task_propagate_pointer(task, &error); GtkWidget *widget = g_weak_ref_get(&read->field);
    Field *field = widget ? g_object_get_data(G_OBJECT(widget), "workflow-field") : NULL;
    RadioSDataClient *current_identity = field ? radios_data_client_acquire(field->hub->ctx) : NULL;
    gboolean same = field && !field->hub->closed && field->file_generation == read->generation && radios_data_client_same_identity(current_identity, read->identity);
    g_clear_pointer(&current_identity, radios_data_client_unref);
    if (same) {
        if (error) status(field->hub, error->message);
        else { g_clear_pointer(&field->value, json_node_free); field->value = json_node_new(JSON_NODE_VALUE); json_node_set_string(field->value, encoded); status(field->hub, "Kaynak dosyanın tam içeriği alındı. Önizleme oluşturabilirsiniz."); }
    }
    g_clear_error(&error); g_free(encoded); g_clear_object(&widget);
}
static void file_selected(GtkFileChooserButton *button, gpointer data)
{
    Field *field = data; GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(button));
    if (field->cancel) g_cancellable_cancel(field->cancel);
    g_clear_object(&field->cancel); g_clear_pointer(&field->value, json_node_free); ++field->file_generation;
    if (!file) return;
    field->cancel = g_cancellable_new(); FileRead *read = g_new0(FileRead, 1); g_weak_ref_init(&read->field, field->root);
    read->identity = radios_data_client_acquire(field->hub->ctx); read->generation = field->file_generation;
    read->maximum = (gsize)CLAMP(radios_json_int(field->schema, "x-radios-max-bytes", 1048576), 1, 1048576);
    GTask *task = g_task_new(file, field->cancel, file_loaded, NULL); g_task_set_task_data(task, read, file_read_free); g_task_run_in_thread(task, file_worker); g_object_unref(task); g_object_unref(file);
    status(field->hub, "Kaynak dosya okunuyor…");
}
static void variant_selected(GtkComboBox *combo, gpointer data)
{
    Field *field = data; gint selected = gtk_combo_box_get_active(combo);
    clear(field->body); g_ptr_array_set_size(field->children, 0);
    if (selected < 0 || !field->variants || (guint)selected >= json_array_get_length(field->variants)) return;
    JsonObject *schema = json_array_get_object_element(field->variants, (guint)selected);
    Field *child = field_new(field->hub, field->name, radios_json_string(schema, "title", field->label), schema, TRUE, field->depth+1);
    g_ptr_array_add(field->children, child); pack(field->body, child->root, FALSE); gtk_widget_show_all(child->root);
}
static void preview_clicked(GtkButton *button, gpointer data)
{
    (void)button; Field *field = data; Hub *hub = field->hub;
    gchar *action = g_strdup(radios_json_string(field->schema, "x-radios-result-source", ""));
    gchar **parts = g_strsplit(action, "/", 2);
    if (parts[0] && parts[1]) { gtk_combo_box_set_active_id(GTK_COMBO_BOX(hub->family), parts[0]); gtk_combo_box_set_active_id(GTK_COMBO_BOX(hub->action), action); }
    g_strfreev(parts); g_free(action);
}
static JsonNode *previous_result(Hub *hub, JsonObject *schema)
{
    const gchar *source = radios_json_string(schema, "x-radios-result-source", "");
    JsonObject *previous = object(hub->history, source); JsonNode *result = previous ? json_object_get_member(previous, "result") : NULL;
    JsonArray *path = array(schema, "x-radios-result-path");
    for (guint i = 0; result && path && i < json_array_get_length(path); ++i) {
        if (!JSON_NODE_HOLDS_OBJECT(result)) return NULL;
        result = json_object_get_member(json_node_get_object(result), json_array_get_string_element(path, i));
    }
    return result;
}
static Field *field_new(Hub *hub, const gchar *name, const gchar *label, JsonObject *input, gboolean required, guint depth)
{
    Field *field = g_new0(Field, 1); field->hub = hub; field->name = g_strdup(name); field->label = g_strdup(label);
    field->schema = json_object_ref(input); field->required = required; field->children = g_ptr_array_new(); field->depth = depth;
    field->root = radios_ui_box(GTK_ORIENTATION_VERTICAL, 5, NULL);
    gchar *widget_name = g_strconcat("workflow-field-", name, NULL); named(field->root, widget_name); g_free(widget_name);
    g_object_set_data_full(G_OBJECT(field->root), "workflow-field", field, field_free);
    GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
    if (!required) {
        field->enabled = gtk_check_button_new_with_label(label); pack(heading, field->enabled, FALSE);
        gchar *enabled_name = g_strconcat("workflow-use-", name, NULL); named(field->enabled, enabled_name); g_free(enabled_name);
        g_signal_connect(field->enabled, "toggled", G_CALLBACK(field_toggled), field);
    } else pack(heading, caption(label), FALSE);
    pack(field->root, heading, FALSE); field->body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 5, NULL); pack(field->root, field->body, FALSE);
    JsonArray *nullable = array(input, "anyOf");
    for (guint i = 0; nullable && i < json_array_get_length(nullable); ++i) {
        if (!g_strcmp0(radios_json_string(json_array_get_object_element(nullable, i), "type", ""), "null")) {
            field->null_value = gtk_check_button_new_with_label("Boş değer olarak temizle"); pack(heading, field->null_value, FALSE);
            gchar *null_name = g_strconcat("workflow-null-", name, NULL); named(field->null_value, null_name); g_free(null_name);
            g_signal_connect(field->null_value, "toggled", G_CALLBACK(field_toggled), field); break;
        }
    }
    if (*radios_json_string(input, "x-radios-result-source", "")) {
        field->kind = g_strdup("preset"); field->value = copy(previous_result(hub, input));
        if (field->value && !g_strcmp0(radios_json_string(input, "x-radios-result-transform", ""), "item_revision_map")) {
            JsonArray *items = JSON_NODE_HOLDS_ARRAY(field->value) ? json_node_get_array(field->value) : NULL;
            JsonObject *mapping = json_object_new(); gboolean valid = items != NULL;
            for (guint i = 0; valid && i < json_array_get_length(items); ++i) {
                JsonObject *row = json_array_get_object_element(items, i); const gchar *id = radios_json_string(row, "item_id", "");
                gint64 revision = radios_json_int(row, "item_revision", 0); valid = g_uuid_string_is_valid(id) && revision > 0;
                if (valid) json_object_set_int_member(mapping, id, revision);
            }
            g_clear_pointer(&field->value, json_node_free);
            if (valid) { field->value = json_node_new(JSON_NODE_OBJECT); json_node_take_object(field->value, mapping); }
            else json_object_unref(mapping);
        }
        if (field->value) { gchar *text = display_value(field->value); pack(field->body, caption(text), FALSE); g_free(text); }
        else { GtkWidget *preview = radios_ui_button("Önce önizlemeyi çalıştır"); pack(field->body, preview, FALSE); g_signal_connect(preview, "clicked", G_CALLBACK(preview_clicked), field); }
        if (field->enabled) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field->enabled), field->value != NULL);
        return field;
    }
    if (!g_strcmp0(radios_json_string(input, "format", ""), "file-base64")) {
        field->kind = g_strdup("file"); field->control = gtk_file_chooser_button_new("Kaynak dosyayı seç", GTK_FILE_CHOOSER_ACTION_OPEN);
        gchar *control_name = g_strconcat("workflow-input-", name, NULL); named(field->control, control_name); g_free(control_name);
        GtkFileFilter *filter = gtk_file_filter_new(); gtk_file_filter_set_name(filter, "Desteklenen aktarım dosyaları"); JsonArray *extensions = array(input, "x-radios-file-extensions");
        for (guint i = 0; extensions && i < json_array_get_length(extensions); ++i) { gchar *pattern = g_strconcat("*.", json_array_get_string_element(extensions, i), NULL); gtk_file_filter_add_pattern(filter, pattern); g_free(pattern); }
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(field->control), filter); pack(field->body, field->control, FALSE); g_signal_connect(field->control, "file-set", G_CALLBACK(file_selected), field);
        if (field->enabled) gtk_widget_set_sensitive(field->body, FALSE);
        return field;
    }
    JsonArray *variants = array(input, "oneOf"); if (!variants) variants = array(input, "anyOf");
    JsonArray *concrete = json_array_new();
    for (guint i = 0; variants && i < json_array_get_length(variants); ++i) {
        JsonObject *option = json_array_get_object_element(variants, i);
        if (g_strcmp0(radios_json_string(option, "type", ""), "null")) json_array_add_element(concrete, copy(json_array_get_element(variants, i)));
    }
    if (!json_object_get_size(input)) {
        const gchar *kinds[] = {"string", "integer", "number", "boolean", "array", "object"}, *titles[] = {"Metin", "Tam sayı", "Sayı", "Açık / kapalı", "Liste", "Ayar grubu"};
        for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) { JsonObject *option = json_object_new(); json_object_set_string_member(option, "type", kinds[i]); json_object_set_string_member(option, "title", titles[i]); json_array_add_object_element(concrete, option); }
    }
    if (json_array_get_length(concrete) > 1 && depth < 8 && !*radios_json_string(input, "x-radios-reference", "")) {
        field->variants = concrete; field->kind = g_strdup("variant"); field->variant = gtk_combo_box_text_new();
        gchar *variant_name = g_strconcat("workflow-choice-", name, NULL); named(field->variant, variant_name); g_free(variant_name);
        for (guint i = 0; i < json_array_get_length(concrete); ++i) {
            JsonObject *option = json_array_get_object_element(concrete, i); gchar *id = g_strdup_printf("%u", i);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(field->variant), id, radios_json_string(option, "title", radios_json_string(option, "type", "Seçenek"))); g_free(id);
        }
        pack(heading, field->variant, FALSE); g_signal_connect(field->variant, "changed", G_CALLBACK(variant_selected), field);
        gtk_combo_box_set_active(GTK_COMBO_BOX(field->variant), 0);
        gtk_widget_set_sensitive(field->body, required);
        return field;
    }
    json_array_unref(concrete);
    JsonObject *schema = concrete_schema(input); const gchar *kind = radios_json_string(schema, "type", "string");
    field->kind = g_strdup(kind); JsonArray *choices = array(schema, "enum");
    const gchar *reference = radios_json_string(input, "x-radios-reference", "");
    if (choices) {
        g_free(field->kind); field->kind = g_strdup("enum"); field->control = gtk_combo_box_text_new();
        for (guint i = 0; i < json_array_get_length(choices); ++i) {
            JsonNode *choice = json_array_get_element(choices, i); gchar *text = JSON_NODE_HOLDS_VALUE(choice) && json_node_get_value_type(choice) == G_TYPE_STRING ? g_strdup(json_node_get_string(choice)) : json_to_string(choice, FALSE);
            gchar *id = g_strdup_printf("%u", i); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(field->control), id, text); g_free(id); g_free(text);
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(field->control), 0); pack(field->body, field->control, FALSE);
    } else if (*reference) {
        field->control = gtk_combo_box_text_new(); pack(field->body, field->control, FALSE);
        g_signal_connect(field->control, "changed", G_CALLBACK(reference_chosen), field);
        GtkWidget *reload = radios_ui_button("Kayıtları yenile"); pack(heading, reload, FALSE); g_signal_connect(reload, "clicked", G_CALLBACK(reference_refresh), field);
        gchar *refresh_name = g_strconcat("workflow-refresh-", name, NULL); named(reload, refresh_name); g_free(refresh_name);
        reference_load(field, NULL);
    } else if (!g_strcmp0(kind, "boolean")) {
        field->control = gtk_check_button_new_with_label("Etkin"); pack(field->body, field->control, FALSE);
    } else if ((!g_strcmp0(kind, "object") || !g_strcmp0(kind, "array")) && depth < 8) {
        if (!g_strcmp0(kind, "array")) {
            GtkWidget *add = radios_ui_button("Listeye satır ekle"); pack(heading, add, FALSE); g_signal_connect(add, "clicked", G_CALLBACK(item_add), field);
            gchar *add_name = g_strconcat("workflow-add-", name, NULL); named(add, add_name); g_free(add_name);
        } else {
            JsonObject *properties = object(schema, "properties"); JsonArray *must = array(schema, "required"); GList *keys = properties ? json_object_get_members(properties) : NULL;
            for (GList *key = keys; key; key = key->next) {
                gboolean child_required = FALSE;
                for (guint i = 0; must && i < json_array_get_length(must); ++i) if (!g_strcmp0(key->data, json_array_get_string_element(must, i))) child_required = TRUE;
                JsonObject *child_schema = object(properties, key->data); if (!child_schema) continue;
                Field *child = field_new(hub, key->data, radios_json_string(child_schema, "title", key->data), child_schema, child_required, depth+1);
                g_ptr_array_add(field->children, child); pack(field->body, child->root, FALSE);
            }
            g_list_free(keys);
            if (!properties || json_object_get_size(properties) == 0 || flag(schema, "additionalProperties")) {
                GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, NULL), *entry = radios_ui_entry(!g_strcmp0(name, "path_bindings") ? "Kaynak dosya yolu" : "Alan adı"), *type = gtk_combo_box_text_new();
                const gchar *kinds[] = {"string", "integer", "number", "boolean", "array", "object"}, *titles[] = {"Metin", "Tam sayı", "Sayı", "Açık / kapalı", "Liste", "Ayar grubu"};
                for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(type), kinds[i], titles[i]);
                gtk_combo_box_set_active(GTK_COMBO_BOX(type), 0); GtkWidget *add = radios_ui_button("Alan ekle");
                JsonObject *additional = object(schema, "additionalProperties"); if (additional && json_object_get_size(additional)) gtk_widget_set_sensitive(type, FALSE);
                pack(row, entry, TRUE); pack(row, type, FALSE); pack(row, add, FALSE); pack(field->body, row, FALSE);
                g_object_set_data(G_OBJECT(field->root), "workflow-property-name", entry); g_object_set_data(G_OBJECT(field->root), "workflow-property-type", type);
                g_signal_connect(add, "clicked", G_CALLBACK(property_add), field);
            }
        }
    } else if (!g_strcmp0(kind, "object") || !g_strcmp0(kind, "array")) {
        g_free(field->kind); field->kind = g_strdup("unsupported");
        pack(field->body, caption("Bu ayar grubu çok derin; en fazla sekiz düzey düzenlenebilir."), FALSE);
    } else {
        field->control = radios_ui_entry(NULL); pack(field->body, field->control, FALSE);
        const gchar *format = radios_json_string(schema, "format", "");
        if (!g_strcmp0(format, "date")) gtk_entry_set_placeholder_text(GTK_ENTRY(field->control), "YYYY-AA-GG");
        else if (!g_strcmp0(format, "date-time")) gtk_entry_set_placeholder_text(GTK_ENTRY(field->control), "2026-10-15T12:00:00Z");
    }
    if (field->control) { gchar *control_name = g_strconcat("workflow-input-", name, NULL); named(field->control, control_name); g_free(control_name); }
    JsonNode *value = json_object_get_member(input, "default"); if (!value) value = json_object_get_member(schema, "default");
    if (value && field->control) {
        if (GTK_IS_ENTRY(field->control)) {
            gchar *text = JSON_NODE_HOLDS_VALUE(value) && json_node_get_value_type(value) == G_TYPE_STRING ? g_strdup(json_node_get_string(value)) : json_to_string(value, FALSE);
            gtk_entry_set_text(GTK_ENTRY(field->control), text); g_free(text);
        } else if (GTK_IS_TOGGLE_BUTTON(field->control) && JSON_NODE_HOLDS_VALUE(value)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field->control), json_node_get_boolean(value));
        else if (choices) for (guint i = 0; i < json_array_get_length(choices); ++i) if (json_node_equal(value, json_array_get_element(choices, i))) gtk_combo_box_set_active(GTK_COMBO_BOX(field->control), (gint)i);
    }
    if (field->enabled) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field->enabled), value != NULL);
    gtk_widget_set_sensitive(field->body, required || value != NULL);
    return field;
}
static JsonNode *field_value(Field *field, GError **error)
{
    if (field->enabled && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field->enabled))) return NULL;
    if (field->null_value && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field->null_value))) return json_node_new(JSON_NODE_NULL);
    if (!g_strcmp0(field->kind, "preset")) {
        if (!field->value) { invalid(field, error, "önce ilgili önizleme işlemini tamamlayın."); return NULL; }
        return copy(field->value);
    }
    if (!g_strcmp0(field->kind, "file")) {
        if (!field->value) { invalid(field, error, "dosyayı seçin ve okumanın tamamlanmasını bekleyin."); return NULL; }
        return copy(field->value);
    }
    if (!g_strcmp0(field->kind, "unsupported")) { invalid(field, error, "ayar ağacı sekiz düzeyi aşamaz."); return NULL; }
    if (!g_strcmp0(field->kind, "variant")) {
        if (!field->children->len) { invalid(field, error, "bir veri türü seçin."); return NULL; }
        return field_value(g_ptr_array_index(field->children, 0), error);
    }
    JsonObject *schema = concrete_schema(field->schema); JsonNode *value = json_node_new(JSON_NODE_VALUE);
    if (!g_strcmp0(field->kind, "enum")) {
        gint choice = gtk_combo_box_get_active(GTK_COMBO_BOX(field->control)); JsonArray *values = array(schema, "enum"); json_node_free(value);
        if (choice < 0 || !values || (guint)choice >= json_array_get_length(values)) { invalid(field, error, "bir seçenek belirleyin."); return NULL; }
        return copy(json_array_get_element(values, (guint)choice));
    }
    const gchar *reference = radios_json_string(field->schema, "x-radios-reference", "");
    if (*reference) {
        const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(field->control));
        if (!id || !g_uuid_string_is_valid(id)) { invalid(field, error, "kayıt listesinden seçim yapın."); json_node_free(value); return NULL; }
        const gchar *reference_value = radios_json_string(field->schema, "x-radios-reference-value", "_id");
        if (g_strcmp0(reference_value, "_id")) {
            JsonNode *records = g_object_get_data(G_OBJECT(field->control), "workflow-records");
            JsonArray *rows = records ? array(json_node_get_object(records), "records") : NULL; JsonNode *selected = NULL;
            for (guint i = 0; rows && i < json_array_get_length(rows); ++i) { JsonObject *row = json_array_get_object_element(rows, i); if (!g_strcmp0(id, radios_json_string(row, "_id", ""))) selected = json_object_get_member(row, reference_value); }
            if (!selected || JSON_NODE_HOLDS_NULL(selected)) { invalid(field, error, "güncel kaydı yeniden yükleyin."); json_node_free(value); return NULL; }
            json_node_free(value); value = copy(selected);
        } else json_node_set_string(value, id);
        return value;
    }
    if (!g_strcmp0(field->kind, "object") || !g_strcmp0(field->kind, "array")) {
        gboolean list = !g_strcmp0(field->kind, "array"); json_node_free(value); value = json_node_new(list ? JSON_NODE_ARRAY : JSON_NODE_OBJECT);
        if (list) json_node_take_array(value, json_array_new()); else json_node_take_object(value, json_object_new());
        for (guint i = 0; i < field->children->len; ++i) {
            Field *child = g_ptr_array_index(field->children, i); JsonNode *result = field_value(child, error);
            if (error && *error) { json_node_free(value); return NULL; }
            if (!result) continue;
            if (list) json_array_add_element(json_node_get_array(value), result); else json_object_set_member(json_node_get_object(value), child->name, result);
        }
        if (list) {
            JsonArray *rows = json_node_get_array(value); guint count = json_array_get_length(rows);
            if ((json_object_has_member(schema, "minItems") && count < (guint)radios_json_int(schema, "minItems", 0)) ||
                (json_object_has_member(schema, "maxItems") && count > (guint)radios_json_int(schema, "maxItems", 100))) {
                invalid(field, error, "listede izin verilen sayıda satır olmalı."); json_node_free(value); return NULL;
            }
            if (flag(schema, "uniqueItems")) for (guint i = 0; i < count; ++i) for (guint j = 0; j < i; ++j)
                if (json_node_equal(json_array_get_element(rows, i), json_array_get_element(rows, j))) { invalid(field, error, "aynı satır birden fazla seçilemez."); json_node_free(value); return NULL; }
        }
        return value;
    }
    if (!g_strcmp0(field->kind, "boolean")) { json_node_set_boolean(value, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field->control))); return value; }
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(field->control));
    if (!g_strcmp0(field->kind, "integer") || !g_strcmp0(field->kind, "number")) {
        gchar *end = NULL; errno = 0;
        if (!g_strcmp0(field->kind, "integer")) {
            gint64 integer = g_ascii_strtoll(text, &end, 10);
            if (errno || !*text || !end || *end) goto number_error;
            json_node_set_int(value, integer);
        } else {
            gdouble number = g_ascii_strtod(text, &end);
            if (errno || !*text || !end || *end || !isfinite(number)) goto number_error;
            json_node_set_double(value, number);
        }
        gdouble number = json_node_get_double(value);
        if ((json_object_has_member(schema, "minimum") && number < json_object_get_double_member(schema, "minimum")) ||
            (json_object_has_member(schema, "maximum") && number > json_object_get_double_member(schema, "maximum"))) goto number_error;
        return value;
    }
    if (field->required && !*text) { invalid(field, error, "bu alanı doldurun."); json_node_free(value); return NULL; }
    json_node_set_string(value, text); return value;
number_error:
    invalid(field, error, "izin verilen aralıkta geçerli bir sayı girin."); json_node_free(value); return NULL;
}
static gchar *display_value(JsonNode *node)
{
    if (!node || JSON_NODE_HOLDS_NULL(node)) return g_strdup("—");
    if (JSON_NODE_HOLDS_OBJECT(node)) return g_strdup("Ayrıntılar");
    if (JSON_NODE_HOLDS_ARRAY(node)) return g_strdup_printf("%u kayıt", json_array_get_length(json_node_get_array(node)));
    if (json_node_get_value_type(node) == G_TYPE_STRING) {
        const gchar *text = json_node_get_string(node);
        if (g_utf8_strlen(text, -1) > 256) { gchar *part = g_utf8_substring(text, 0, 256); gchar *result = g_strconcat(part, "…", NULL); g_free(part); return result; }
        return g_strdup(text);
    }
    if (json_node_get_value_type(node) == G_TYPE_BOOLEAN) return g_strdup(json_node_get_boolean(node) ? "Evet" : "Hayır");
    return json_to_string(node, FALSE);
}
static void result_append(GtkTreeStore *store, GtkTreeIter *parent, const gchar *name, JsonNode *node, guint depth)
{
    if (depth > 8 || !g_ascii_strcasecmp(name, "token") || strstr(name, "secret") || strstr(name, "password")) return;
    GtkTreeIter row; gchar *value = display_value(node); gtk_tree_store_append(store, &row, parent);
    gtk_tree_store_set(store, &row, 0, name, 1, value, -1); g_free(value);
    if (node && JSON_NODE_HOLDS_OBJECT(node)) {
        JsonObject *record = json_node_get_object(node); GList *keys = json_object_get_members(record);
        for (GList *key = keys; key; key = key->next) result_append(store, &row, key->data, json_object_get_member(record, key->data), depth+1);
        g_list_free(keys);
    } else if (node && JSON_NODE_HOLDS_ARRAY(node)) {
        JsonArray *rows = json_node_get_array(node);
        for (guint i = 0; i < MIN(json_array_get_length(rows), 1000u); ++i) { gchar *title = g_strdup_printf("Kayıt %u", i+1); result_append(store, &row, title, json_array_get_element(rows, i), depth+1); g_free(title); }
    }
}
static void show_result(Hub *hub, JsonNode *result, const gchar *title)
{
    GtkTreeStore *store = gtk_tree_store_new(2, G_TYPE_STRING, G_TYPE_STRING); result_append(store, NULL, title, result, 0);
    gtk_tree_view_set_model(GTK_TREE_VIEW(hub->results), GTK_TREE_MODEL(store)); g_object_unref(store);
    GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1); gtk_tree_view_expand_to_path(GTK_TREE_VIEW(hub->results), path); gtk_tree_path_free(path);
}
static void result_read(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; Job *job = data; Hub *hub = get(owner); if (!current(hub, job)) return;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { status(hub, error ? error->message : "Kayıt yeniden okunamadı; işlem alındısı korunuyor."); return; }
    JsonArray *rows = array(json_node_get_object(result), "records");
    if (job->record_id) {
        JsonObject *record = rows && json_array_get_length(rows) == 1 ? json_array_get_object_element(rows, 0) : NULL;
        if (!record || g_strcmp0(radios_json_string(record, "_id", ""), job->record_id) || radios_json_int(record, "_rev", 0) < job->revision) {
            status(hub, "İşlem alındı fakat kayıt sürümü yeniden doğrulanamadı; kayıtları yenileyin."); return;
        }
        g_object_set_data_full(G_OBJECT(hub->root), "workflow-readback", copy(result), (GDestroyNotify)json_node_free);
        status(hub, "İşlem tamamlandı; kayıt veritabanından yeniden okunarak doğrulandı.");
    } else status(hub, "Güncel kayıtlar getirildi. Belirsiz işlemi yeniden göndermeden önce bu kayıtları kontrol edin.");
    show_result(hub, result, "Güncel kayıtlar");
}
static void inspect_result(GtkButton *button, gpointer data)
{
    (void)button; Hub *hub = data; JsonObject *action = selected_action(hub); if (!action || hub->closed) return;
    const gchar *collection = radios_json_string(action, "result_collection", "");
    if (!*collection) { status(hub, "Bu işlem için sonuç tablosundaki alındıyı ve ilgili kayıt ekranını kullanın."); return; }
    JsonNode *body = radios_data_body(hub->ctx); json_object_set_string_member(json_node_get_object(body), "collection", collection);
    JsonObject *saved = object(hub->history, radios_json_string(action, "id", ""));
    JsonObject *last_request = hub->retry ? json_node_get_object(hub->retry) : object(saved, "request");
    const gchar *target_station = radios_json_string(last_request, "station_id", "");
    if (*target_station) json_object_set_string_member(json_node_get_object(body), "station_id", target_station);
    Job *job = job_new(hub); radios_data_request(hub->ctx, hub->root, "POST", "/v1/workflows/desktop-workflows/records", body, result_read, job, job_free); json_node_free(body);
}
typedef struct { GWeakRef hub; gchar *content; } Download;
static void download_free(gpointer data)
{ Download *download = data; g_weak_ref_clear(&download->hub); g_free(download->content); g_free(download); }
static void downloaded(GtkDialog *dialog, gint response, gpointer data)
{
    Download *download = data; GtkWidget *owner = g_weak_ref_get(&download->hub); Hub *hub = owner ? get(owner) : NULL;
    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)); GError *error = NULL;
        if (path && g_file_set_contents(path, download->content, -1, &error)) { if (hub && !hub->closed) status(hub, "Aktarım dosyası kaydedildi."); }
        else { if (hub && !hub->closed) status(hub, error ? error->message : "Dosya adı seçin."); g_clear_error(&error); g_free(path); g_clear_object(&owner); return; }
        g_free(path);
    }
    g_clear_object(&owner); gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void download_result(GtkButton *button, gpointer data)
{
    (void)button; Hub *hub = data; JsonObject *action = selected_action(hub), *download = object(action, "download");
    JsonNode *result = g_object_get_data(G_OBJECT(hub->root), "workflow-last-result");
    JsonObject *record = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    const gchar *content = radios_json_string(record, radios_json_string(download, "field", "content"), NULL);
    if (!download || !content) return;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Aktarım dosyasını kaydet", hub->ctx->parent, GTK_FILE_CHOOSER_ACTION_SAVE,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL); named(dialog, "workflow-download-dialog");
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE); gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE); gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    radios_ui_file_chooser_default_folder(GTK_FILE_CHOOSER(dialog), hub->ctx);
    gchar *filename = radios_ui_file_name_suggestion(radios_json_string(record, "filename", radios_json_string(download, "filename", "radios-aktarim.txt")), "radios-aktarim.txt");
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), filename); g_free(filename);
    Download *job = g_new0(Download, 1); g_weak_ref_init(&job->hub, hub->root); job->content = g_strdup(content);
    g_object_set_data_full(G_OBJECT(dialog), "workflow-download", job, download_free); g_signal_connect(dialog, "response", G_CALLBACK(downloaded), job);
    g_signal_connect_object(hub->root, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED); gtk_widget_show(dialog);
}
static void submitted(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; Hub *hub = get(owner); Job *job = data; if (!current(hub, job)) return;
    hub->busy = FALSE; gtk_widget_set_sensitive(hub->submit, TRUE);
    gboolean uncertain = error && error->domain == RADIOS_DATA_CLIENT_ERROR &&
        (error->code == RADIOS_DATA_CLIENT_ERROR_TRANSPORT || error->code == RADIOS_DATA_CLIENT_ERROR_TIMEOUT ||
         error->code == RADIOS_DATA_CLIENT_ERROR_COMMITTED || error->code == RADIOS_DATA_CLIENT_ERROR_UNAVAILABLE || error->code == RADIOS_DATA_CLIENT_ERROR_INVALID_RESPONSE);
    if (uncertain) {
        JsonObject *action = selected_action(hub), *properties = object(object(action, "request_schema"), "properties");
        gboolean idempotent = flag(action, "idempotent") || (properties && json_object_has_member(properties, "idempotency_key"));
        gtk_widget_set_sensitive(hub->submit, idempotent);
        gtk_button_set_label(GTK_BUTTON(hub->submit), "Aynı işlemi yeniden dene");
        status(hub, idempotent ? "İşlemin sonucu doğrulanamadı. Aynı işlem kimliğiyle yeniden deneyebilir veya güncel kayıtları kontrol edebilirsiniz." :
            "İşlemin sonucu doğrulanamadı. Yinelenen kayıt oluşturmamak için yeniden gönderim durduruldu; Güncel kayıtları getir ile sonucu kontrol edin."); return;
    }
    if (!error && result) {
        const gchar *action_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(hub->action));
        JsonObject *previous = json_object_new(); json_object_set_member(previous, "request", copy(hub->retry)); json_object_set_member(previous, "result", copy(result));
        json_object_set_object_member(hub->history, action_id, previous);
    }
    g_clear_pointer(&hub->retry, json_node_free); g_clear_pointer(&hub->retry_path, g_free);
    gtk_widget_set_sensitive(hub->form, TRUE); gtk_widget_set_sensitive(hub->family, TRUE); gtk_widget_set_sensitive(hub->action, TRUE);
    gtk_button_set_label(GTK_BUTTON(hub->submit), "İşlemi uygula");
    if (error || !result) { status(hub, error ? error->message : "Geçerli sonuç alınamadı."); return; }
    show_result(hub, result, "İşlem sonucu");
    g_object_set_data_full(G_OBJECT(hub->root), "workflow-last-result", copy(result), (GDestroyNotify)json_node_free);
    gtk_widget_set_sensitive(hub->download, object(selected_action(hub), "download") != NULL);
    status(hub, "İşlem sonucu veritabanından alındı. Kayıt listeleri güncelleniyor.");
    JsonObject *action = selected_action(hub); const gchar *collection = radios_json_string(action, "result_collection", "");
    const gchar *id = JSON_NODE_HOLDS_OBJECT(result) ? radios_json_string(json_node_get_object(result), radios_json_string(action, "result_id_field", "_id"), "") : "";
    if (*collection && g_uuid_string_is_valid(id)) {
        JsonNode *body = radios_data_body(hub->ctx); JsonObject *request = json_node_get_object(body);
        json_object_set_string_member(request, "collection", collection); json_object_set_string_member(request, "record_id", id);
        JsonObject *saved = object(hub->history, radios_json_string(action, "id", "")); const gchar *target_station = radios_json_string(object(saved, "request"), "station_id", "");
        if (*target_station) json_object_set_string_member(request, "station_id", target_station);
        Job *read = job_new(hub); read->record_id = g_strdup(id); read->revision = radios_json_int(json_node_get_object(result), "_rev", 0);
        radios_data_request(hub->ctx, hub->root, "POST", "/v1/workflows/desktop-workflows/records", body, result_read, read, job_free); json_node_free(body);
    }
    for (guint i = 0; i < hub->fields->len; ++i) { Field *field = g_ptr_array_index(hub->fields, i); if (*radios_json_string(field->schema, "x-radios-reference", "")) reference_load(field, NULL); }
    radios_data_refresh(hub->ctx);
}
static void submit(GtkButton *button, gpointer data)
{
    (void)button; Hub *hub = data; if (hub->closed || hub->busy) return;
    RadioSDataClient *identity = radios_data_client_acquire(hub->ctx);
    gboolean same = radios_data_client_same_identity(identity, hub->identity); g_clear_pointer(&identity, radios_data_client_unref);
    if (!same) { status(hub, "Veri oturumu değişti. İşlemi yeni oturumda incelemek için alanları yenileyin."); return; }
    JsonObject *action = selected_action(hub); if (!action) return;
    if (!hub->retry) {
        JsonNode *body = radios_data_body(hub->ctx); JsonObject *request = json_node_get_object(body); GError *error = NULL;
        JsonObject *properties = object(object(action, "request_schema"), "properties");
        if (!properties || !json_object_has_member(properties, "station_id")) json_object_remove_member(request, "station_id");
        for (guint i = 0; i < hub->fields->len; ++i) {
            Field *field = g_ptr_array_index(hub->fields, i); JsonNode *value = field_value(field, &error);
            if (error) break;
            if (value) json_object_set_member(request, field->name, value);
        }
        if (error) { status(hub, error->message); g_clear_error(&error); json_node_free(body); return; }
        JsonArray *fields = array(action, "fields");
        JsonObject *prefill = object(hub->history, radios_json_string(action, "prefill_from_action", ""));
        JsonObject *previous_request = prefill ? object(prefill, "request") : NULL;
        for (guint i = 0; fields && i < json_array_get_length(fields); ++i) {
            JsonObject *field = json_array_get_object_element(fields, i), *schema = object(field, "schema"); const gchar *name = radios_json_string(field, "name", "");
            if (flag(schema, "x-radios-generated")) {
                JsonNode *previous_key = previous_request ? json_object_get_member(previous_request, name) : NULL;
                if (previous_key) json_object_set_member(request, name, copy(previous_key));
                else { gchar *id = g_uuid_string_random(); json_object_set_string_member(request, name, id); g_free(id); }
            }
            if (!g_strcmp0(name, "source_station_id")) json_object_set_string_member(request, name, radios_data_station_id(hub->ctx));
        }
        hub->retry = body; hub->retry_path = g_strdup(radios_json_string(action, "path", ""));
    }
    if (!g_str_has_prefix(hub->retry_path, "/v1/workflows/")) return;
    g_object_set_data(G_OBJECT(hub->root), "workflow-last-result", NULL); g_object_set_data(G_OBJECT(hub->root), "workflow-readback", NULL);
    hub->busy = TRUE; gtk_widget_set_sensitive(hub->submit, FALSE); gtk_widget_set_sensitive(hub->form, FALSE);
    gtk_widget_set_sensitive(hub->family, FALSE); gtk_widget_set_sensitive(hub->action, FALSE);
    status(hub, "İşlem veritabanında doğrulanıyor…"); Job *job = job_new(hub);
    if (!radios_data_request(hub->ctx, hub->root, "POST", hub->retry_path, hub->retry, submitted, job, job_free)) {
        hub->busy = FALSE; gtk_widget_set_sensitive(hub->submit, TRUE); status(hub, "Bağlantı kurulamadı. Girdiler korunuyor; aynı işlemi yeniden deneyin.");
    }
}
static void action_selected(GtkComboBox *combo, gpointer data)
{
    (void)combo; Hub *hub = data; if (hub->applying || hub->busy || hub->retry) return;
    ++hub->generation; clear(hub->form); g_ptr_array_set_size(hub->fields, 0);
    g_object_set_data(G_OBJECT(hub->root), "workflow-last-result", NULL); g_object_set_data(G_OBJECT(hub->root), "workflow-readback", NULL);
    gtk_widget_set_sensitive(hub->download, FALSE);
    JsonObject *action = selected_action(hub); JsonArray *fields = array(action, "fields");
    JsonObject *previous = object(hub->history, radios_json_string(action, "prefill_from_action", "")); JsonObject *previous_request = previous ? object(previous, "request") : NULL;
    for (guint i = 0; fields && i < json_array_get_length(fields); ++i) {
        JsonObject *descriptor = json_array_get_object_element(fields, i); const gchar *name = radios_json_string(descriptor, "name", ""); JsonObject *schema = object(descriptor, "schema");
        gboolean station_target = g_str_has_prefix(radios_json_string(action, "id", ""), "stations/");
        if (!schema || !*name || (!g_strcmp0(name, "station_id") && !station_target) || !g_strcmp0(name, "source_station_id") || flag(schema, "x-radios-generated")) continue;
        Field *field = field_new(hub, name, radios_json_string(descriptor, "label", name), schema, flag(descriptor, "required"), 0);
        JsonNode *preset = previous_request ? json_object_get_member(previous_request, name) : NULL;
        if (preset && !field->value) {
            field->value = copy(preset); g_free(field->kind); field->kind = g_strdup("preset"); clear(field->body); field->control = NULL;
            gchar *text = !g_strcmp0(radios_json_string(schema, "format", ""), "file-base64") ? g_strdup("Önizlemede seçilen kaynak dosya") : display_value(preset); pack(field->body, caption(text), FALSE); g_free(text);
            if (field->enabled) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field->enabled), TRUE);
        }
        g_ptr_array_add(hub->fields, field); pack(hub->form, field->root, FALSE);
    }
    gtk_widget_show_all(hub->form); gtk_widget_set_sensitive(hub->submit, action != NULL);
    status(hub, "");
}
static void family_selected(GtkComboBox *combo, gpointer data)
{
    Hub *hub = data; if (hub->applying || hub->busy || hub->retry) return;
    const gchar *id = gtk_combo_box_get_active_id(combo); hub->applying = TRUE; gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(hub->action));
    JsonArray *families = hub->catalogue ? array(json_node_get_object(hub->catalogue), "families") : NULL;
    for (guint i = 0; id && families && i < json_array_get_length(families); ++i) {
        JsonObject *family = json_array_get_object_element(families, i); if (g_strcmp0(id, radios_json_string(family, "id", ""))) continue;
        JsonArray *actions = array(family, "actions");
        for (guint j = 0; actions && j < json_array_get_length(actions); ++j) { JsonObject *action = json_array_get_object_element(actions, j); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(hub->action), radios_json_string(action, "id", ""), radios_json_string(action, "label", "İşlem")); }
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(hub->action), 0); hub->applying = FALSE; action_selected(GTK_COMBO_BOX(hub->action), hub);
}
static void catalogued(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; Hub *hub = get(owner); Job *job = data; if (!current(hub, job)) return;
    hub->loading = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result) || radios_json_int(json_node_get_object(result), "contract_version", 0) != 1) {
        status(hub, error ? error->message : "İşlem listesi alınamadı."); return;
    }
    g_clear_pointer(&hub->catalogue, json_node_free); hub->catalogue = copy(result);
    hub->applying = TRUE; gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(hub->family));
    JsonArray *families = array(json_node_get_object(result), "families");
    for (guint i = 0; families && i < json_array_get_length(families); ++i) { JsonObject *family = json_array_get_object_element(families, i); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(hub->family), radios_json_string(family, "id", ""), radios_json_string(family, "label", "İş akışı")); }
    gtk_combo_box_set_active(GTK_COMBO_BOX(hub->family), 0); hub->applying = FALSE; family_selected(GTK_COMBO_BOX(hub->family), hub);
}
static void catalogue_refresh(RadioSViewContext *ctx, GtkWidget *owner)
{
    Hub *hub = get(owner); if (!hub || hub->closed || hub->busy || hub->loading || hub->retry) return;
    /* Passive refresh must not replace an operator's draft. Reference pickers
     * have their own explicit refresh action. */
    if (hub->catalogue) return;
    if (!radios_data_connected(ctx)) { status(hub, "İş akışları için veri servisine bağlanın."); return; }
    g_clear_pointer(&hub->identity, radios_data_client_unref); hub->identity = radios_data_client_acquire(ctx);
    Job *job = job_new(hub); JsonNode *body = radios_data_body(ctx); hub->loading = TRUE;
    if (!radios_data_request(ctx, owner, "POST", "/v1/workflows/desktop-workflows/catalogue", body, catalogued, job, job_free)) hub->loading = FALSE;
    json_node_free(body);
}
static void refresh_clicked(GtkButton *button, gpointer data)
{
    (void)button; Hub *hub = data;
    if (hub->busy || hub->retry) { status(hub, "Önce bekleyen işlemin sonucunu doğrulayın."); return; }
    g_clear_pointer(&hub->catalogue, json_node_free); catalogue_refresh(hub->ctx, hub->root);
}
static void destroyed(GtkWidget *widget, gpointer data) { (void)widget; ((Hub *)data)->closed = TRUE; }
static void hub_free(gpointer data)
{
    Hub *hub = data; g_clear_pointer(&hub->catalogue, json_node_free); g_clear_pointer(&hub->retry, json_node_free);
    g_clear_pointer(&hub->identity, radios_data_client_unref); json_object_unref(hub->history); g_free(hub->retry_path); g_ptr_array_unref(hub->fields); g_free(hub);
}
GtkWidget *radios_workflow_hub_new(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx != NULL, NULL); Hub *hub = g_new0(Hub, 1); hub->ctx = ctx; hub->fields = g_ptr_array_new(); hub->history = json_object_new();
    hub->root = radios_ui_box(GTK_ORIENTATION_VERTICAL, 10, NULL); named(hub->root, "workflow-hub");
    g_object_set_data_full(G_OBJECT(hub->root), "radios-workflow-hub", hub, hub_free); g_signal_connect(hub->root, "destroy", G_CALLBACK(destroyed), hub);
    GtkWidget *toolbar = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL); hub->family = gtk_combo_box_text_new(); hub->action = gtk_combo_box_text_new();
    named(hub->family, "workflow-family"); named(hub->action, "workflow-action"); pack(toolbar, hub->family, TRUE); pack(toolbar, hub->action, TRUE);
    hub->refresh = radios_ui_button("İşlemleri yenile"); named(hub->refresh, "workflow-refresh"); pack(toolbar, hub->refresh, FALSE); pack(hub->root, toolbar, FALSE);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL); named(paned, "workflow-workspace"); gtk_widget_set_size_request(paned, -1, 320); pack(hub->root, paned, TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    hub->form = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, NULL); named(hub->form, "workflow-form"); gtk_container_set_border_width(GTK_CONTAINER(hub->form), 10);
    gtk_container_add(GTK_CONTAINER(scroll), hub->form); gtk_paned_pack1(GTK_PANED(paned), scroll, TRUE, FALSE);
    GtkWidget *results_scroll = gtk_scrolled_window_new(NULL, NULL); hub->results = gtk_tree_view_new(); named(hub->results, "workflow-results");
    const gchar *columns[] = {"Bilgi", "Değer"};
    for (guint i = 0; i < G_N_ELEMENTS(columns); ++i) { GtkCellRenderer *cell = gtk_cell_renderer_text_new(); g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL); GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(columns[i], cell, "text", (gint)i, NULL); gtk_tree_view_column_set_expand(column, TRUE); gtk_tree_view_append_column(GTK_TREE_VIEW(hub->results), column); }
    gtk_container_add(GTK_CONTAINER(results_scroll), hub->results); gtk_paned_pack2(GTK_PANED(paned), results_scroll, TRUE, FALSE); gtk_paned_set_position(GTK_PANED(paned), 520);
    GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    hub->submit = radios_ui_button("İşlemi uygula"); named(hub->submit, "workflow-submit"); gtk_widget_set_sensitive(hub->submit, FALSE); pack(actions, hub->submit, FALSE);
    hub->inspect = radios_ui_button("Güncel kayıtları getir"); named(hub->inspect, "workflow-inspect"); pack(actions, hub->inspect, FALSE); pack(hub->root, actions, FALSE);
    hub->download = radios_ui_button("Dosyayı kaydet"); named(hub->download, "workflow-download"); gtk_widget_set_sensitive(hub->download, FALSE); pack(actions, hub->download, FALSE);
    hub->status = caption("İşlem listesi yükleniyor…"); named(hub->status, "workflow-status"); pack(hub->root, hub->status, FALSE);
    gtk_box_reorder_child(GTK_BOX(hub->root), actions, 2); gtk_box_reorder_child(GTK_BOX(hub->root), hub->status, 3);
    g_signal_connect(hub->family, "changed", G_CALLBACK(family_selected), hub); g_signal_connect(hub->action, "changed", G_CALLBACK(action_selected), hub);
    g_signal_connect(hub->refresh, "clicked", G_CALLBACK(refresh_clicked), hub); g_signal_connect(hub->submit, "clicked", G_CALLBACK(submit), hub);
    g_signal_connect(hub->inspect, "clicked", G_CALLBACK(inspect_result), hub);
    g_signal_connect(hub->download, "clicked", G_CALLBACK(download_result), hub);
    radios_data_register_refresh(ctx, hub->root, catalogue_refresh); catalogue_refresh(ctx, hub->root); return hub->root;
}
