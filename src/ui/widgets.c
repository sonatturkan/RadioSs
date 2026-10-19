#include "radios/widgets.h"

static void
add_classes(GtkWidget *widget, const gchar *names)
{
    if (names == NULL)
        return;
    gchar **classes = g_strsplit(names, " ", -1);
    for (guint i = 0; classes[i] != NULL; ++i)
        if (*classes[i] != '\0')
            gtk_style_context_add_class(gtk_widget_get_style_context(widget), classes[i]);
    g_strfreev(classes);
}

GtkWidget *
radios_ui_label(const gchar *text, const gchar *class_name)
{
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(widget), PANGO_WRAP_WORD_CHAR);
    add_classes(widget, class_name);
    /* Keep source HTML text roles distinct. A countdown output and its small
     * caption are both GtkLabels, but source output/span/p rules must not leak
     * font sizes into one another. This is visual metadata only. */
    GtkStyleContext *style = gtk_widget_get_style_context(widget);
    const gchar *semantic = "span";
    static const gchar *const paragraphs[] = {
        "broadcast-content", "header-behavior-text", "header-next-title",
        "header-next-advert", "header-programme-title", "header-current-programme",
        "header-programme-end", "task-help", "automation-help", "task-feedback",
        "pw-status", "automation-status", "pw-empty", "automation-empty", NULL
    };
    for (guint i = 0; paragraphs[i] != NULL; ++i)
        if (gtk_style_context_has_class(style, paragraphs[i]))
            semantic = "paragraph";
    if (gtk_style_context_has_class(style, "header-clock-time") ||
        gtk_style_context_has_class(style, "header-calendar"))
        semantic = "time";
    gchar **classes = g_strsplit(class_name != NULL ? class_name : "", " ", -1);
    for (guint i = 0; classes[i] != NULL; ++i)
        if (g_str_equal(classes[i], "header-countdown") ||
            g_str_equal(classes[i], "knob-value") ||
            g_str_equal(classes[i], "volume-value") ||
            g_str_has_suffix(classes[i], "-volume-value"))
            semantic = "output";
    g_strfreev(classes);
    if (!gtk_style_context_has_class(style, "heading"))
        gtk_style_context_add_class(style, semantic);
    return widget;
}

GtkWidget *
radios_ui_box(GtkOrientation orientation, gint spacing, const gchar *class_name)
{
    GtkWidget *widget = gtk_box_new(orientation, spacing);
    add_classes(widget, class_name);
    return widget;
}

GtkWidget *
radios_ui_button(const gchar *text)
{
    GtkWidget *button = gtk_button_new_with_label(text);
    add_classes(button, "ld-btn");
    return button;
}

GtkWidget *
radios_ui_entry(const gchar *placeholder)
{
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 12);
    return entry;
}

GtkWidget *
radios_ui_combo(const gchar *const *items, gint active)
{
    GtkWidget *combo = gtk_combo_box_text_new();
    for (guint i = 0; items != NULL && items[i] != NULL; ++i)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), items[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
    return combo;
}

GtkWidget *
radios_ui_panel(const gchar *title, GtkWidget **body)
{
    GtkWidget *panel = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, "ld-panel");
    GtkWidget *head = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "ld-head");
    gtk_box_pack_start(GTK_BOX(head), radios_ui_label(title, "ld-tab heading"), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(panel), head, FALSE, FALSE, 0);
    *body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "ld-body");
    gtk_box_pack_start(GTK_BOX(panel), *body, TRUE, TRUE, 0);
    return panel;
}

GtkWidget *
radios_ui_table(const gchar *const *headers, guint count)
{
    GType *types = g_new(GType, count);
    for (guint i = 0; i < count; ++i)
        types[i] = G_TYPE_STRING;
    GtkListStore *store = gtk_list_store_newv((gint)count, types);
    g_free(types);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), TRUE);
    gtk_tree_view_set_grid_lines(GTK_TREE_VIEW(view), GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);
    for (guint i = 0; i < count; ++i) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(headers[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_column_set_expand(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
    }
    add_classes(view, "ld-table");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 120);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    g_object_set_data(G_OBJECT(scroll), "tree-view", view);
    g_object_set_data(G_OBJECT(scroll), "list-store", gtk_tree_view_get_model(GTK_TREE_VIEW(view)));
    return scroll;
}

typedef struct {
    RadioSViewContext *ctx;
    gchar *message;
} Notice;

static void
notice_free(gpointer data, GClosure *closure)
{
    Notice *notice = data;
    (void)closure;
    g_free(notice->message);
    g_free(notice);
}

static void
notice_clicked(GtkButton *button, gpointer data)
{
    Notice *notice = data;
    (void)button;
    if (notice->ctx->notify != NULL)
        notice->ctx->notify(notice->ctx->notify_data, notice->message);
}

void
radios_ui_bind_notice(GtkWidget *button, RadioSViewContext *ctx, const gchar *message)
{
    Notice *notice = g_new0(Notice, 1);
    notice->ctx = ctx;
    notice->message = g_strdup(message);
    g_signal_connect_data(button, "clicked", G_CALLBACK(notice_clicked), notice, notice_free, 0);
}

static void
form_response(GtkDialog *dialog, gint response, gpointer data)
{
    RadioSViewContext *ctx = data;
    if (response == GTK_RESPONSE_ACCEPT) {
        const gchar *message = "Veri servisi henüz bağlı değil; değişiklik kaydedilmedi. Girişler korunuyor.";
        GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "form-status");
        gtk_label_set_text(GTK_LABEL(status), message);
        if (ctx->notify != NULL)
            ctx->notify(ctx->notify_data, message);
        return;
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

void
radios_ui_form_open(RadioSViewContext *ctx, const gchar *title, const gchar *const *fields)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL,
        "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 460, 360);
    add_classes(dialog, "radios task-dialog");
    GtkWidget *area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), area);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll, TRUE, TRUE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(area), 20);
    for (guint i = 0; fields != NULL && fields[i] != NULL; ++i) {
        gtk_box_pack_start(GTK_BOX(area), radios_ui_label(fields[i], "form-label"), FALSE, FALSE, 0);
        GtkWidget *entry = radios_ui_entry(NULL);
        gchar *name = g_strdup_printf("form-field-%u", i);
        gtk_widget_set_name(entry, name);
        g_free(name);
        gtk_entry_set_max_length(GTK_ENTRY(entry), 4096);
        gtk_box_pack_start(GTK_BOX(area), entry, FALSE, FALSE, 0);
    }
    GtkWidget *status = radios_ui_label("Veri bağlantısı bekleniyor.", "task-feedback");
    gtk_box_pack_start(GTK_BOX(area), status, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(dialog), "form-status", status);
    g_signal_connect(dialog, "response", G_CALLBACK(form_response), ctx);
    gtk_widget_show_all(dialog);
}

typedef struct {
    RadioSViewContext *ctx;
    gchar *title;
    gchar **fields;
} Form;

static void
form_free(gpointer data, GClosure *closure)
{
    Form *form = data;
    (void)closure;
    g_free(form->title);
    g_strfreev(form->fields);
    g_free(form);
}

static void
form_clicked(GtkButton *button, gpointer data)
{
    Form *form = data;
    (void)button;
    radios_ui_form_open(form->ctx, form->title, (const gchar *const *)form->fields);
}

void
radios_ui_bind_form(GtkWidget *button, RadioSViewContext *ctx, const gchar *title, const gchar *const *fields)
{
    Form *form = g_new0(Form, 1);
    form->ctx = ctx;
    form->title = g_strdup(title);
    form->fields = g_strdupv((gchar **)fields);
    g_signal_connect_data(button, "clicked", G_CALLBACK(form_clicked), form, form_free, 0);
}

/* Kılavuz s.4 adım 5, kullanıcı isteği 18.10 (U4): mixer channels are chosen
 * in a compact tree, not a wall of 32 boxes. One summary button opens a
 * popover; its tree groups the defined strips under their DJ / Konuşma /
 * Müzik label ("Etiketsiz" without one). A saved channel the mixer no longer
 * defines stays listed, so a choice is never lost unseen. */
enum { PICK_ACTIVE, PICK_PARTIAL, PICK_TEXT, PICK_NUMBER, PICK_COLUMNS };
typedef struct {
    GtkWidget *button, *caption, *tree;
    GtkTreeStore *store;
    JsonArray *channels;
    guint32 selected;
    void (*changed)(GtkWidget *picker, gpointer user);
    gpointer user;
} ChannelPicker;
static const gchar *const picker_labels[] = {"dj", "talk", "music", ""};
static const gchar *const picker_titles[] = {"DJ", "Konuşma", "Müzik", "Etiketsiz"};

static ChannelPicker *
picker_get(GtkWidget *widget)
{
    return widget != NULL ? g_object_get_data(G_OBJECT(widget), "radios-channel-picker") : NULL;
}

static JsonObject *
picker_channel(ChannelPicker *picker, guint number)
{
    for (guint i = 0; picker->channels != NULL && i < json_array_get_length(picker->channels); ++i) {
        JsonObject *row = json_array_get_object_element(picker->channels, i);
        if (row != NULL && json_object_has_member(row, "number") &&
            (guint)json_object_get_int_member(row, "number") == number)
            return row;
    }
    return NULL;
}

static const gchar *
picker_text(JsonObject *object, const gchar *key)
{
    JsonNode *node = object != NULL ? json_object_get_member(object, key) : NULL;
    return node != NULL && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : "";
}

static void
picker_caption(ChannelPicker *picker)
{
    guint count = 0;
    GString *list = g_string_new(NULL);
    for (guint number = 1; number <= 32; ++number)
        if (picker->selected & (1u << (number - 1))) {
            ++count;
            g_string_append_printf(list, "%s%u", list->len ? ", " : "", number);
        }
    gchar *text = count ? g_strdup_printf("%u kanal seçili ▾", count) : g_strdup("Kanal seçilmedi ▾");
    gtk_label_set_text(GTK_LABEL(picker->caption), text);
    gchar *tip = count ? g_strdup_printf("Kanal %s", list->str) : NULL;
    gtk_widget_set_tooltip_text(picker->button, tip);
    g_free(text);
    g_free(tip);
    g_string_free(list, TRUE);
}

/* Group rows show all / some / none of their channels. */
static void
picker_groups(ChannelPicker *picker)
{
    GtkTreeModel *model = GTK_TREE_MODEL(picker->store);
    GtkTreeIter group, child;
    gboolean more = gtk_tree_model_get_iter_first(model, &group);
    while (more) {
        guint on = 0, total = 0;
        gboolean children = gtk_tree_model_iter_children(model, &child, &group);
        while (children) {
            gint number = 0;
            gtk_tree_model_get(model, &child, PICK_NUMBER, &number, -1);
            gboolean active = number > 0 && (picker->selected & (1u << (number - 1)));
            gtk_tree_store_set(picker->store, &child, PICK_ACTIVE, active, -1);
            on += active;
            ++total;
            children = gtk_tree_model_iter_next(model, &child);
        }
        gtk_tree_store_set(picker->store, &group, PICK_ACTIVE, total && on == total, PICK_PARTIAL, on && on < total, -1);
        more = gtk_tree_model_iter_next(model, &group);
    }
    picker_caption(picker);
}

static gchar *
picker_row_text(ChannelPicker *picker, guint number)
{
    JsonObject *row = picker_channel(picker, number);
    const gchar *name = picker_text(row, "name"), *source = picker_text(row, "source");
    if (row == NULL)
        return g_strdup_printf("Kanal %u · kayıtlı, mikserde tanımsız", number);
    GString *text = g_string_new(NULL);
    g_string_append_printf(text, "Kanal %u", number);
    if (*name && g_strcmp0(name, text->str))
        g_string_append_printf(text, " · %s", name);
    if (*source)
        g_string_append_printf(text, " · %s", source);
    return g_string_free(text, FALSE);
}

static void
picker_render(ChannelPicker *picker)
{
    GtkTreeModel *model = GTK_TREE_MODEL(picker->store);
    gtk_tree_store_clear(picker->store);
    guint32 listed = 0;
    for (guint g = 0; g < G_N_ELEMENTS(picker_labels); ++g) {
        GtkTreeIter group;
        gboolean added = FALSE;
        for (guint i = 0; picker->channels != NULL && i < json_array_get_length(picker->channels); ++i) {
            JsonObject *row = json_array_get_object_element(picker->channels, i);
            gint64 number = row != NULL && json_object_has_member(row, "number") ? json_object_get_int_member(row, "number") : 0;
            const gchar *label = picker_text(row, "label");
            gboolean known = !g_strcmp0(label, "dj") || !g_strcmp0(label, "talk") || !g_strcmp0(label, "music");
            if (number < 1 || number > 32 || (listed & (1u << (number - 1))) ||
                (*picker_labels[g] ? g_strcmp0(label, picker_labels[g]) : known))
                continue;
            if (!added) {
                gtk_tree_store_append(picker->store, &group, NULL);
                gtk_tree_store_set(picker->store, &group, PICK_TEXT, picker_titles[g], PICK_NUMBER, 0, -1);
                added = TRUE;
            }
            GtkTreeIter child;
            gchar *text = picker_row_text(picker, (guint)number);
            gtk_tree_store_append(picker->store, &child, &group);
            gtk_tree_store_set(picker->store, &child, PICK_TEXT, text, PICK_NUMBER, (gint)number, -1);
            g_free(text);
            listed |= 1u << (number - 1);
        }
    }
    guint32 orphans = picker->selected & ~listed;
    if (orphans) {
        GtkTreeIter group, child;
        gtk_tree_store_append(picker->store, &group, NULL);
        gtk_tree_store_set(picker->store, &group, PICK_TEXT, "Kayıtlı · tanımsız", PICK_NUMBER, 0, -1);
        for (guint number = 1; number <= 32; ++number)
            if (orphans & (1u << (number - 1))) {
                gchar *text = picker_row_text(picker, number);
                gtk_tree_store_append(picker->store, &child, &group);
                gtk_tree_store_set(picker->store, &child, PICK_TEXT, text, PICK_NUMBER, (gint)number, -1);
                g_free(text);
            }
    }
    picker_groups(picker);
    /* Groups with a chosen channel open; the rest stay folded. */
    GtkTreeIter group;
    gboolean more = gtk_tree_model_get_iter_first(model, &group);
    while (more) {
        gboolean partial = FALSE, active = FALSE;
        gtk_tree_model_get(model, &group, PICK_ACTIVE, &active, PICK_PARTIAL, &partial, -1);
        GtkTreePath *path = gtk_tree_model_get_path(model, &group);
        if (active || partial)
            gtk_tree_view_expand_row(GTK_TREE_VIEW(picker->tree), path, FALSE);
        gtk_tree_path_free(path);
        more = gtk_tree_model_iter_next(model, &group);
    }
}

static void
picker_toggled(GtkCellRendererToggle *renderer, gchar *path_text, gpointer data)
{
    (void)renderer;
    ChannelPicker *picker = data;
    GtkTreeModel *model = GTK_TREE_MODEL(picker->store);
    GtkTreeIter row, child;
    if (!gtk_tree_model_get_iter_from_string(model, &row, path_text))
        return;
    gint number = 0;
    gboolean active = FALSE;
    gtk_tree_model_get(model, &row, PICK_NUMBER, &number, PICK_ACTIVE, &active, -1);
    if (number > 0) {
        picker->selected ^= 1u << (number - 1);
    } else {
        gboolean children = gtk_tree_model_iter_children(model, &child, &row);
        while (children) {
            gint member = 0;
            gtk_tree_model_get(model, &child, PICK_NUMBER, &member, -1);
            if (member > 0) {
                if (active)
                    picker->selected &= ~(1u << (member - 1));
                else
                    picker->selected |= 1u << (member - 1);
            }
            children = gtk_tree_model_iter_next(model, &child);
        }
    }
    picker_groups(picker);
    if (picker->changed != NULL)
        picker->changed(picker->button, picker->user);
}

static void
picker_free(gpointer data)
{
    ChannelPicker *picker = data;
    g_clear_object(&picker->store);
    g_clear_pointer(&picker->channels, json_array_unref);
    g_free(picker);
}

GtkWidget *
radios_ui_channel_picker_new(const gchar *name)
{
    ChannelPicker *picker = g_new0(ChannelPicker, 1);
    picker->button = gtk_menu_button_new();
    gtk_widget_set_name(picker->button, name);
    picker->caption = gtk_label_new("Kanal seçilmedi ▾");
    gtk_label_set_xalign(GTK_LABEL(picker->caption), 0.0f);
    gtk_container_add(GTK_CONTAINER(picker->button), picker->caption);
    picker->store = gtk_tree_store_new(PICK_COLUMNS, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_INT);
    picker->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(picker->store));
    gchar *tree_name = g_strconcat(name, "-tree", NULL);
    gtk_widget_set_name(picker->tree, tree_name);
    g_free(tree_name);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(picker->tree), FALSE);
    GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new();
    gtk_tree_view_column_pack_start(column, toggle, FALSE);
    gtk_tree_view_column_add_attribute(column, toggle, "active", PICK_ACTIVE);
    gtk_tree_view_column_add_attribute(column, toggle, "inconsistent", PICK_PARTIAL);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(column, text, TRUE);
    gtk_tree_view_column_add_attribute(column, text, "text", PICK_TEXT);
    gtk_tree_view_append_column(GTK_TREE_VIEW(picker->tree), column);
    g_signal_connect(toggle, "toggled", G_CALLBACK(picker_toggled), picker);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_widget_set_size_request(scroll, 320, -1);
    gtk_container_add(GTK_CONTAINER(scroll), picker->tree);
    GtkWidget *popover = gtk_popover_new(picker->button);
    gtk_container_set_border_width(GTK_CONTAINER(popover), 6);
    gtk_container_add(GTK_CONTAINER(popover), scroll);
    gtk_widget_show_all(scroll);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(picker->button), popover);
    g_object_set_data_full(G_OBJECT(picker->button), "radios-channel-picker", picker, picker_free);
    picker_render(picker);
    return picker->button;
}

void
radios_ui_channel_picker_set_channels(GtkWidget *widget, JsonArray *channels)
{
    ChannelPicker *picker = picker_get(widget);
    if (picker == NULL)
        return;
    g_clear_pointer(&picker->channels, json_array_unref);
    if (channels != NULL)
        picker->channels = json_array_ref(channels);
    picker_render(picker);
}

void
radios_ui_channel_picker_set_selected(GtkWidget *widget, JsonArray *numbers)
{
    ChannelPicker *picker = picker_get(widget);
    if (picker == NULL)
        return;
    picker->selected = 0;
    for (guint i = 0; numbers != NULL && i < json_array_get_length(numbers); ++i) {
        JsonNode *node = json_array_get_element(numbers, i);
        gint64 number = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 ? json_node_get_int(node) : 0;
        if (number >= 1 && number <= 32)
            picker->selected |= 1u << (number - 1);
    }
    picker_render(picker);
}

JsonArray *
radios_ui_channel_picker_selected(GtkWidget *widget)
{
    ChannelPicker *picker = picker_get(widget);
    JsonArray *numbers = json_array_new();
    for (guint number = 1; picker != NULL && number <= 32; ++number)
        if (picker->selected & (1u << (number - 1)))
            json_array_add_int_element(numbers, number);
    return numbers;
}

const gchar *
radios_ui_channel_picker_agreed_label(GtkWidget *widget)
{
    ChannelPicker *picker = picker_get(widget);
    const gchar *agreed = NULL;
    for (guint number = 1; picker != NULL && number <= 32; ++number) {
        if (!(picker->selected & (1u << (number - 1))))
            continue;
        const gchar *label = picker_text(picker_channel(picker, number), "label");
        if (g_strcmp0(label, "dj") && g_strcmp0(label, "talk") && g_strcmp0(label, "music"))
            continue;
        if (agreed != NULL && g_strcmp0(agreed, label))
            return NULL;
        agreed = label;
    }
    return agreed;
}

void
radios_ui_channel_picker_toggle(GtkWidget *widget, guint number)
{
    ChannelPicker *picker = picker_get(widget);
    if (picker == NULL || number < 1 || number > 32)
        return;
    picker->selected ^= 1u << (number - 1);
    picker_render(picker);
    if (picker->changed != NULL)
        picker->changed(picker->button, picker->user);
}

void
radios_ui_channel_picker_set_changed(GtkWidget *widget, void (*changed)(GtkWidget *picker, gpointer user), gpointer user)
{
    ChannelPicker *picker = picker_get(widget);
    if (picker == NULL)
        return;
    picker->changed = changed;
    picker->user = user;
}
