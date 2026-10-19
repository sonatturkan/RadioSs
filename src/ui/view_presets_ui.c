#include "radios/view_presets_ui.h"
#include "radios/settings_sync.h"

/* Settings › Görünüm: saved views of the workstation settings, kept in the
 * station database by settings_sync (R3.4). */
typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *list, *name, *save, *remove, *status;
    gboolean bound, filling;
} Presets;

static void report(Presets *p, const gchar *text, gboolean failed)
{
    gtk_label_set_text(GTK_LABEL(p->status), text ? text : "");
    GtkStyleContext *style = gtk_widget_get_style_context(p->status);
    if (failed) gtk_style_context_add_class(style, "error"); else gtk_style_context_remove_class(style, "error");
}
static void report_name(Presets *p, const gchar *format, const gchar *name)
{
    gchar *text = g_strdup_printf(format, name); report(p, text, FALSE); g_free(text);
}
static void sensitivity(Presets *p)
{
    gboolean available = radios_settings_views_available(p->ctx);
    gtk_widget_set_sensitive(p->list, available);
    gtk_widget_set_sensitive(p->name, available);
    gtk_widget_set_sensitive(p->save, available);
    gtk_widget_set_sensitive(p->remove, available && gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->list)));
}
/* Rebuilds the list only when the names changed, so that applying the
 * chosen view never rebuilds the list it was chosen from. */
static void fill(gpointer data)
{
    Presets *p = data; gchar *active = NULL; gchar **names = radios_settings_views_list(p->ctx, &active);
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(p->list)); GtkTreeIter iter;
    gboolean same = TRUE, valid = gtk_tree_model_get_iter_first(model, &iter); guint count = 0;
    for (; names[count] && same; ++count) {
        gchar *shown = NULL;
        if (valid) gtk_tree_model_get(model, &iter, 0, &shown, -1);
        same = valid && !g_strcmp0(shown, names[count]); g_free(shown);
        if (valid) valid = gtk_tree_model_iter_next(model, &iter);
    }
    same = same && !names[count] && !valid;
    p->filling = TRUE;
    if (!same) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(p->list));
        for (guint i = 0; names[i]; ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->list), names[i], names[i]);
    }
    if (g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->list)), active) &&
        !(active && gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->list), active)))
        gtk_combo_box_set_active(GTK_COMBO_BOX(p->list), -1);
    p->filling = FALSE;
    g_strfreev(names); g_free(active);
    if (!p->bound) report(p, "Veritabanı bağlantısı yok.", TRUE);
    else if (!radios_settings_views_available(p->ctx)) report(p, "Görünümler yükleniyor…", FALSE);
    sensitivity(p);
}
static void chosen(GtkComboBox *combo, gpointer data)
{
    Presets *p = data;
    if (p->filling) return;
    const gchar *id = gtk_combo_box_get_active_id(combo);
    if (!id) { sensitivity(p); return; }
    gchar *name = g_strdup(id); GError *error = NULL;
    gtk_entry_set_text(GTK_ENTRY(p->name), name);
    if (radios_settings_views_apply(p->ctx, name, &error)) report_name(p, "“%s” uygulandı.", name);
    else { report(p, error->message, TRUE); g_clear_error(&error); }
    g_free(name); sensitivity(p);
}
static void save(GtkWidget *widget, gpointer data)
{
    (void)widget; Presets *p = data;
    const gchar *typed = gtk_entry_get_text(GTK_ENTRY(p->name)), *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->list));
    gchar *name = g_strstrip(g_strdup(*typed ? typed : id ? id : "")); GError *error = NULL;
    if (radios_settings_views_save(p->ctx, name, &error)) { gtk_entry_set_text(GTK_ENTRY(p->name), name); report_name(p, "“%s” kaydedildi.", name); }
    else { report(p, error->message, TRUE); g_clear_error(&error); }
    g_free(name);
}
static void remove_view(GtkButton *button, gpointer data)
{
    (void)button; Presets *p = data;
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->list));
    if (!id) return;
    gchar *name = g_strdup(id); GError *error = NULL;
    if (radios_settings_views_delete(p->ctx, name, &error)) { gtk_entry_set_text(GTK_ENTRY(p->name), ""); report_name(p, "“%s” silindi.", name); }
    else { report(p, error->message, TRUE); g_clear_error(&error); }
    g_free(name);
}
GtkWidget *radios_view_presets_section_new(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx != NULL, NULL);
    Presets *p = g_new0(Presets, 1); p->ctx = ctx;
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_name(section, "preferences-view-presets");
    g_object_set_data_full(G_OBJECT(section), "radios-view-presets", p, g_free);
    GtkWidget *title = gtk_label_new("Kayıtlı görünümler"); gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_box_pack_start(GTK_BOX(section), title, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_widget_set_name(row, "preferences-view-presets-row");
    gtk_box_pack_start(GTK_BOX(section), row, FALSE, FALSE, 0);
    p->list = gtk_combo_box_text_new(); gtk_widget_set_name(p->list, "preferences-view-presets-list");
    gtk_widget_set_size_request(p->list, 200, -1);
    gtk_widget_set_tooltip_text(p->list, "Seçilen görünüm hemen uygulanır");
    p->name = gtk_entry_new(); gtk_widget_set_name(p->name, "preferences-view-presets-name");
    gtk_entry_set_max_length(GTK_ENTRY(p->name), RADIOS_SETTINGS_VIEW_NAME_MAX); gtk_entry_set_width_chars(GTK_ENTRY(p->name), 18);
    gtk_widget_set_tooltip_text(p->name, "Görünüm adı; seçili görünümün adı onun üstüne kaydeder");
    p->save = gtk_button_new_with_label("Kaydet"); gtk_widget_set_name(p->save, "preferences-view-presets-save");
    gtk_widget_set_tooltip_text(p->save, "Geçerli görünüm ayarlarını bu adla veritabanına kaydet");
    p->remove = gtk_button_new_with_label("Sil"); gtk_widget_set_name(p->remove, "preferences-view-presets-delete");
    gtk_widget_set_tooltip_text(p->remove, "Seçili görünümü sil");
    p->status = gtk_label_new(""); gtk_widget_set_name(p->status, "preferences-view-presets-status");
    gtk_label_set_xalign(GTK_LABEL(p->status), 0); gtk_label_set_ellipsize(GTK_LABEL(p->status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(p->status, TRUE);
    gtk_box_pack_start(GTK_BOX(row), p->list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->remove, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), p->status, TRUE, TRUE, 0);
    g_signal_connect(p->list, "changed", G_CALLBACK(chosen), p);
    g_signal_connect(p->name, "activate", G_CALLBACK(save), p);
    g_signal_connect(p->save, "clicked", G_CALLBACK(save), p);
    g_signal_connect(p->remove, "clicked", G_CALLBACK(remove_view), p);
    p->bound = radios_settings_views_watch(ctx, section, fill, p);
    fill(p);
    gtk_widget_show_all(section);
    return section;
}
