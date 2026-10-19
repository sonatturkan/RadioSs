#include "radios/transfer_ui.h"
#include "radios/data.h"
#include "radios/import_profiles_ui.h"
#include "radios/settings_sync.h"
#include "radios/platform.h"
#include <string.h>

/* Ayarlar › Export / Import: one .st transfer package in each direction.
 * Backend contract: docs/kilavuz-2026-10-18-D.md (POST
 * /v1/workflows/desktop-transfer/<operation>). The page box owns every
 * request, so callbacks stop when the page is destroyed. */
#define PAGE_KEY "radios-transfer-page"
#define API "/v1/workflows/desktop-transfer/"
#define POLL_MS 500
#define POLL_FAILURE_LIMIT 6
#define OFFLINE "Veri servisi bağlı değil."

typedef struct {
    RadioSViewContext *ctx;
    gboolean import;
    GtkWidget *page, *status, *run, *cancel, *progress, *groups, *media;
    /* Export */
    GtkWidget *scope_all, *scope_selected, *destination, *name;
    gchar *default_name;
    /* Import */
    GtkWidget *package, *summary, *summary_head, *summary_media, *summary_warnings;
    GtkWidget *options, *settings, *media_root_row, *media_root;
    gchar *package_path;
    gchar *job_id;
    guint poll, tick, failures;
    gint64 media_count, media_bytes;
    gboolean busy, cancelling, loading, loaded, online, polling, disposed;
    gboolean syncing; /* a group button and its document list are being matched up */
} TransferPage;

static void pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static GtkWidget *styled(GtkWidget *widget, const gchar *style)
{ gtk_style_context_add_class(gtk_widget_get_style_context(widget), style); return widget; }
static GtkWidget *named(GtkWidget *widget, const gchar *name) { gtk_widget_set_name(widget, name); return widget; }
static GtkWidget *text(const gchar *value, const gchar *style)
{
    GtkWidget *label = gtk_label_new(value); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE); gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 90);
    return style ? styled(label, style) : label;
}
static GtkWidget *section_new(const gchar *title)
{
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); styled(section, "preferences-section");
    pack(section, text(title, "preferences-subtitle"), FALSE);
    return section;
}
static GtkWidget *field_row(const gchar *caption, GtkWidget *control)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *label = text(caption, NULL); gtk_widget_set_size_request(label, 110, -1);
    pack(row, label, FALSE); pack(row, control, TRUE); return row;
}
/* Dependent controls are shown or hidden, never greyed: show the subtree
 * once, then keep show_all away from it and toggle it explicitly. */
static GtkWidget *optional(GtkWidget *widget)
{ gtk_widget_show_all(widget); gtk_widget_set_no_show_all(widget, TRUE); gtk_widget_hide(widget); return widget; }
static gboolean active(GtkWidget *toggle) { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle)); }
static TransferPage *page_of(GtkWidget *owner) { return owner ? g_object_get_data(G_OBJECT(owner), PAGE_KEY) : NULL; }
static JsonObject *object_of(JsonNode *node)
{ return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL; }
static JsonObject *member_object(JsonObject *parent, const gchar *name)
{ return parent ? object_of(json_object_get_member(parent, name)) : NULL; }
static JsonArray *member_array(JsonObject *parent, const gchar *name)
{
    JsonNode *node = parent ? json_object_get_member(parent, name) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static void status_set(TransferPage *page, const gchar *message)
{ gtk_label_set_text(GTK_LABEL(page->status), message ? message : ""); }
static void status_failure(TransferPage *page, const gchar *prefix, const GError *failure)
{
    gchar *message = g_strdup_printf("%s: %s", prefix, failure ? failure->message : "geçersiz yanıt");
    status_set(page, message); g_free(message);
}
static void announce(TransferPage *page, const gchar *message)
{ if (page->ctx->notify) page->ctx->notify(page->ctx->notify_data, message); }
static void css_ensure(void)
{
    static gsize once = 0;
    if (!g_once_init_enter(&once)) return;
    GdkScreen *screen = gdk_screen_get_default();
    if (screen) {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            "#preferences-window .preferences-status { color: #929292; font-size: 12px; }"
            "#preferences-window .transfer-summary-head { color: #c8c8c8; }", -1, NULL);
        gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
        /* Above the Preferences sheet (APPLICATION + 30), which styles every
         * button and check: a flat disclosure toggle, and a partly chosen
         * group drawn like a checked one with a dash instead of a tick. */
        css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            "#preferences-window button.transfer-expand { min-width: 18px; min-height: 18px; padding: 0 5px; color: #929292;"
            " background-color: transparent; border-color: transparent; }"
            "#preferences-window button.transfer-expand:hover, #preferences-window button.transfer-expand:checked:hover"
            " { color: #ededed; background-color: #222; border-color: #3b3b3b; }"
            "#preferences-window button.transfer-expand:checked { color: #ededed; background-color: transparent; border-color: transparent; }"
            "#preferences-window button.transfer-expand:focus { border-color: #36caff; }"
            "#preferences-window check:indeterminate { border-color: #36caff; background-color: #36caff; }", -1, NULL);
        gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 31);
        g_object_unref(css);
    }
    g_once_init_leave(&once, 1);
}

/* Group rows: one check button per settings kind, in both directions. An
 * export group that lists its documents also gets a disclosure toggle and an
 * indented list, hidden until opened, with one check button per document. The
 * group button then mirrors that list: all checked → checked, some →
 * inconsistent (the group still goes out, with only those documents), none →
 * unchecked. Clicking it checks the whole list, or clears a full one. */
static void rows_clear(GtkWidget *box)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *item = children; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(children);
}
static guint rows_count(GtkWidget *box)
{ GList *children = gtk_container_get_children(GTK_CONTAINER(box)); guint count = g_list_length(children); g_list_free(children); return count; }
static void update(TransferPage *page);
static GtkWidget *group_check(GtkWidget *row)
{ return GTK_IS_TOGGLE_BUTTON(row) ? row : g_object_get_data(G_OBJECT(row), "transfer-check"); }
static GtkWidget *group_list(GtkWidget *check) { return check ? g_object_get_data(G_OBJECT(check), "transfer-list") : NULL; }
static guint group_checked(GtkWidget *check, guint *total)
{
    GtkWidget *list = group_list(check); guint checked = 0; *total = 0;
    if (!list) return 0;
    GList *items = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *item = items; item; item = item->next) { ++*total; if (active(item->data)) ++checked; }
    g_list_free(items); return checked;
}
static gboolean group_included(GtkWidget *check)
{ guint total, checked = group_checked(check, &total); return total ? checked > 0 : active(check); }
/* The flag keeps programmatic toggles from feeding back between the two. */
static void group_sync(TransferPage *page, GtkWidget *check)
{
    guint total, checked = group_checked(check, &total);
    if (!total) return;
    page->syncing = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), checked == total);
    gtk_toggle_button_set_inconsistent(GTK_TOGGLE_BUTTON(check), checked > 0 && checked < total);
    page->syncing = FALSE;
}
static void group_toggled(GtkToggleButton *button, gpointer data)
{
    TransferPage *page = data; GtkWidget *list = group_list(GTK_WIDGET(button));
    if (page->syncing) return;
    if (list) {
        gboolean state = gtk_toggle_button_get_active(button);
        page->syncing = TRUE; gtk_toggle_button_set_inconsistent(button, FALSE);
        GList *items = gtk_container_get_children(GTK_CONTAINER(list));
        for (GList *item = items; item; item = item->next) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(item->data), state);
        g_list_free(items); page->syncing = FALSE;
    }
    update(page);
}
static void item_toggled(GtkToggleButton *button, gpointer data)
{
    TransferPage *page = data;
    if (page->syncing) return;
    group_sync(page, g_object_get_data(G_OBJECT(button), "transfer-group"));
    update(page);
}
static void expand_toggled(GtkToggleButton *button, gpointer data)
{
    (void)data; gboolean open = gtk_toggle_button_get_active(button);
    gtk_button_set_label(GTK_BUTTON(button), open ? "▾" : "▸");
    gtk_widget_set_visible(g_object_get_data(G_OBJECT(button), "transfer-items"), open);
}
static GtkWidget *group_check_new(TransferPage *page, const gchar *prefix, const gchar *kind, const gchar *label, gint64 count)
{
    gchar *caption = g_str_equal(kind, "local_settings") ? g_strdup(label) : g_strdup_printf("%s (%" G_GINT64_FORMAT ")", label, count);
    GtkWidget *row = gtk_check_button_new_with_label(caption); g_free(caption);
    gchar *name = g_strconcat(prefix, kind, NULL); named(row, name); g_free(name);
    g_object_set_data_full(G_OBJECT(row), "transfer-kind", g_strdup(kind), g_free);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row), TRUE);
    g_signal_connect(row, "toggled", G_CALLBACK(group_toggled), page);
    return row;
}
static void row_add(TransferPage *page, const gchar *prefix, const gchar *kind, const gchar *label, gint64 count)
{ GtkWidget *row = group_check_new(page, prefix, kind, label, count); pack(page->groups, row, FALSE); gtk_widget_show_all(row); }

/* The page reloads its groups on every map and data refresh. A reload keeps
 * the operator's choices: unchecked groups and documents stay unchecked, open
 * lists stay open, and a document new to a checked group arrives checked. */
static gchar *choice_key(const gchar *what, const gchar *kind, const gchar *id)
{ return id ? g_strjoin("\x1f", what, kind, id, NULL) : g_strjoin("\x1f", what, kind, NULL); }
static gboolean choice_saved(GHashTable *saved, const gchar *what, const gchar *kind, const gchar *id)
{ gchar *key = choice_key(what, kind, id); gboolean found = g_hash_table_contains(saved, key); g_free(key); return found; }
static GHashTable *choices_save(TransferPage *page)
{
    GHashTable *saved = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GList *rows = gtk_container_get_children(GTK_CONTAINER(page->groups));
    for (GList *row = rows; row; row = row->next) {
        GtkWidget *check = group_check(row->data), *list = group_list(check), *expand;
        const gchar *kind = check ? g_object_get_data(G_OBJECT(check), "transfer-kind") : NULL;
        if (!kind) continue;
        if (!group_included(check)) g_hash_table_add(saved, choice_key("off", kind, NULL));
        else if (list) {
            GList *items = gtk_container_get_children(GTK_CONTAINER(list));
            for (GList *item = items; item; item = item->next)
                if (!active(item->data)) g_hash_table_add(saved, choice_key("off", kind, g_object_get_data(G_OBJECT(item->data), "transfer-id")));
            g_list_free(items);
        }
        if ((expand = g_object_get_data(G_OBJECT(check), "transfer-expand")) && active(expand))
            g_hash_table_add(saved, choice_key("open", kind, NULL));
    }
    g_list_free(rows); return saved;
}
static void export_row_add(TransferPage *page, JsonObject *group, GHashTable *saved)
{
    const gchar *kind = radios_json_string(group, "id", "");
    if (!*kind) return;
    GtkWidget *check = group_check_new(page, "transfer-export-group-", kind, radios_json_string(group, "label", kind),
        radios_json_int(group, "count", 0));
    GtkWidget *entry = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    g_object_set_data(G_OBJECT(entry), "transfer-check", check);
    pack(head, check, FALSE); pack(entry, head, FALSE);
    gboolean off = choice_saved(saved, "off", kind, NULL);
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    JsonArray *documents = g_str_equal(kind, "local_settings") ? NULL : member_array(group, "documents");
    for (guint i = 0; documents && i < json_array_get_length(documents); ++i) {
        JsonObject *document = object_of(json_array_get_element(documents, i));
        const gchar *id = radios_json_string(document, "id", ""), *title = radios_json_string(document, "name", "");
        if (!*id) continue;
        GtkWidget *item = gtk_check_button_new_with_label(*title ? title : id), *caption = gtk_bin_get_child(GTK_BIN(item));
        gchar *name = g_strconcat("transfer-export-item-", kind, "-", id, NULL); named(item, name); g_free(name);
        if (GTK_IS_LABEL(caption)) { gtk_label_set_ellipsize(GTK_LABEL(caption), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(caption), 72); }
        if (g_utf8_strlen(title, -1) > 72) gtk_widget_set_tooltip_text(item, title);
        g_object_set_data_full(G_OBJECT(item), "transfer-id", g_strdup(id), g_free);
        g_object_set_data(G_OBJECT(item), "transfer-group", check);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(item), !off && !choice_saved(saved, "off", kind, id));
        g_signal_connect(item, "toggled", G_CALLBACK(item_toggled), page);
        pack(list, item, FALSE);
    }
    if (rows_count(list)) {
        gchar *name = g_strconcat("transfer-export-group-", kind, "-expand", NULL);
        GtkWidget *expand = styled(named(gtk_toggle_button_new_with_label("▸"), name), "transfer-expand"); g_free(name);
        gtk_widget_set_tooltip_text(expand, "Kayıtları göster"); gtk_widget_set_valign(expand, GTK_ALIGN_CENTER);
        name = g_strconcat("transfer-export-group-", kind, "-items", NULL);
        GtkWidget *items = named(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), name); g_free(name);
        gtk_widget_set_margin_start(items, 28);
        /* A long list scrolls on its own instead of pushing the form down. */
        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
        gtk_container_add(GTK_CONTAINER(scroll), list); pack(items, scroll, FALSE);
        g_object_set_data(G_OBJECT(expand), "transfer-items", items);
        g_object_set_data(G_OBJECT(check), "transfer-list", list);
        g_object_set_data(G_OBJECT(check), "transfer-expand", expand);
        g_signal_connect(expand, "toggled", G_CALLBACK(expand_toggled), NULL);
        pack(head, expand, FALSE); pack(entry, optional(items), FALSE);
        group_sync(page, check);
        if (choice_saved(saved, "open", kind, NULL)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(expand), TRUE);
    } else {
        g_object_ref_sink(list); g_object_unref(list);
        page->syncing = TRUE; gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), !off); page->syncing = FALSE;
    }
    pack(page->groups, entry, FALSE); gtk_widget_show_all(entry);
}
static JsonArray *checked_kinds(TransferPage *page)
{
    JsonArray *kinds = json_array_new();
    GList *rows = gtk_container_get_children(GTK_CONTAINER(page->groups));
    for (GList *row = rows; row; row = row->next) {
        GtkWidget *check = group_check(row->data);
        if (check && group_included(check)) json_array_add_string_element(kinds, g_object_get_data(G_OBJECT(check), "transfer-kind"));
    }
    g_list_free(rows); return kinds;
}
/* {kind: [id, ...]} for partly chosen groups only, or NULL: a fully checked
 * group goes out whole, including documents beyond the listed ones. */
static JsonObject *chosen_documents(TransferPage *page)
{
    JsonObject *documents = json_object_new();
    GList *rows = gtk_container_get_children(GTK_CONTAINER(page->groups));
    for (GList *row = rows; row; row = row->next) {
        GtkWidget *check = group_check(row->data); guint total = 0, checked = check ? group_checked(check, &total) : 0;
        if (!checked || checked == total) continue;
        JsonArray *ids = json_array_new();
        GList *items = gtk_container_get_children(GTK_CONTAINER(group_list(check)));
        for (GList *item = items; item; item = item->next)
            if (active(item->data)) json_array_add_string_element(ids, g_object_get_data(G_OBJECT(item->data), "transfer-id"));
        g_list_free(items);
        json_object_set_array_member(documents, g_object_get_data(G_OBJECT(check), "transfer-kind"), ids);
    }
    g_list_free(rows);
    if (json_object_get_size(documents)) return documents;
    json_object_unref(documents); return NULL;
}
static gboolean any_checked(TransferPage *page)
{ JsonArray *kinds = checked_kinds(page); gboolean any = json_array_get_length(kinds) > 0; json_array_unref(kinds); return any; }

/* Normal form: dependent fields follow the selection; everything idles while
 * the data service is away or a job runs. */
static void update(TransferPage *page)
{
    gboolean online = radios_data_connected(page->ctx), idle = online && !page->busy;
    if (page->import) {
        gboolean inspected = page->loaded && page->package_path != NULL;
        gboolean has_settings = rows_count(page->groups) > 0, has_media = page->media_count > 0;
        gtk_widget_set_visible(page->summary, inspected);
        gtk_widget_set_visible(page->options, inspected);
        gtk_widget_set_visible(page->settings, has_settings);
        gboolean settings = has_settings && active(page->settings), media = has_media && active(page->media);
        gtk_widget_set_visible(page->groups, settings);
        gtk_widget_set_visible(page->media, has_media);
        gtk_widget_set_visible(page->media_root_row, media);
        GtkWidget *controls[] = {page->package, page->settings, page->groups, page->media, page->media_root};
        for (guint i = 0; i < G_N_ELEMENTS(controls); ++i) gtk_widget_set_sensitive(controls[i], idle);
        gtk_widget_set_sensitive(page->run, idle && inspected && ((settings && any_checked(page)) || media));
    } else {
        gboolean selected = active(page->scope_selected);
        gtk_widget_set_visible(page->groups, selected);
        GtkWidget *controls[] = {page->scope_all, page->scope_selected, page->groups, page->destination, page->name};
        for (guint i = 0; i < G_N_ELEMENTS(controls); ++i) gtk_widget_set_sensitive(controls[i], idle);
        gtk_widget_set_sensitive(page->media, idle && page->media_count > 0);
        gboolean chosen = !selected || any_checked(page) || (page->media_count > 0 && active(page->media));
        gtk_widget_set_sensitive(page->run, idle && page->loaded && chosen);
    }
    gtk_widget_set_visible(page->cancel, page->busy);
    gtk_widget_set_sensitive(page->cancel, page->busy && page->job_id != NULL && !page->cancelling);
}
static void toggled(GtkToggleButton *button, gpointer data) { (void)button; update(data); }

static gboolean post(TransferPage *page, const gchar *operation, JsonNode *body, RadioSDataCallback callback)
{
    gchar *path = g_strconcat(API, operation, NULL);
    gboolean sent = radios_data_request(page->ctx, page->page, "POST", path, body, callback, NULL, NULL);
    g_free(path); json_node_free(body); return sent;
}

/* Job progress: poll every 500 ms until a terminal state. */
static void poll_stop(TransferPage *page)
{ if (page->poll) { g_source_remove(page->poll); page->poll = 0; } }
static void job_finish(TransferPage *page, const gchar *message, gboolean notify)
{
    page->busy = FALSE; page->cancelling = FALSE; g_clear_pointer(&page->job_id, g_free);
    poll_stop(page); status_set(page, message);
    if (notify) announce(page, message);
    update(page);
}
static void polled(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *failure, gpointer data)
{
    (void)data; TransferPage *page = page_of(owner);
    if (!page || page->disposed) return;
    page->polling = FALSE;
    if (!page->busy) return;
    JsonObject *job = object_of(node);
    if (failure || !job) {
        if (++page->failures < POLL_FAILURE_LIMIT) return;
        gchar *message = g_strdup_printf("İş durumu alınamadı: %s", failure ? failure->message : "geçersiz yanıt");
        job_finish(page, message, TRUE); g_free(message); return;
    }
    page->failures = 0;
    gint64 progress = radios_json_int(job, "progress", 0); progress = CLAMP(progress, 0, 100);
    const gchar *message = radios_json_string(job, "message", ""), *state = radios_json_string(job, "state", "");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(page->progress), (gdouble)progress / 100.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(page->progress), *message ? message : state);
    if (g_str_equal(state, "running")) return;
    gchar *final;
    if (g_str_equal(state, "done")) {
        JsonObject *result = member_object(job, "result");
        GString *details = g_string_new(NULL);
        JsonArray *warnings = member_array(result, "warnings");
        for (guint i = 0; warnings && i < json_array_get_length(warnings); ++i) {
            JsonNode *warning = json_array_get_element(warnings, i);
            if (JSON_NODE_HOLDS_VALUE(warning) && json_node_get_value_type(warning) == G_TYPE_STRING)
                g_string_append_printf(details, "\n• %s", json_node_get_string(warning));
        }
        JsonObject *local = member_object(result, "local_settings");
        if (page->import && local && json_object_get_size(local)) {
            GError *local_error = NULL;
            if (!radios_settings_local_import(ctx, local, &local_error)) {
                g_string_append_printf(details, "\nYerel ayarlar uygulanamadı: %s", local_error ? local_error->message : "bilinmeyen hata");
                g_clear_error(&local_error);
            } else g_string_append(details, "\nYerel ayarlar yüklendi. Arşiv, analiz ve depolama değişiklikleri yeniden açılışta uygulanır.");
        }
        const gchar *path = radios_json_string(job, "package_path", "");
        if (page->import) {
            final = g_strdup_printf("İçe aktarma tamamlandı.%s%s%s", *message ? " " : "", message, details->str);
            radios_data_refresh(ctx);
        } else final = *path ? g_strdup_printf("Paket yazıldı: %s%s", path, details->str) :
            g_strdup_printf("Dışa aktarma tamamlandı.%s%s%s", *message ? " " : "", message, details->str);
        g_string_free(details, TRUE);
    } else if (g_str_equal(state, "cancelled")) {
        final = g_strdup(page->import ? "İçe aktarma iptal edildi." : "Dışa aktarma iptal edildi.");
    } else final = g_strdup_printf("%s başarısız: %s", page->import ? "İçe aktarma" : "Dışa aktarma",
        *message ? message : "bilinmeyen hata");
    job_finish(page, final, TRUE); g_free(final);
}
static gboolean poll_tick(gpointer data)
{
    TransferPage *page = data;
    if (page->disposed || !page->busy || !page->job_id) { page->poll = 0; return G_SOURCE_REMOVE; }
    if (page->polling) return G_SOURCE_CONTINUE;
    JsonNode *body = radios_data_body(page->ctx);
    json_object_set_string_member(json_node_get_object(body), "job_id", page->job_id);
    page->polling = post(page, "job", body, polled);
    if (page->polling) return G_SOURCE_CONTINUE;
    page->poll = 0; job_finish(page, "İş durumu alınamadı: veri servisi bağlı değil.", TRUE);
    return G_SOURCE_REMOVE;
}
static void started(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *failure, gpointer data)
{
    (void)ctx; (void)data; TransferPage *page = page_of(owner);
    if (!page || page->disposed) return;
    const gchar *job_id = radios_json_string(object_of(node), "job_id", "");
    if (failure || !*job_id) {
        page->busy = FALSE; gtk_widget_hide(page->progress);
        gchar *message = g_strdup_printf("%s başlatılamadı: %s", page->import ? "İçe aktarma" : "Dışa aktarma",
            failure ? failure->message : "geçersiz yanıt");
        status_set(page, message); announce(page, message); g_free(message); update(page); return;
    }
    g_free(page->job_id); page->job_id = g_strdup(job_id);
    status_set(page, page->import ? "İçe aktarılıyor…" : "Dışa aktarılıyor…");
    poll_stop(page); page->poll = g_timeout_add(POLL_MS, poll_tick, page);
    update(page);
}
static void job_begin(TransferPage *page, const gchar *operation, JsonNode *body, const gchar *message)
{
    page->busy = TRUE; page->cancelling = FALSE; page->failures = 0; page->polling = FALSE;
    g_clear_pointer(&page->job_id, g_free);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(page->progress), 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(page->progress), message);
    gtk_widget_show(page->progress); status_set(page, message);
    if (!post(page, operation, body, started)) { page->busy = FALSE; gtk_widget_hide(page->progress); status_set(page, OFFLINE); }
    update(page);
}
static void cancel_sent(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *failure, gpointer data)
{
    (void)ctx; (void)node; (void)data; TransferPage *page = page_of(owner);
    if (!page || page->disposed) return;
    if (failure) { page->cancelling = FALSE; status_failure(page, "İptal isteği gönderilemedi", failure); }
    update(page);
}
static void cancel_clicked(GtkButton *button, gpointer data)
{
    (void)button; TransferPage *page = data;
    if (!page->busy || !page->job_id || page->cancelling) return;
    JsonNode *body = radios_data_body(page->ctx);
    json_object_set_string_member(json_node_get_object(body), "job_id", page->job_id);
    page->cancelling = post(page, "job-cancel", body, cancel_sent);
    status_set(page, page->cancelling ? "İptal ediliyor…" : OFFLINE); update(page);
}

/* Export: settings groups and media counts come from the service. */
static gchar *slug(const gchar *value)
{
    GString *out = g_string_new(NULL); gboolean separator = TRUE;
    for (const gchar *p = value ? value : ""; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (g_unichar_isalnum(c)) { g_string_append_unichar(out, c); separator = FALSE; }
        else if (!separator) { g_string_append_c(out, '-'); separator = TRUE; }
    }
    while (out->len && out->str[out->len - 1] == '-') g_string_truncate(out, out->len - 1);
    return g_string_free(out, FALSE);
}
static gchar *default_name(TransferPage *page)
{
    gchar *station = slug(radios_json_string(member_object(radios_data_context(page->ctx), "station"), "name", ""));
    GDateTime *now = g_date_time_new_now_local(); gchar *stamp = g_date_time_format(now, "%Y%m%d-%H%M"); g_date_time_unref(now);
    gchar *name = *station ? g_strdup_printf("RadioS-%s-%s.st", station, stamp) : g_strdup_printf("RadioS-%s.st", stamp);
    g_free(station); g_free(stamp); return name;
}
static void name_refresh(TransferPage *page)
{
    const gchar *current = gtk_entry_get_text(GTK_ENTRY(page->name));
    gchar *name = default_name(page);
    if (!*current || !g_strcmp0(current, page->default_name)) gtk_entry_set_text(GTK_ENTRY(page->name), name);
    g_free(page->default_name); page->default_name = name;
}
static void groups_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *failure, gpointer data)
{
    (void)ctx; (void)data; TransferPage *page = page_of(owner);
    if (!page || page->disposed) return;
    page->loading = FALSE;
    JsonObject *result = object_of(node);
    if (failure || !result) { status_failure(page, "Ayar grupları yüklenemedi", failure); update(page); return; }
    GHashTable *saved = choices_save(page);
    rows_clear(page->groups);
    JsonArray *groups = member_array(result, "groups");
    for (guint i = 0; groups && i < json_array_get_length(groups); ++i)
        export_row_add(page, object_of(json_array_get_element(groups, i)), saved);
    g_hash_table_unref(saved);
    JsonObject *media = member_object(result, "media");
    page->media_count = radios_json_int(media, "count", 0); page->media_bytes = radios_json_int(media, "bytes", 0);
    gchar *size = g_format_size((guint64)MAX(page->media_bytes, 0));
    gchar *caption = page->media_count > 0 ? g_strdup_printf("Medya (%" G_GINT64_FORMAT " şarkı, %s)", page->media_count, size) : g_strdup("Medya (yok)");
    gtk_button_set_label(GTK_BUTTON(page->media), caption); g_free(caption); g_free(size);
    if (page->media_count <= 0) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->media), FALSE);
    name_refresh(page);
    page->loaded = TRUE;
    if (!page->busy) status_set(page, "");
    update(page);
}
static void groups_load(TransferPage *page)
{
    if (page->import || page->loading || page->busy || !radios_data_connected(page->ctx)) return;
    page->loading = TRUE; status_set(page, "Ayar grupları yükleniyor…");
    if (!post(page, "groups", radios_data_body(page->ctx), groups_loaded)) { page->loading = FALSE; status_set(page, OFFLINE); }
    update(page);
}
static void export_clicked(GtkButton *button, gpointer data)
{
    (void)button; TransferPage *page = data;
    if (page->busy || !page->loaded || !radios_data_connected(page->ctx)) return;
    gchar *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(page->destination));
    if (!folder || !g_file_test(folder, G_FILE_TEST_IS_DIR)) { status_set(page, "Hedef klasör seçin."); g_free(folder); return; }
    gchar *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(page->name))));
    if (!*name) { g_free(name); name = default_name(page); }
    /* Windows names are case-insensitive: "Paket.ST" already has the suffix. */
    gsize length = strlen(name);
    if (length >= 3 && g_ascii_strcasecmp(name + length - 3, ".st") == 0) memcpy(name + length - 3, ".st", 3);
    else { gchar *fixed = g_strconcat(name, ".st", NULL); g_free(name); name = fixed; }
    /* The same rules on every system, so packages stay interchangeable:
     * no \ / : * ? " < > | or control characters, no reserved device name
     * (CON, NUL, COM1 ...), no trailing dot or space (NTFS would drop it),
     * no leading dot, at most 200 bytes. */
    if (!radios_path_component_valid(name) || *name == '.' || strlen(name) > 200) {
        status_set(page, "Dosya adı \\ / : * ? \" < > | içeremez, ayrılmış aygıt adı (CON, NUL, COM1…) olamaz, "
            "nokta ile başlayamaz, nokta/boşlukla bitemez ve 200 karakteri aşamaz.");
        g_free(name); g_free(folder); return;
    }
    gtk_entry_set_text(GTK_ENTRY(page->name), name);
    gboolean selected = active(page->scope_selected), media = page->media_count > 0 && active(page->media);
    JsonArray *kinds = selected ? checked_kinds(page) : json_array_new();
    JsonNode *body = radios_data_body(page->ctx); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "destination_dir", folder);
    json_object_set_string_member(object, "file_name", name);
    json_object_set_string_member(object, "settings", !selected ? "all" : json_array_get_length(kinds) ? "selected" : "none");
    json_object_set_array_member(object, "kinds", kinds);
    JsonObject *documents = selected ? chosen_documents(page) : NULL;
    if (documents) json_object_set_object_member(object, "documents", documents);
    json_object_set_boolean_member(object, "media", media);
    gboolean include_local = !selected;
    for (guint i = 0; selected && i < json_array_get_length(kinds); ++i)
        include_local |= g_str_equal(json_array_get_string_element(kinds, i), "local_settings");
    if (include_local && page->ctx->config && page->ctx->config->project_root) {
        GError *local_error = NULL;
        JsonObject *local = radios_settings_local_export(page->ctx->config->project_root, &local_error);
        if (!local) {
            status_failure(page, "Yerel ayarlar okunamadı", local_error); g_clear_error(&local_error);
            json_node_free(body); g_free(folder); g_free(name); return;
        }
        json_object_set_object_member(object, "local_settings", local);
    }
    g_free(folder); g_free(name);
    job_begin(page, "export-start", body, "Dışa aktarma başlatılıyor…");
}

/* Import: the chosen package is inspected before anything is restored. */
static void inspected(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *failure, gpointer data)
{
    (void)ctx; (void)data; TransferPage *page = page_of(owner);
    if (!page || page->disposed) return;
    page->loading = FALSE;
    JsonObject *result = object_of(node);
    if (failure || !result) { status_failure(page, "Paket incelenemedi", failure); update(page); return; }
    JsonObject *manifest = member_object(result, "manifest"), *station = member_object(manifest, "station");
    const gchar *created = radios_json_string(manifest, "created_at", ""), *version = radios_json_string(manifest, "application_version", "");
    GDateTime *stamp = *created ? g_date_time_new_from_iso8601(created, NULL) : NULL;
    GDateTime *local = stamp ? g_date_time_to_local(stamp) : NULL;
    gchar *when = local ? g_date_time_format(local, "%d.%m.%Y %H:%M") : g_strdup(*created ? created : "—");
    if (local) g_date_time_unref(local);
    if (stamp) g_date_time_unref(stamp);
    gchar *head = g_strdup_printf("Oluşturulma: %s · İstasyon: %s%s%s", when, radios_json_string(station, "name", "—"),
        *version ? " · RadioS " : "", version);
    gtk_label_set_text(GTK_LABEL(page->summary_head), head); g_free(head); g_free(when);
    rows_clear(page->groups);
    JsonArray *settings = member_array(result, "settings");
    for (guint i = 0; settings && i < json_array_get_length(settings); ++i) {
        JsonObject *group = object_of(json_array_get_element(settings, i));
        const gchar *kind = radios_json_string(group, "kind", "");
        if (*kind) row_add(page, "transfer-import-group-", kind, radios_json_string(group, "label", kind), radios_json_int(group, "count", 0));
    }
    JsonObject *media = member_object(result, "media");
    page->media_count = radios_json_int(media, "count", 0); page->media_bytes = radios_json_int(media, "bytes", 0);
    gchar *size = g_format_size((guint64)MAX(page->media_bytes, 0));
    gchar *line = page->media_count > 0 ? g_strdup_printf("Medya: %" G_GINT64_FORMAT " şarkı (%s)", page->media_count, size) : g_strdup("Medya: yok");
    gtk_label_set_text(GTK_LABEL(page->summary_media), line); g_free(line); g_free(size);
    GString *warnings = g_string_new(NULL); JsonArray *list = member_array(result, "warnings");
    for (guint i = 0; list && i < json_array_get_length(list); ++i) {
        JsonNode *item = json_array_get_element(list, i);
        if (!JSON_NODE_HOLDS_VALUE(item) || json_node_get_value_type(item) != G_TYPE_STRING) continue;
        g_string_append_printf(warnings, "%sUyarı: %s", warnings->len ? "\n" : "", json_node_get_string(item));
    }
    gtk_label_set_text(GTK_LABEL(page->summary_warnings), warnings->str);
    gtk_widget_set_visible(page->summary_warnings, warnings->len > 0); g_string_free(warnings, TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->settings), rows_count(page->groups) > 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->media), page->media_count > 0);
    page->loaded = TRUE;
    if (!page->busy) status_set(page, "");
    update(page);
}
static void inspect(TransferPage *page, gchar *path)
{
    g_free(page->package_path); page->package_path = path;
    page->loaded = FALSE; page->loading = TRUE; page->media_count = 0; page->media_bytes = 0;
    rows_clear(page->groups); status_set(page, "Paket inceleniyor…");
    JsonNode *body = radios_data_body(page->ctx);
    json_object_set_string_member(json_node_get_object(body), "package_path", path);
    if (!post(page, "inspect", body, inspected)) { page->loading = FALSE; status_set(page, OFFLINE); }
    update(page);
}
static void package_set(GtkFileChooserButton *button, gpointer data)
{
    TransferPage *page = data; if (page->busy) return;
    gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(button));
    if (!path) return;
    if (!radios_data_connected(page->ctx)) { g_free(path); status_set(page, OFFLINE); return; }
    inspect(page, path);
}
static void import_clicked(GtkButton *button, gpointer data)
{
    (void)button; TransferPage *page = data;
    if (page->busy || !page->loaded || !page->package_path || !radios_data_connected(page->ctx)) return;
    gboolean settings = rows_count(page->groups) > 0 && active(page->settings);
    gboolean media = page->media_count > 0 && active(page->media);
    JsonArray *kinds = settings ? checked_kinds(page) : json_array_new();
    if (settings && !json_array_get_length(kinds)) settings = FALSE;
    if (!settings && !media) { json_array_unref(kinds); status_set(page, "Geri yüklenecek içerik seçin."); return; }
    gchar *root = media ? gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(page->media_root)) : NULL;
    if (media && (!root || !g_file_test(root, G_FILE_TEST_IS_DIR))) {
        json_array_unref(kinds); g_free(root); status_set(page, "Medya için hedef ana klasör seçin."); return;
    }
    JsonNode *body = radios_data_body(page->ctx); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "package_path", page->package_path);
    json_object_set_boolean_member(object, "settings", settings);
    json_object_set_array_member(object, "kinds", kinds);
    json_object_set_boolean_member(object, "media", media);
    if (root) json_object_set_string_member(object, "media_root", root); else json_object_set_null_member(object, "media_root");
    g_free(root);
    job_begin(page, "import-start", body, "İçe aktarma başlatılıyor…");
}

/* Connection changes: offline pages idle; a returning service reloads groups. */
static void offline_cleared(TransferPage *page)
{ if (!page->busy && g_str_equal(gtk_label_get_text(GTK_LABEL(page->status)), OFFLINE)) status_set(page, ""); }
static gboolean tick(gpointer data)
{
    TransferPage *page = data; if (page->disposed) return G_SOURCE_REMOVE;
    gboolean online = radios_data_connected(page->ctx);
    if (online != page->online) {
        page->online = online;
        if (!online) {
            if (page->busy) job_finish(page, "Veri servisi bağlantısı kesildi; iş durumu izlenemiyor.", TRUE);
            else status_set(page, OFFLINE);
            page->loading = FALSE; page->polling = FALSE;
        } else { offline_cleared(page); groups_load(page); }
        update(page);
    }
    return G_SOURCE_CONTINUE;
}
static void refreshed(RadioSViewContext *ctx, GtkWidget *owner)
{
    (void)ctx; TransferPage *page = page_of(owner);
    if (!page || page->disposed) return;
    page->online = radios_data_connected(page->ctx);
    if (page->online) offline_cleared(page);
    groups_load(page); update(page);
}
static void mapped(GtkWidget *widget, gpointer data)
{ (void)widget; TransferPage *page = data; if (!page->import) groups_load(page); }
static void destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; TransferPage *page = data; page->disposed = TRUE;
    poll_stop(page); if (page->tick) { g_source_remove(page->tick); page->tick = 0; }
}
static void page_free(gpointer data)
{
    TransferPage *page = data;
    g_free(page->default_name); g_free(page->package_path); g_free(page->job_id); g_free(page);
}
/* Known folder (SHGetKnownFolderPath through GLib: Belgeler, Müzik), else the
 * profile folder.  g_get_home_dir() alone could be an MSYS2/Git-Bash HOME. */
static const gchar *default_folder(GUserDirectory directory)
{
    const gchar *known = g_get_user_special_dir(directory);
    if (known && g_file_test(known, G_FILE_TEST_IS_DIR)) return known;
    known = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    if (known && g_file_test(known, G_FILE_TEST_IS_DIR)) return known;
    return g_get_home_dir();
}
static GtkWidget *folder_chooser(const gchar *title, const gchar *name, const gchar *initial)
{
    GtkWidget *chooser = named(gtk_file_chooser_button_new(title, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER), name);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(chooser), TRUE);
    if (initial && g_file_test(initial, G_FILE_TEST_IS_DIR)) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), initial);
    return chooser;
}
static TransferPage *page_new(RadioSViewContext *ctx, gboolean import, const gchar *root, const gchar *title)
{
    css_ensure();
    TransferPage *page = g_new0(TransferPage, 1); page->ctx = ctx; page->import = import;
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16); gtk_container_set_border_width(GTK_CONTAINER(body), 30);
    named(body, root); page->page = body;
    g_object_set_data_full(G_OBJECT(body), PAGE_KEY, page, page_free);
    g_signal_connect(body, "destroy", G_CALLBACK(destroyed), page);
    g_signal_connect(body, "map", G_CALLBACK(mapped), page);
    pack(body, text(title, "preferences-heading"), FALSE);
    return page;
}
static GtkWidget *page_finish(TransferPage *page, const gchar *prefix)
{
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gchar *name = g_strconcat(prefix, "run", NULL);
    page->run = named(gtk_button_new_with_label(page->import ? "İçe aktar" : "Dışa aktar"), name); g_free(name);
    pack(actions, page->run, FALSE);
    name = g_strconcat(prefix, "cancel", NULL);
    page->cancel = named(gtk_button_new_with_label("Vazgeç"), name); g_free(name);
    pack(actions, optional(page->cancel), FALSE); pack(page->page, actions, FALSE);
    name = g_strconcat(prefix, "progress", NULL);
    page->progress = named(gtk_progress_bar_new(), name); g_free(name);
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(page->progress), TRUE);
    pack(page->page, optional(page->progress), FALSE);
    name = g_strconcat(prefix, "status", NULL);
    page->status = named(text("", "preferences-status"), name); g_free(name);
    gtk_label_set_selectable(GTK_LABEL(page->status), TRUE); pack(page->page, page->status, FALSE);
    g_signal_connect(page->run, "clicked", G_CALLBACK(page->import ? import_clicked : export_clicked), page);
    g_signal_connect(page->cancel, "clicked", G_CALLBACK(cancel_clicked), page);
    page->online = radios_data_connected(page->ctx);
    if (!page->online) status_set(page, OFFLINE);
    if (page->ctx->data) radios_data_register_refresh(page->ctx, page->page, refreshed);
    update(page);
    page->tick = g_timeout_add(POLL_MS, tick, page);
    name = g_strconcat(prefix, "scroll", NULL);
    GtkWidget *scroll = named(gtk_scrolled_window_new(NULL, NULL), name); g_free(name);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), page->page);
    return scroll;
}

GtkWidget *radios_transfer_export_page_new(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx != NULL, NULL);
    TransferPage *page = page_new(ctx, FALSE, "preferences-export-page", "Export");
    GtkWidget *scope = section_new("Kapsam");
    page->scope_all = named(gtk_radio_button_new_with_label(NULL, "Tüm radyo ayarları"), "transfer-export-scope-all");
    page->scope_selected = named(gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(page->scope_all), "Seçili radyo ayarları"),
        "transfer-export-scope-selected");
    pack(scope, page->scope_all, FALSE); pack(scope, page->scope_selected, FALSE);
    page->groups = named(gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), "transfer-export-groups");
    gtk_widget_set_margin_start(page->groups, 28); pack(scope, optional(page->groups), FALSE);
    page->media = named(gtk_check_button_new_with_label("Medya"), "transfer-export-media");
    gtk_widget_set_tooltip_text(page->media, "Şarkı dosyaları, analizleri, albüm ilişkileri ve klasör düzeni");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->media), TRUE); pack(scope, page->media, FALSE);
    pack(page->page, scope, FALSE);
    GtkWidget *target = section_new("Hedef");
    const gchar *backups = ctx->config ? ctx->config->backup_root : NULL;
    page->destination = folder_chooser("Hedef klasör", "transfer-export-destination",
        backups && g_file_test(backups, G_FILE_TEST_IS_DIR) ? backups : default_folder(G_USER_DIRECTORY_DOCUMENTS));
    pack(target, field_row("Klasör", page->destination), FALSE);
    page->name = named(gtk_entry_new(), "transfer-export-name");
    gtk_entry_set_max_length(GTK_ENTRY(page->name), 200);
    page->default_name = default_name(page); gtk_entry_set_text(GTK_ENTRY(page->name), page->default_name);
    pack(target, field_row("Dosya adı", page->name), FALSE);
    pack(page->page, target, FALSE);
    g_signal_connect(page->scope_all, "toggled", G_CALLBACK(toggled), page);
    g_signal_connect(page->scope_selected, "toggled", G_CALLBACK(toggled), page);
    g_signal_connect(page->media, "toggled", G_CALLBACK(toggled), page);
    return page_finish(page, "transfer-export-");
}

GtkWidget *radios_transfer_import_page_new(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx != NULL, NULL);
    TransferPage *page = page_new(ctx, TRUE, "preferences-import-page", "Import");
    GtkWidget *source = section_new("Paket");
    page->package = named(gtk_file_chooser_button_new("Aktarım paketi", GTK_FILE_CHOOSER_ACTION_OPEN), "transfer-import-package");
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(page->package), TRUE);
    GtkFileFilter *filter = gtk_file_filter_new(); gtk_file_filter_set_name(filter, "RadioS aktarım paketi (*.st)");
    gtk_file_filter_add_pattern(filter, "*.[sS][tT]"); gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(page->package), filter);
    pack(source, field_row("Dosya", page->package), FALSE);
    pack(page->page, source, FALSE);
    page->summary = named(section_new("Paket içeriği"), "transfer-import-summary");
    page->summary_head = named(text("", "transfer-summary-head"), "transfer-import-created"); pack(page->summary, page->summary_head, FALSE);
    page->groups = named(gtk_box_new(GTK_ORIENTATION_VERTICAL, 2), "transfer-import-groups");
    pack(page->summary, page->groups, FALSE);
    page->summary_media = named(text("", NULL), "transfer-import-media-summary"); pack(page->summary, page->summary_media, FALSE);
    page->summary_warnings = named(text("", "preferences-subtitle"), "transfer-import-warnings"); pack(page->summary, page->summary_warnings, FALSE);
    pack(page->page, optional(page->summary), FALSE);
    gtk_widget_set_no_show_all(page->groups, TRUE); gtk_widget_set_no_show_all(page->summary_warnings, TRUE);
    page->options = named(section_new("Geri yükleme"), "transfer-import-options");
    page->settings = named(gtk_check_button_new_with_label("Ayarları geri yükle"), "transfer-import-settings");
    pack(page->options, page->settings, FALSE);
    page->media = named(gtk_check_button_new_with_label("Medyayı yerleştir"), "transfer-import-media");
    gtk_widget_set_tooltip_text(page->media, "Şarkılar hedef ana klasörün altına albüm ve alt klasör düzeniyle yerleşir");
    pack(page->options, page->media, FALSE);
    page->media_root = folder_chooser("Hedef ana klasör", "transfer-import-media-root", default_folder(G_USER_DIRECTORY_MUSIC));
    gchar *chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(page->media_root));
    if (!chosen) gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(page->media_root), default_folder(G_USER_DIRECTORY_DOCUMENTS));
    g_free(chosen);
    page->media_root_row = named(field_row("Ana klasör", page->media_root), "transfer-import-media-root-row");
    gtk_widget_set_margin_start(page->media_root_row, 28);
    pack(page->options, page->media_root_row, FALSE);
    pack(page->page, optional(page->options), FALSE);
    gtk_widget_set_no_show_all(page->settings, TRUE); gtk_widget_set_no_show_all(page->media, TRUE);
    gtk_widget_set_no_show_all(page->media_root_row, TRUE);
    g_signal_connect(page->package, "file-set", G_CALLBACK(package_set), page);
    g_signal_connect(page->settings, "toggled", G_CALLBACK(toggled), page);
    g_signal_connect(page->media, "toggled", G_CALLBACK(toggled), page);
    GtkWidget *scroll = page_finish(page, "transfer-import-");
    pack(page->page, radios_import_profiles_section_new(ctx), FALSE);
    return scroll;
}
