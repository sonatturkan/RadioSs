#include "radios/widgets.h"
#include "radios/data.h"
#include "radios/playout_ui.h"
#include "radios/automation_calendar.h"
#include <math.h>
#include <stdio.h>

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *stack, *tabs[3], *name, *summary_name, *summary_mode;
    GtkWidget *track_gap, *artist_gap, *summary_gaps, *seed, *status;
    GtkWidget *sources_box, *sources_empty, *source_add, *summary_sources;
    GtkWidget *decks_box, *summary_decks, *summary_saved;
    GtkWidget *summary_candidates;
    GtkWidget *weekdays[7];
    GPtrArray *sources;
    GHashTable *fields;
    GHashTable *selected_decks;
    GHashTable *deck_policies;
    gboolean selecting, ready, updating;
    GtkWidget *widget;
    RadioSDocument *document;
    gboolean dirty, applying;
    GtkWidget *preview_button, *preview_clear, *preview_apply, *preview_download, *preview_table, *preview_empty;
    GtkWidget *live_start, *live_pause, *live_stop, *live_next, *live_state;
    JsonNode *preview;
    guint preview_generation, playout_timer;
    gboolean preview_busy, disposed;
} AutomationView;

typedef struct {
    AutomationView *view;
    GtkWidget *row, *playlist, *weight, *count, *start, *end, *days, *up, *down;
    gchar *id;
} AutomationSource;

static void automation_update_controls(AutomationView *view);
static void automation_refresh_source_choices(AutomationView *view);
static JsonNode *automation_config(AutomationView *view);
static void automation_collect_playlist_ids(GtkWidget *widget, GHashTable *names);
static void automation_preview_clear(GtkButton *button, gpointer data);
static void automation_preview_apply(GtkButton *button, gpointer data);
static void automation_preview_download(GtkButton *button, gpointer data);
static void automation_live_action(GtkButton *button, gpointer data);
static gboolean automation_playout_tick(gpointer data);
static void automation_preview_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data);

static void automation_dirty(AutomationView *view)
{
    view->dirty = TRUE;
    if (view->preview || view->preview_busy) automation_preview_clear(NULL, view);
    if (view->summary_saved) gtk_label_set_text(GTK_LABEL(view->summary_saved), "Kaydedilmemiş değişiklikler");
    if (view->document && radios_data_document_ready(view->document)) radios_data_document_dirty(view->document, TRUE);
}

static void automation_source_free(gpointer data)
{ AutomationSource *source = data; g_free(source->id); g_free(source); }

static void
automation_free(gpointer data)
{
    AutomationView *view = data;
    g_ptr_array_unref(view->sources);
    g_hash_table_unref(view->fields);
    g_hash_table_unref(view->selected_decks);
    g_hash_table_unref(view->deck_policies);
    g_clear_pointer(&view->preview, json_node_free);
    g_free(view);
}

static void
automation_notice(AutomationView *view, const gchar *message)
{
    gtk_label_set_text(GTK_LABEL(view->status), message);
    if (view->ctx->notify != NULL) view->ctx->notify(view->ctx->notify_data, message);
}

static void
automation_changed(GtkWidget *widget, gpointer data)
{
    AutomationView *view = data;
    (void)widget;
    if (view->ready && !view->updating && !view->applying) automation_dirty(view);
    if (view->ready) automation_update_controls(view);
}

/* Keep long playlist names and option captions inside the shared side panel. */
static void
automation_compact_combo(GtkWidget *widget)
{
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(widget));
    for (GList *item = cells; item; item = item->next)
        if (GTK_IS_CELL_RENDERER_TEXT(item->data))
            g_object_set(item->data, "ellipsize", PANGO_ELLIPSIZE_END,
                         "max-width-chars", 20, NULL);
    g_list_free(cells);
    gtk_widget_set_hexpand(widget, TRUE);
}

static GtkWidget *
automation_register(AutomationView *view, const gchar *name, GtkWidget *widget)
{
    if (GTK_IS_COMBO_BOX(widget)) automation_compact_combo(widget);
    g_hash_table_insert(view->fields, g_strdup(name), widget);
    GString *id = g_string_new("automation-");
    for (const gchar *cursor = name; *cursor != '\0'; ++cursor) {
        if (g_ascii_isupper(*cursor)) g_string_append_c(id, '-');
        g_string_append_c(id, g_ascii_tolower(*cursor));
    }
    gtk_widget_set_name(widget, id->str); g_string_free(id, TRUE);
    if (GTK_IS_ENTRY(widget) && !GTK_IS_SPIN_BUTTON(widget)) {
        if (g_str_equal(name, "planDate")) gtk_entry_set_max_length(GTK_ENTRY(widget), 10);
        else if (g_str_equal(name, "startTime") || g_str_equal(name, "windowStart") ||
                 g_str_equal(name, "windowEnd")) gtk_entry_set_max_length(GTK_ENTRY(widget), 5);
    }
    const gchar *signal = GTK_IS_SPIN_BUTTON(widget) ? "value-changed" :
        (GTK_IS_TOGGLE_BUTTON(widget) ? "toggled" : "changed");
    g_signal_connect(widget, signal, G_CALLBACK(automation_changed), view);
    return widget;
}

static GtkWidget *automation_control(AutomationView *view, const gchar *name)
{ return g_hash_table_lookup(view->fields, name); }
static gint automation_deck_compare(gconstpointer left, gconstpointer right)
{
    gint64 first = g_ascii_strtoll(left, NULL, 10), second = g_ascii_strtoll(right, NULL, 10);
    return first < second ? -1 : first > second ? 1 : g_strcmp0(left, right);
}

static void
automation_update_controls(AutomationView *view)
{
    if (view->updating || view->applying) return;
    view->updating = TRUE;
    gint transition = gtk_combo_box_get_active(GTK_COMBO_BOX(automation_control(view, "transition")));
    gint boundary = gtk_combo_box_get_active(GTK_COMBO_BOX(automation_control(view, "boundaryTransition")));
    gint fallback = gtk_combo_box_get_active(GTK_COMBO_BOX(automation_control(view, "fallbackMode")));
    gint mode = gtk_combo_box_get_active(GTK_COMBO_BOX(automation_control(view, "sourceMode")));
    gboolean jingle = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(automation_control(view, "jingleEnabled")));
    gtk_widget_set_sensitive(automation_control(view, "overlap"), transition == 1 || transition == 2);
    gtk_widget_set_sensitive(automation_control(view, "boundaryOverlap"), boundary == 2 || boundary == 3);
    gtk_widget_set_sensitive(automation_control(view, "fallbackPlaylist"), fallback == 2);
    const gchar *jingle_fields[] = { "jinglePlaylist", "jingleEvery", "jingleDeck" };
    for (guint i = 0; i < G_N_ELEMENTS(jingle_fields); ++i)
        gtk_widget_set_sensitive(automation_control(view, jingle_fields[i]), jingle);
    for (guint i = 0; i < view->sources->len; ++i) {
        AutomationSource *source = g_ptr_array_index(view->sources, i);
        gtk_widget_set_sensitive(source->weight, mode == 1);
        gtk_widget_set_sensitive(source->count, mode == 0);
        gtk_widget_set_sensitive(source->up, i > 0);
        gtk_widget_set_sensitive(source->down, i + 1 < view->sources->len);
    }
    const gchar *playlist_fields[] = { "jinglePlaylist", "fallbackPlaylist" };
    for (guint i = 0; i < G_N_ELEMENTS(playlist_fields); ++i) {
        GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT(automation_control(view, playlist_fields[i]));
        gchar *active = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)));
        gtk_combo_box_text_remove_all(combo);
        gtk_combo_box_text_append(combo, "", "Playlist seç");
        if (view->ctx->data) {
            GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
            automation_collect_playlist_ids(GTK_WIDGET(view->ctx->parent), names);
            GList *ids = g_hash_table_get_keys(names); ids = g_list_sort(ids, (GCompareFunc)g_strcmp0);
            for (GList *item = ids; item; item = item->next)
                gtk_combo_box_text_append(combo, item->data, g_hash_table_lookup(names, item->data));
            if (active && *active && !g_hash_table_contains(names, active))
                gtk_combo_box_text_append(combo, active, "Kaydedilmiş playlist · kayıt seçimi bekleniyor");
            g_list_free(ids); g_hash_table_unref(names);
        } else
        for (guint j = 0; j < view->sources->len; ++j) {
            AutomationSource *source = g_ptr_array_index(view->sources, j);
            const gchar *name = gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(source->playlist))));
            const gchar *id = view->ctx->data ? gtk_combo_box_get_active_id(GTK_COMBO_BOX(source->playlist)) : name;
            if (id && *id != '\0') {
                gchar *caption = g_strdup_printf("%s (%s)", name, view->ctx->data ? "medya doğrulanmadı" : "taslak / doğrulanmadı");
                gtk_combo_box_text_append(combo, id, caption); g_free(caption);
            }
        }
        if (active == NULL || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), active))
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), "");
        g_free(active);
    }
    gtk_widget_set_visible(view->sources_empty, view->sources->len == 0);
    gtk_widget_set_sensitive(view->source_add, view->sources->len < 50);
    gchar *text = g_strdup_printf("%u%s", view->sources->len, view->sources->len > 0 ? " (taslak)" : "");
    gtk_label_set_text(GTK_LABEL(view->summary_sources), text); g_free(text);
    view->updating = FALSE;
}

static gboolean
automation_time_valid(const gchar *text)
{ return g_regex_match_simple("^([01][0-9]|2[0-3]):[0-5][0-9]$", text, 0, 0); }

static gboolean
automation_number_valid(AutomationView *view, GtkWidget *input)
{
    if (!gtk_widget_is_sensitive(input)) return TRUE;
    gchar *end = NULL;
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(input));
    gdouble number = g_strtod(text, &end);
    GtkAdjustment *adjustment = gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(input));
    gboolean valid = text != end && end != NULL && *end == '\0' && isfinite(number) &&
        number >= gtk_adjustment_get_lower(adjustment) && number <= gtk_adjustment_get_upper(adjustment);
    if (gtk_spin_button_get_digits(GTK_SPIN_BUTTON(input)) == 0 && number != floor(number)) valid = FALSE;
    if (!valid) {
        automation_notice(view, "Sayısal alan geçersiz veya izin verilen aralığın dışında.");
        gtk_widget_grab_focus(input);
        return FALSE;
    }
    gtk_spin_button_update(GTK_SPIN_BUTTON(input));
    return TRUE;
}

static gboolean
automation_validate(AutomationView *view)
{
    const gchar *required[] = { "name", "seed" };
    for (guint i = 0; i < G_N_ELEMENTS(required); ++i) {
        GtkWidget *input = automation_control(view, required[i]);
        gchar *text = g_strdup(gtk_entry_get_text(GTK_ENTRY(input))); g_strstrip(text);
        gboolean valid = *text != '\0'; g_free(text);
        if (!valid) { automation_notice(view, i == 0 ? "Rotasyon adı gerekli." : "Rastgelelik anahtarı gerekli."); gtk_widget_grab_focus(input); return FALSE; }
    }
    GtkWidget *date = automation_control(view, "planDate");
    const gchar *date_text = gtk_entry_get_text(GTK_ENTRY(date));
    guint year = 0, month = 0, day = 0;
    if (!g_regex_match_simple("^[0-9]{4}-[0-9]{2}-[0-9]{2}$", date_text, 0, 0) ||
        sscanf(date_text, "%u-%u-%u", &year, &month, &day) != 3 || year > 9999 ||
        !g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year)) {
        automation_notice(view, "Plan tarihi geçerli bir YYYY-AA-GG tarihi olmalı."); gtk_widget_grab_focus(date); return FALSE;
    }
    const gchar *times[] = { "startTime", "windowStart", "windowEnd" };
    for (guint i = 0; i < G_N_ELEMENTS(times); ++i) {
        GtkWidget *input = automation_control(view, times[i]);
        if (!automation_time_valid(gtk_entry_get_text(GTK_ENTRY(input)))) {
            automation_notice(view, "UTC saatini 00:00–23:59 aralığında SS:DD biçiminde girin."); gtk_widget_grab_focus(input); return FALSE;
        }
    }
    gboolean day_selected = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(view->weekdays); ++i)
        day_selected |= gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(view->weekdays[i]));
    if (!day_selected) { automation_notice(view, "En az bir yayın günü seçin."); return FALSE; }
    GHashTableIter fields_iter;
    gpointer field_value;
    g_hash_table_iter_init(&fields_iter, view->fields);
    while (g_hash_table_iter_next(&fields_iter, NULL, &field_value)) {
        GtkWidget *input = field_value;
        if (!GTK_IS_SPIN_BUTTON(input) || !gtk_widget_is_sensitive(input)) continue;
        if (!automation_number_valid(view, input)) return FALSE;
    }
    for (guint i = 0; i < view->sources->len; ++i) {
        AutomationSource *source = g_ptr_array_index(view->sources, i);
        GtkWidget *entry = gtk_bin_get_child(GTK_BIN(source->playlist));
        gchar *name = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))); g_strstrip(name);
        gboolean valid = *name != '\0'; g_free(name);
        if (view->ctx->data) {
            const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(source->playlist));
            valid = id != NULL && *id != '\0';
        }
        if (!valid || !automation_time_valid(gtk_entry_get_text(GTK_ENTRY(source->start))) ||
            !automation_time_valid(gtk_entry_get_text(GTK_ENTRY(source->end)))) {
            automation_notice(view, "Her kaynak için playlist adı/kimliği ve geçerli UTC başlangıç/bitiş saati gerekli."); gtk_widget_grab_focus(valid ? source->start : entry); return FALSE;
        }
        if (!automation_number_valid(view, source->weight) ||
            !automation_number_valid(view, source->count)) return FALSE;
    }
    const gchar *conditional[] = { "jinglePlaylist", "fallbackPlaylist" };
    for (guint i = 0; i < G_N_ELEMENTS(conditional); ++i) {
        GtkWidget *combo = automation_control(view, conditional[i]);
        const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
        if (gtk_widget_is_sensitive(combo) && (id == NULL || *id == '\0')) {
            automation_notice(view, i == 0 ? "Jingle etkin: jingle playlisti seçin." : "Yedek playlist politikası için bir playlist seçin.");
            gtk_widget_grab_focus(combo); return FALSE;
        }
    }
    GList *deck_rows = gtk_container_get_children(GTK_CONTAINER(view->decks_box));
    gboolean deck_values_valid = TRUE;
    for (GList *row = deck_rows; row && deck_values_valid; row = row->next) {
        GtkWidget *overlap = g_object_get_data(G_OBJECT(row->data), "automation-deck-overlap");
        if (overlap) deck_values_valid = automation_number_valid(view, overlap);
    }
    g_list_free(deck_rows);
    if (!deck_values_valid) return FALSE;
    return TRUE;
}

static void
automation_json_string(GString *json, const gchar *text)
{
    g_string_append_c(json, '"');
    for (const guchar *cursor = (const guchar *)(text != NULL ? text : ""); *cursor != 0; ++cursor) {
        if (*cursor == '"' || *cursor == '\\') { g_string_append_c(json, '\\'); g_string_append_c(json, (gchar)*cursor); }
        else if (*cursor < 32) g_string_append_printf(json, "\\u%04x", *cursor);
        else g_string_append_c(json, (gchar)*cursor);
    }
    g_string_append_c(json, '"');
}

static void
automation_export_response(GtkDialog *dialog, gint response, gpointer data)
{
    AutomationView *view = data;
    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        GError *error = NULL;
        if (filename != NULL && g_file_set_contents(filename,
            g_object_get_data(G_OBJECT(dialog), "automation-export-data"), -1, &error))
            automation_notice(view, g_object_get_data(G_OBJECT(dialog), "automation-plan-export") ? "Yayın planı JSON dosyasına kaydedildi." : "Rotasyon kuralları JSON dosyasına kaydedildi.");
        else automation_notice(view, error != NULL ? error->message : "Dosya adı seçilmedi.");
        g_clear_error(&error); g_free(filename);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
/* The save dialogs belong to the main window (destroy-with-parent) but must
 * not outlive the rules page whose view their response handler uses: closing
 * the page closes an open dialog.  Without this a page rebuilt while the
 * dialog was open left the handler with a freed view, or (when the handler
 * was tied to the page) a dialog whose buttons no longer closed it. */
static void
automation_dialog_follow_page(AutomationView *view, GtkWidget *dialog)
{
    g_signal_connect_object(view->widget, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
}

static void
automation_export(GtkButton *button, gpointer data)
{
    AutomationView *view = data; (void)button;
    if (!automation_validate(view)) return;
    GString *json = g_string_new("{\n  \"format\": \"RadioS GUI taslağı\",\n  \"saved_to_database\": false,\n  \"rules\": {\n");
    GList *keys = g_hash_table_get_keys(view->fields);
    keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
    for (GList *item = keys; item != NULL; item = item->next) {
        const gchar *name = item->data;
        GtkWidget *input = automation_control(view, name);
        g_string_append(json, "    "); automation_json_string(json, name); g_string_append(json, ": ");
        if (GTK_IS_SPIN_BUTTON(input)) {
            gchar number[G_ASCII_DTOSTR_BUF_SIZE];
            g_ascii_dtostr(number, sizeof(number), gtk_spin_button_get_value(GTK_SPIN_BUTTON(input)));
            g_string_append(json, number);
        } else if (GTK_IS_TOGGLE_BUTTON(input))
            g_string_append(json, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(input)) ? "true" : "false");
        else if (GTK_IS_ENTRY(input)) automation_json_string(json, gtk_entry_get_text(GTK_ENTRY(input)));
        else if (GTK_IS_COMBO_BOX_TEXT(input)) {
            gchar *text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(input));
            automation_json_string(json, text); g_free(text);
        }
        g_string_append(json, ",\n");
    }
    g_list_free(keys);
    g_string_append(json, "    \"weekdays\": ["); gboolean first = TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(view->weekdays); ++i)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(view->weekdays[i]))) {
            g_string_append_printf(json, "%s%u", first ? "" : ", ", i + 1); first = FALSE;
        }
    g_string_append(json, "],\n    \"sources\": [");
    for (guint i = 0; i < view->sources->len; ++i) {
        AutomationSource *source = g_ptr_array_index(view->sources, i);
        g_string_append(json, i > 0 ? ",\n      {\"playlist_reference\": " : "\n      {\"playlist_reference\": ");
        automation_json_string(json, gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(source->playlist)))));
        g_string_append_printf(json, ", \"weight\": %d, \"count\": %d, \"start\": ",
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(source->weight)),
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(source->count)));
        automation_json_string(json, gtk_entry_get_text(GTK_ENTRY(source->start)));
        g_string_append(json, ", \"end\": "); automation_json_string(json, gtk_entry_get_text(GTK_ENTRY(source->end)));
        g_string_append(json, ", \"days\": ");
        gchar *days = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(source->days));
        automation_json_string(json, days); g_free(days);
        g_string_append(json, ", \"verified\": false}");
    }
    g_string_append(json, "\n    ],\n    \"decks\": [");
    GList *deck_ids = g_hash_table_get_keys(view->selected_decks);
    deck_ids = g_list_sort(deck_ids, automation_deck_compare);
    for (GList *item = deck_ids; item != NULL; item = item->next) {
        if (item != deck_ids) g_string_append(json, ", ");
        automation_json_string(json, item->data);
    }
    g_list_free(deck_ids);
    JsonNode *configuration = automation_config(view);
    gchar *policies = json_to_string(json_object_get_member(json_node_get_object(configuration), "deckTransitions"), FALSE);
    g_string_append_printf(json, "],\n    \"deckTransitions\": %s\n  }\n}\n", policies);
    g_free(policies); json_node_free(configuration);
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Rotasyon GUI taslağını dışa aktar", view->ctx->parent,
        GTK_FILE_CHOOSER_ACTION_SAVE, "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_widget_set_name(dialog, "automation-export-dialog");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    radios_ui_file_chooser_default_folder(GTK_FILE_CHOOSER(dialog), view->ctx);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "radio-s-rules.json");
    g_object_set_data_full(G_OBJECT(dialog), "automation-export-data", g_string_free(json, FALSE), g_free);
    g_signal_connect(dialog, "response", G_CALLBACK(automation_export_response), view);
    automation_dialog_follow_page(view, dialog);
    gtk_widget_show(dialog);
}

static void
automation_calendar_response(GtkDialog *dialog, gint response, gpointer data)
{
    GtkWidget *entry = data;
    if (response == GTK_RESPONSE_ACCEPT) {
        GtkCalendar *calendar = g_object_get_data(G_OBJECT(dialog), "automation-calendar");
        guint year, month, day;
        gtk_calendar_get_date(calendar, &year, &month, &day);
        gchar *date = g_strdup_printf("%04u-%02u-%02u", year, month + 1, day);
        gtk_entry_set_text(GTK_ENTRY(entry), date); g_free(date);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
automation_calendar(GtkButton *button, gpointer data)
{
    AutomationView *view = data; (void)button;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Plan tarihi · UTC", view->ctx->parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL,
        "Seç", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *calendar = gtk_calendar_new();
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(automation_control(view, "planDate")));
    guint year = 0, month = 0, day = 0;
    if (g_regex_match_simple("^[0-9]{4}-[0-9]{2}-[0-9]{2}$", text, 0, 0) &&
        sscanf(text, "%u-%u-%u", &year, &month, &day) == 3 && year <= 9999 &&
        g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year)) {
        gtk_calendar_select_month(GTK_CALENDAR(calendar), month - 1, year);
        gtk_calendar_select_day(GTK_CALENDAR(calendar), day);
    }
    gtk_widget_set_name(dialog, "automation-calendar-dialog");
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), calendar, TRUE, TRUE, 20);
    g_object_set_data(G_OBJECT(dialog), "automation-calendar", calendar);
    g_signal_connect(dialog, "response", G_CALLBACK(automation_calendar_response), automation_control(view, "planDate"));
    /* The response writes into the page's date entry. */
    automation_dialog_follow_page(view, dialog);
    gtk_widget_show_all(dialog);
}

static void
automation_deck_toggled(GtkToggleButton *button, gpointer data)
{
    AutomationView *view = data;
    if (!view->applying && !view->updating) automation_dirty(view);
    const gchar *id = g_object_get_data(G_OBJECT(button), "automation-deck-id");
    const gchar *name = gtk_button_get_label(GTK_BUTTON(button));
    if (gtk_toggle_button_get_active(button))
        g_hash_table_replace(view->selected_decks, g_strdup(id), g_strdup(name));
    else g_hash_table_remove(view->selected_decks, id);
    GtkWidget *mode = g_object_get_data(G_OBJECT(button), "automation-deck-transition");
    GtkWidget *overlap = g_object_get_data(G_OBJECT(button), "automation-deck-overlap");
    if (mode && overlap) {
        gboolean selected = gtk_toggle_button_get_active(button);
        gint active = gtk_combo_box_get_active(GTK_COMBO_BOX(mode));
        gtk_widget_set_sensitive(mode, selected);
        gtk_widget_set_sensitive(overlap, selected && (active == 2 || active == 3));
    }
    GString *text = g_string_new("");
    GHashTableIter iter; gpointer value;
    g_hash_table_iter_init(&iter, view->selected_decks);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        if (text->len > 0) g_string_append(text, ", ");
        g_string_append(text, value);
    }
    gtk_label_set_text(GTK_LABEL(view->summary_decks), text->len > 0 ? text->str : "Atanmadı");
    g_string_free(text, TRUE);
}
static void automation_deck_policy_changed(GtkWidget *widget, gpointer data)
{
    AutomationView *view = data;
    if (view->updating || view->applying) return;
    GtkWidget *row = gtk_widget_get_parent(widget);
    GtkWidget *mode = g_object_get_data(G_OBJECT(row), "automation-deck-transition");
    GtkWidget *overlap = g_object_get_data(G_OBJECT(row), "automation-deck-overlap");
    GtkWidget *check = g_object_get_data(G_OBJECT(row), "automation-deck-check");
    const gchar *identifier = g_object_get_data(G_OBJECT(check), "automation-deck-id");
    gint active = gtk_combo_box_get_active(GTK_COMBO_BOX(mode));
    if (!automation_number_valid(view, overlap)) return;
    view->updating = TRUE;
    if (active != 2 && active != 3) gtk_spin_button_set_value(GTK_SPIN_BUTTON(overlap), 0);
    gtk_widget_set_sensitive(overlap, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check)) && (active == 2 || active == 3));
    if (active > 0) {
        JsonObject *policy = json_object_new();
        json_object_set_string_member(policy, "deck_id", identifier);
        json_object_set_int_member(policy, "transition", active - 1);
        json_object_set_double_member(policy, "overlap", gtk_spin_button_get_value(GTK_SPIN_BUTTON(overlap)));
        g_hash_table_replace(view->deck_policies, g_strdup(identifier), policy);
    } else g_hash_table_remove(view->deck_policies, identifier);
    view->updating = FALSE; automation_dirty(view);
}

static void
automation_collect_players(GtkWidget *widget, GHashTable *players)
{
    const gchar *id = g_object_get_data(G_OBJECT(widget), "radios-player-id");
    const gchar *name = g_object_get_data(G_OBJECT(widget), "radios-player-name");
    if (id != NULL && name != NULL) g_hash_table_replace(players, g_strdup(id), g_strdup(name));
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *item = children; item != NULL; item = item->next)
            automation_collect_players(item->data, players);
        g_list_free(children);
    }
}

static void
automation_collect_playlist_drafts(GtkWidget *widget, GHashTable *names)
{
    GPtrArray *drafts = g_object_get_data(G_OBJECT(widget), "radios-playlist-drafts");
    if (drafts != NULL)
        for (guint i = 0; i < drafts->len; ++i)
            g_hash_table_add(names, g_strdup(g_ptr_array_index(drafts, i)));
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *item = children; item != NULL; item = item->next)
            automation_collect_playlist_drafts(item->data, names);
        g_list_free(children);
    }
}

static void
automation_collect_playlist_ids(GtkWidget *widget, GHashTable *names)
{
    GHashTable *records = g_object_get_data(G_OBJECT(widget), "radios-playlists");
    if (records) {
        GHashTableIter iter; gpointer id, name; g_hash_table_iter_init(&iter, records);
        while (g_hash_table_iter_next(&iter, &id, &name))
            g_hash_table_replace(names, g_strdup(id), g_strdup(name));
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *item = children; item; item = item->next) automation_collect_playlist_ids(item->data, names);
        g_list_free(children);
    }
}

static void
automation_refresh_source_choices(AutomationView *view)
{
    GHashTable *names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, view->ctx->data ? g_free : NULL);
    if (view->ctx->data) automation_collect_playlist_ids(GTK_WIDGET(view->ctx->parent), names);
    else automation_collect_playlist_drafts(GTK_WIDGET(view->ctx->parent), names);
    GList *choices = g_hash_table_get_keys(names);
    choices = g_list_sort(choices, (GCompareFunc)g_utf8_collate);
    gboolean updating = view->updating;
    view->updating = TRUE;
    for (guint i = 0; i < view->sources->len; ++i) {
        AutomationSource *source = g_ptr_array_index(view->sources, i);
        GtkWidget *entry = gtk_bin_get_child(GTK_BIN(source->playlist));
        gchar *current = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
        gchar *active = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(source->playlist)));
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(source->playlist));
        for (GList *item = choices; item != NULL; item = item->next)
            if (view->ctx->data) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(source->playlist), item->data, g_hash_table_lookup(names, item->data));
            else gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(source->playlist), item->data);
        if (view->ctx->data && active && *active && !g_hash_table_contains(names, active))
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(source->playlist), active, "Kaydedilmiş playlist · kayıt seçimi bekleniyor");
        if (!view->ctx->data || !active || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(source->playlist), active)) gtk_entry_set_text(GTK_ENTRY(entry), current);
        g_free(current); g_free(active);
    }
    view->updating = updating;
    g_list_free(choices); g_hash_table_unref(names);
}

void
radios_automation_playlists_changed(GtkWidget *parent)
{
    AutomationView *view = g_object_get_data(G_OBJECT(parent), "automation-view");
    if (view && view->ready) { automation_refresh_source_choices(view); automation_update_controls(view); }
    if (GTK_IS_CONTAINER(parent)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(parent));
        for (GList *item = children; item; item = item->next) radios_automation_playlists_changed(item->data);
        g_list_free(children);
    }
}

static void
automation_mapped(GtkWidget *widget, gpointer data)
{
    AutomationView *view = data; (void)widget;
    gboolean updating = view->updating, pruned = FALSE;
    view->updating = TRUE;
    automation_refresh_source_choices(view);
    GHashTable *players = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    automation_collect_players(GTK_WIDGET(view->ctx->parent), players);
    GHashTableIter iter; gpointer id, name;
    g_hash_table_iter_init(&iter, view->selected_decks);
    while (g_hash_table_iter_next(&iter, &id, NULL))
        if (!g_hash_table_contains(players, id)) { g_hash_table_remove(view->deck_policies, id); g_hash_table_iter_remove(&iter); pruned = TRUE; }
    GList *children = gtk_container_get_children(GTK_CONTAINER(view->decks_box));
    for (GList *item = children; item != NULL; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(children);
    GtkComboBoxText *jingle = GTK_COMBO_BOX_TEXT(automation_control(view, "jingleDeck"));
    gchar *active = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(jingle)));
    gtk_combo_box_text_remove_all(jingle);
    gtk_combo_box_text_append(jingle, "", "Müzik deck sırasını kullan");
    GList *player_ids = g_list_sort(g_hash_table_get_keys(players), automation_deck_compare);
    for (GList *player = player_ids; player; player = player->next) {
        id = player->data; name = g_hash_table_lookup(players, id);
        GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
        GtkWidget *check = gtk_check_button_new_with_label(name);
        gchar *widget_id = g_strdup_printf("automation-deck-%s", (gchar *)id); gtk_widget_set_name(check, widget_id); g_free(widget_id);
        g_object_set_data_full(G_OBJECT(check), "automation-deck-id", g_strdup(id), g_free);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), g_hash_table_contains(view->selected_decks, id));
        g_signal_connect(check, "toggled", G_CALLBACK(automation_deck_toggled), view);
        const gchar *modes[] = { "Genel geçiş", "Parça sonu", "Crossfade", "Cue / auto-next", "Manuel", NULL };
        GtkWidget *mode = radios_ui_combo(modes, 0), *overlap = gtk_spin_button_new_with_range(0, 30, 0.1);
        automation_compact_combo(mode); gtk_entry_set_width_chars(GTK_ENTRY(overlap), 4);
        JsonObject *policy = g_hash_table_lookup(view->deck_policies, id);
        gtk_combo_box_set_active(GTK_COMBO_BOX(mode), policy ? (gint)radios_json_int(policy, "transition", 0) + 1 : 0);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(overlap), 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(overlap), policy ? json_object_get_double_member(policy, "overlap") : 0);
        widget_id = g_strdup_printf("automation-deck-transition-%s", (gchar *)id); gtk_widget_set_name(mode, widget_id); g_free(widget_id);
        widget_id = g_strdup_printf("automation-deck-overlap-%s", (gchar *)id); gtk_widget_set_name(overlap, widget_id); g_free(widget_id);
        gtk_widget_set_tooltip_text(mode, "Bu AutoDJ deckinden sonraki parçaya geçiş");
        gtk_widget_set_tooltip_text(overlap, "AutoDJ bindirme süresi · saniye");
        g_object_set_data(G_OBJECT(check), "automation-deck-transition", mode);
        g_object_set_data(G_OBJECT(check), "automation-deck-overlap", overlap);
        g_object_set_data(G_OBJECT(row), "automation-deck-transition", mode);
        g_object_set_data(G_OBJECT(row), "automation-deck-overlap", overlap);
        g_object_set_data(G_OBJECT(row), "automation-deck-check", check);
        gboolean selected = g_hash_table_contains(view->selected_decks, id);
        gint active_mode = gtk_combo_box_get_active(GTK_COMBO_BOX(mode));
        gtk_widget_set_sensitive(mode, selected);
        gtk_widget_set_sensitive(overlap, selected && (active_mode == 2 || active_mode == 3));
        gtk_box_pack_start(GTK_BOX(row), check, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(row), mode, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), overlap, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), radios_ui_label("sn", NULL), FALSE, FALSE, 0);
        g_signal_connect(mode, "changed", G_CALLBACK(automation_deck_policy_changed), view);
        g_signal_connect(overlap, "value-changed", G_CALLBACK(automation_deck_policy_changed), view);
        gtk_box_pack_start(GTK_BOX(view->decks_box), row, FALSE, FALSE, 0);
        gtk_combo_box_text_append(jingle, id, name);
    }
    g_list_free(player_ids);
    if (g_hash_table_size(players) == 0)
        gtk_box_pack_start(GTK_BOX(view->decks_box), radios_ui_label("Yayın ekranında oynatıcı oluşturulmadı.", "automation-help"), FALSE, FALSE, 0);
    gboolean jingle_missing = active && *active && !g_hash_table_contains(players, active);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(jingle), active != NULL && g_hash_table_contains(players, active) ? active : "");
    g_free(active); g_hash_table_unref(players);
    gtk_widget_show_all(view->decks_box);
    /* Refresh the summary after removed GUI cards have been pruned. */
    GString *summary = g_string_new("");
    g_hash_table_iter_init(&iter, view->selected_decks);
    while (g_hash_table_iter_next(&iter, NULL, &name)) {
        if (summary->len > 0) g_string_append(summary, ", ");
        g_string_append(summary, name);
    }
    gtk_label_set_text(GTK_LABEL(view->summary_decks), summary->len > 0 ? summary->str : "Atanmadı");
    g_string_free(summary, TRUE);
    view->updating = updating;
    if (!view->applying && (pruned || jingle_missing)) automation_dirty(view);
}

static void
automation_save(GtkButton *button, gpointer data)
{
    AutomationView *view = data; (void)button;
    if (!automation_validate(view)) return;
    if (view->ctx->data) {
        JsonNode *config = automation_config(view);
        if (radios_data_document_save(view->document, config)) automation_notice(view, "Rotasyon kuralları kaydediliyor…");
        else automation_notice(view, "Rotasyon kaydı/bağlantısı henüz hazır değil; girdiler korunuyor.");
        json_node_free(config);
    } else
        automation_notice(view, "Rotasyon oturum taslağı doğrulandı. Veritabanına kaydedilmedi; canlı yayın değiştirilmedi.");
}

static void
automation_source_action(GtkButton *button, gpointer data)
{
    AutomationSource *source = data;
    AutomationView *view = source->view;
    if (!view->applying) automation_dirty(view);
    gint action = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "automation-source-action"));
    guint index = 0;
    if (!g_ptr_array_find(view->sources, source, &index)) return;
    if (action == 0) {
        gtk_widget_destroy(source->row);
        g_ptr_array_remove_index(view->sources, index);
    } else {
        gint next = (gint)index + action;
        if (next < 0 || next >= (gint)view->sources->len) return;
        gpointer other = g_ptr_array_index(view->sources, (guint)next);
        g_ptr_array_index(view->sources, (guint)next) = source;
        g_ptr_array_index(view->sources, index) = other;
        for (guint i = 0; i < view->sources->len; ++i)
            gtk_box_reorder_child(GTK_BOX(view->sources_box), ((AutomationSource *)g_ptr_array_index(view->sources, i))->row, (gint)i);
    }
    automation_update_controls(view);
}

static void
automation_add_source(GtkButton *button, gpointer data)
{
    AutomationView *view = data; (void)button;
    if (view->sources->len >= 50) return;
    AutomationSource *source = g_new0(AutomationSource, 1);
    source->view = view;
    source->id = g_uuid_string_random();
    source->row = gtk_grid_new();
    gtk_widget_set_name(source->row, "automation-source-row");
    g_object_set_data_full(G_OBJECT(source->row), "automation-source-id", g_strdup(source->id), g_free);
    gtk_grid_set_column_spacing(GTK_GRID(source->row), 12);
    gtk_grid_set_row_spacing(GTK_GRID(source->row), 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(source->row), "automation-source-row");
    GtkWidget *identity = radios_ui_box(GTK_ORIENTATION_VERTICAL, 7, NULL);
    source->playlist = gtk_combo_box_text_new_with_entry();
    gtk_widget_set_name(source->playlist, "automation-source-selector");
    automation_compact_combo(source->playlist);
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(source->playlist));
    gtk_widget_set_name(entry, "automation-source-playlist");
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), view->ctx->data ? "Veritabanı playlisti seç" : "Playlist adı / kimliği (taslak)");
    gtk_entry_set_max_length(GTK_ENTRY(entry), 120);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 20);
    g_signal_connect(entry, "changed", G_CALLBACK(automation_changed), view);
    gtk_widget_set_hexpand(identity, TRUE);
    gtk_box_pack_start(GTK_BOX(identity), source->playlist, FALSE, FALSE, 0);
    GtkWidget *window = gtk_expander_new("Gün / saat sınırı");
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "automation-source-window");
    GtkWidget *fields = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(fields), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(fields), 8);
    source->start = radios_ui_entry("00:00"); gtk_entry_set_text(GTK_ENTRY(source->start), "00:00");
    source->end = radios_ui_entry("00:00"); gtk_entry_set_text(GTK_ENTRY(source->end), "00:00");
    gtk_widget_set_name(source->start, "automation-source-start");
    gtk_widget_set_name(source->end, "automation-source-end");
    gtk_entry_set_max_length(GTK_ENTRY(source->start), 5); gtk_entry_set_max_length(GTK_ENTRY(source->end), 5);
    gtk_entry_set_width_chars(GTK_ENTRY(source->start), 5); gtk_entry_set_width_chars(GTK_ENTRY(source->end), 5);
    const gchar *day_choices[] = { "Her gün", "Hafta içi", "Hafta sonu", NULL };
    source->days = radios_ui_combo(day_choices, 0);
    gtk_widget_set_name(source->days, "automation-source-days");
    g_signal_connect(source->start, "changed", G_CALLBACK(automation_changed), view);
    g_signal_connect(source->end, "changed", G_CALLBACK(automation_changed), view);
    g_signal_connect(source->days, "changed", G_CALLBACK(automation_changed), view);
    const gchar *field_names[] = { "Başlangıç · UTC", "Bitiş · UTC", "Günler" };
    GtkWidget *inputs[] = { source->start, source->end, source->days };
    for (guint i = 0; i < G_N_ELEMENTS(inputs); ++i) {
        GtkWidget *field = radios_ui_box(GTK_ORIENTATION_VERTICAL, 5, NULL);
        gtk_box_pack_start(GTK_BOX(field), radios_ui_label(field_names[i], NULL), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(field), inputs[i], FALSE, FALSE, 0);
        gtk_grid_attach(GTK_GRID(fields), field, (gint)i, 0, 1, 1);
    }
    gtk_container_add(GTK_CONTAINER(window), fields);
    gtk_box_pack_start(GTK_BOX(identity), window, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(source->row), identity, 0, 0, 3, 1);
    source->weight = gtk_spin_button_new_with_range(1, 100, 1);
    source->count = gtk_spin_button_new_with_range(1, 100, 1);
    g_signal_connect(source->weight, "value-changed", G_CALLBACK(automation_changed), view);
    g_signal_connect(source->count, "value-changed", G_CALLBACK(automation_changed), view);
    gtk_widget_set_name(source->weight, "automation-source-weight");
    gtk_widget_set_name(source->count, "automation-source-count");
    g_object_set_data(G_OBJECT(source->row), "automation-source-playlist", entry);
    g_object_set_data(G_OBJECT(source->row), "automation-source-weight", source->weight);
    g_object_set_data(G_OBJECT(source->row), "automation-source-count", source->count);
    g_object_set_data(G_OBJECT(source->row), "automation-source-start", source->start);
    g_object_set_data(G_OBJECT(source->row), "automation-source-end", source->end);
    GtkWidget *numbers[] = { source->weight, source->count };
    for (guint i = 0; i < G_N_ELEMENTS(numbers); ++i) {
        gtk_entry_set_width_chars(GTK_ENTRY(numbers[i]), 4);
        gtk_widget_set_valign(numbers[i], GTK_ALIGN_START);
        GtkWidget *number_field = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, NULL);
        gtk_box_pack_start(GTK_BOX(number_field), radios_ui_label(i ? "Turda parça" : "Ağırlık", "automation-source-caption"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(number_field), numbers[i], FALSE, FALSE, 0);
        gtk_grid_attach(GTK_GRID(source->row), number_field, (gint)i, 1, 1, 1);
    }
    GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, "automation-source-buttons");
    const gchar *names[] = { "↑", "↓", "×" };
    const gchar *tips[] = { "Kaynağı yukarı taşı", "Kaynağı aşağı taşı", "Kaynağı kaldır" };
    const gchar *ids[] = { "automation-source-up", "automation-source-down", "automation-source-remove" };
    const gint directions[] = { -1, 1, 0 };
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *action = radios_ui_button(names[i]);
        gtk_widget_set_name(action, ids[i]);
        gtk_widget_set_tooltip_text(action, tips[i]);
        g_object_set_data(G_OBJECT(action), "automation-source-action", GINT_TO_POINTER(directions[i]));
        g_signal_connect(action, "clicked", G_CALLBACK(automation_source_action), source);
        gtk_box_pack_start(GTK_BOX(actions), action, FALSE, FALSE, 0);
        if (i == 0) source->up = action;
        else if (i == 1) source->down = action;
    }
    gtk_widget_set_valign(actions, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(source->row), actions, 2, 1, 1, 1);
    g_ptr_array_add(view->sources, source);
    if (!view->applying) automation_dirty(view);
    gtk_box_pack_start(GTK_BOX(view->sources_box), source->row, FALSE, FALSE, 0);
    gtk_widget_show_all(source->row);
    automation_refresh_source_choices(view);
    automation_update_controls(view);
}

static JsonNode *
automation_config(AutomationView *view)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *config = json_object_new();
    json_node_take_object(node, config);
    GHashTableIter fields; gpointer key, value; g_hash_table_iter_init(&fields, view->fields);
    while (g_hash_table_iter_next(&fields, &key, &value)) {
        const gchar *name = key; GtkWidget *input = value;
        if (GTK_IS_SPIN_BUTTON(input)) {
            if (gtk_spin_button_get_digits(GTK_SPIN_BUTTON(input)) == 0)
                json_object_set_int_member(config, name, gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(input)));
            else json_object_set_double_member(config, name, gtk_spin_button_get_value(GTK_SPIN_BUTTON(input)));
        } else if (GTK_IS_TOGGLE_BUTTON(input)) json_object_set_boolean_member(config, name, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(input)));
        else if (GTK_IS_ENTRY(input)) json_object_set_string_member(config, name, gtk_entry_get_text(GTK_ENTRY(input)));
        else if (GTK_IS_COMBO_BOX(input)) {
            if (g_str_equal(name, "jinglePlaylist") || g_str_equal(name, "fallbackPlaylist") || g_str_equal(name, "jingleDeck")) {
                const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(input));
                if (id && *id) json_object_set_string_member(config, name, id);
                else json_object_set_null_member(config, name);
            } else json_object_set_int_member(config, name, gtk_combo_box_get_active(GTK_COMBO_BOX(input)));
        }
    }
    JsonArray *days = json_array_new(), *sources = json_array_new(), *decks = json_array_new();
    JsonArray *policies = json_array_new();
    for (guint i = 0; i < G_N_ELEMENTS(view->weekdays); ++i)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(view->weekdays[i]))) json_array_add_int_element(days, i + 1);
    for (guint i = 0; i < view->sources->len; ++i) {
        AutomationSource *source = g_ptr_array_index(view->sources, i); JsonObject *row = json_object_new();
        const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(source->playlist));
        json_object_set_string_member(row, "source_id", source->id);
        json_object_set_string_member(row, "playlist_id", id ? id : "");
        json_object_set_int_member(row, "weight", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(source->weight)));
        json_object_set_int_member(row, "count", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(source->count)));
        json_object_set_string_member(row, "start", gtk_entry_get_text(GTK_ENTRY(source->start)));
        json_object_set_string_member(row, "end", gtk_entry_get_text(GTK_ENTRY(source->end)));
        json_object_set_int_member(row, "days", gtk_combo_box_get_active(GTK_COMBO_BOX(source->days)));
        json_array_add_object_element(sources, row);
    }
    GList *ids = g_hash_table_get_keys(view->selected_decks); ids = g_list_sort(ids, automation_deck_compare);
    for (GList *item = ids; item; item = item->next) {
        json_array_add_string_element(decks, item->data);
        JsonObject *policy = g_hash_table_lookup(view->deck_policies, item->data);
        if (policy) json_array_add_object_element(policies, json_object_ref(policy));
    }
    g_list_free(ids);
    json_object_set_array_member(config, "weekdays", days);
    json_object_set_array_member(config, "sources", sources);
    json_object_set_array_member(config, "decks", decks);
    json_object_set_array_member(config, "deckTransitions", policies);
    return node;
}

static void
automation_select_id(GtkWidget *widget, const gchar *id)
{
    if (!id || !*id) gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), "");
    else if (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), id)) {
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), id, "Kaydedilmiş seçim · bağlantı bekleniyor");
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), id);
    }
}

static void
automation_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    AutomationView *view = data; (void)ctx; (void)owner;
    if (view->dirty && !g_object_get_data(G_OBJECT(owner), "radios-data-explicit-reload")) {
        JsonNode *current = automation_config(view), *incoming = json_node_new(JSON_NODE_OBJECT);
        json_node_set_object(incoming, config);
        gboolean equal = json_node_equal(current, incoming);
        json_node_free(current); json_node_free(incoming);
        if (!equal) {
            automation_dirty(view);
            automation_notice(view, "Kaydedilmiş kayıt alındı; daha yeni GUI değişiklikleriniz korunuyor."); return;
        }
    }
    view->applying = TRUE;
    g_hash_table_remove_all(view->deck_policies);
    JsonArray *policies = json_object_has_member(config, "deckTransitions") ? json_object_get_array_member(config, "deckTransitions") : NULL;
    for (guint index = 0; policies && index < json_array_get_length(policies); ++index) {
        JsonObject *policy = json_array_get_object_element(policies, index);
        g_hash_table_replace(view->deck_policies, g_strdup(radios_json_string(policy, "deck_id", "")), json_object_ref(policy));
    }
    GHashTableIter fields; gpointer key, value; g_hash_table_iter_init(&fields, view->fields);
    while (g_hash_table_iter_next(&fields, &key, &value)) {
        const gchar *name = key; GtkWidget *input = value;
        if (!json_object_has_member(config, name)) continue;
        if (GTK_IS_SPIN_BUTTON(input)) gtk_spin_button_set_value(GTK_SPIN_BUTTON(input), json_object_get_double_member(config, name));
        else if (GTK_IS_TOGGLE_BUTTON(input)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(input), json_object_get_boolean_member(config, name));
        else if (GTK_IS_ENTRY(input)) gtk_entry_set_text(GTK_ENTRY(input), radios_json_string(config, name, ""));
        else if (GTK_IS_COMBO_BOX(input)) {
            if (g_str_equal(name, "jinglePlaylist") || g_str_equal(name, "fallbackPlaylist") || g_str_equal(name, "jingleDeck"))
                automation_select_id(input, radios_json_string(config, name, ""));
            else gtk_combo_box_set_active(GTK_COMBO_BOX(input), (gint)radios_json_int(config, name, 0));
        }
    }
    JsonArray *days = json_object_get_array_member(config, "weekdays");
    for (guint i = 0; i < G_N_ELEMENTS(view->weekdays); ++i) {
        gboolean active = FALSE;
        for (guint j = 0; days && j < json_array_get_length(days); ++j) active |= json_array_get_int_element(days, j) == (gint64)i + 1;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->weekdays[i]), active);
    }
    for (guint i = 0; i < view->sources->len; ++i) gtk_widget_destroy(((AutomationSource *)g_ptr_array_index(view->sources, i))->row);
    g_ptr_array_set_size(view->sources, 0);
    JsonArray *sources = json_object_get_array_member(config, "sources");
    for (guint i = 0; sources && i < json_array_get_length(sources); ++i) {
        automation_add_source(NULL, view); AutomationSource *source = g_ptr_array_index(view->sources, view->sources->len - 1);
        JsonObject *row = json_array_get_object_element(sources, i);
        g_free(source->id); source->id = g_strdup(radios_json_string(row, "source_id", ""));
        g_object_set_data_full(G_OBJECT(source->row), "automation-source-id", g_strdup(source->id), g_free);
        automation_select_id(source->playlist, radios_json_string(row, "playlist_id", ""));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(source->weight), json_object_get_double_member(row, "weight"));
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(source->count), json_object_get_double_member(row, "count"));
        gtk_entry_set_text(GTK_ENTRY(source->start), radios_json_string(row, "start", "00:00"));
        gtk_entry_set_text(GTK_ENTRY(source->end), radios_json_string(row, "end", "00:00"));
        gtk_combo_box_set_active(GTK_COMBO_BOX(source->days), (gint)radios_json_int(row, "days", 0));
    }
    g_hash_table_remove_all(view->selected_decks);
    JsonArray *decks = json_object_get_array_member(config, "decks");
    for (guint i = 0; decks && i < json_array_get_length(decks); ++i) {
        const gchar *id = json_array_get_string_element(decks, i);
        g_hash_table_insert(view->selected_decks, g_strdup(id), g_strdup(id));
    }
    view->dirty = FALSE; view->applying = FALSE;
    gtk_label_set_text(GTK_LABEL(view->summary_saved), "Veritabanında kayıtlı");
    automation_update_controls(view);
    automation_mapped(view->widget, view);
    automation_notice(view, "Rotasyon kuralları veritabanıyla eşlendi.");
}

static GtkWidget *
automation_card(const gchar *title, const gchar *description, GtkWidget **body,
                GtkWidget **head)
{
    GtkWidget *card = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "automation-card");
    GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "automation-card-heading");
    GtkWidget *text = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, NULL);
    GtkWidget *title_label = radios_ui_label(title, "h3 heading");
    gtk_label_set_max_width_chars(GTK_LABEL(title_label), 36);
    gtk_box_pack_start(GTK_BOX(text), title_label, FALSE, FALSE, 0);
    if (description != NULL) {
        GtkWidget *description_label = radios_ui_label(description, "task-help");
        gtk_label_set_max_width_chars(GTK_LABEL(description_label), 42);
        gtk_box_pack_start(GTK_BOX(text), description_label, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(heading), text, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(card), heading, FALSE, FALSE, 0);
    *body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL);
    gtk_box_pack_start(GTK_BOX(card), *body, FALSE, FALSE, 0);
    if (head != NULL)
        *head = heading;
    return card;
}

static GtkWidget *
automation_grid(guint columns)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(grid), "automation-fields");
    g_object_set_data(G_OBJECT(grid), "automation-field-columns", GUINT_TO_POINTER(columns));
    if (columns == 3)
        gtk_style_context_add_class(gtk_widget_get_style_context(grid), "automation-fields-three");
    return grid;
}

static void
automation_field(GtkWidget *grid, const gchar *name, GtkWidget *input, guint column, guint row)
{
    GtkWidget *field = radios_ui_box(GTK_ORIENTATION_VERTICAL, 5, "automation-field");
    GtkWidget *label = radios_ui_label(name, NULL);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 22);
    if (GTK_IS_COMBO_BOX(input)) automation_compact_combo(input);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), input);
    gtk_widget_set_hexpand(input, TRUE);
    gtk_widget_set_hexpand(field, TRUE);
    gtk_box_pack_start(GTK_BOX(field), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(field), input, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), field, (gint)column, (gint)row, 1, 1);
}

static GtkWidget *
automation_spin(gdouble min, gdouble max, gdouble step, gdouble value)
{
    GtkWidget *spin = gtk_spin_button_new_with_range(min, max, step);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), step < 1.0 ? 1 : 0);
    gtk_entry_set_width_chars(GTK_ENTRY(spin), 6);
    return spin;
}

static GtkWidget *
automation_entry(const gchar *text)
{
    GtkWidget *entry = radios_ui_entry(NULL);
    gtk_entry_set_text(GTK_ENTRY(entry), text);
    return entry;
}

static GtkWidget *
automation_check(GtkWidget *body, const gchar *text, gboolean checked)
{
    GtkWidget *check = gtk_check_button_new_with_label(text);
    gtk_style_context_add_class(gtk_widget_get_style_context(check), "automation-check");
    GtkWidget *label = gtk_bin_get_child(GTK_BIN(check));
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), checked);
    gtk_box_pack_start(GTK_BOX(body), check, FALSE, FALSE, 0);
    return check;
}

static void
automation_tab(GtkButton *button, gpointer data)
{
    AutomationView *view = data;
    if (view->selecting)
        return;
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "automation-tab"));
    const gchar *names[] = { "rotation", "timing", "decks" };
    view->selecting = TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(view->tabs); ++i)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->tabs[i]), i == index);
    gtk_stack_set_visible_child_name(GTK_STACK(view->stack), names[index]);
    view->selecting = FALSE;
}

static void
automation_name_changed(GtkEditable *entry, gpointer data)
{
    AutomationView *view = data;
    gtk_label_set_text(GTK_LABEL(view->summary_name), gtk_entry_get_text(GTK_ENTRY(entry)));
}

static void
automation_mode_changed(GtkComboBox *combo, gpointer data)
{
    AutomationView *view = data;
    const gchar *names[] = { "Rastgele", "Playlist sırası", "En az planlanan" };
    gint index = gtk_combo_box_get_active(combo);
    if (index >= 0 && index < (gint)G_N_ELEMENTS(names))
        gtk_label_set_text(GTK_LABEL(view->summary_mode), names[index]);
}

static void
automation_gaps_changed(GtkSpinButton *spin, gpointer data)
{
    AutomationView *view = data;
    gchar *text = g_strdup_printf("%d / %d dk",
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(view->track_gap)),
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(view->artist_gap)));
    (void)spin;
    gtk_label_set_text(GTK_LABEL(view->summary_gaps), text);
    g_free(text);
}

static void
automation_new_seed(GtkButton *button, gpointer data)
{
    AutomationView *view = data;
    gchar *seed = g_uuid_string_random();
    (void)button;
    gtk_entry_set_text(GTK_ENTRY(view->seed), seed);
    g_free(seed);
}

static void
automation_preview(GtkButton *button, gpointer data)
{
    AutomationView *view = data;
    (void)button;
    if (!automation_validate(view)) return;
    if (!view->sources->len) { automation_notice(view, "Önizleme için en az bir kaynak playlist seçin."); return; }
    if (!view->ctx->playout) { automation_notice(view, "Yerel yürütücü bağlı değil; yayın sırası üretilmedi."); return; }
    automation_preview_clear(NULL, view); view->preview_busy = TRUE; gtk_widget_set_sensitive(view->preview_button, FALSE);
    JsonNode *config = automation_config(view); GError *error = NULL;
    if (!radios_playout_ui_preview_automation(view->ctx, view->widget, json_node_get_object(config), automation_preview_received,
        GUINT_TO_POINTER(view->preview_generation), NULL, &error)) {
        view->preview_busy = FALSE; gtk_widget_set_sensitive(view->preview_button, TRUE);
        automation_notice(view, error ? error->message : "Önizleme başlatılamadı.");
    } else automation_notice(view, "Doğrulanmış medya ve kurallarla yayın sırası hazırlanıyor…");
    g_clear_error(&error); json_node_free(config);
}
static void automation_preview_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; AutomationView *view = owner ? g_object_get_data(G_OBJECT(owner), "automation-view") : NULL;
    if (!view || view->disposed || GPOINTER_TO_UINT(data) != view->preview_generation) return;
    view->preview_busy = FALSE; gtk_widget_set_sensitive(view->preview_button, TRUE);
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { automation_notice(view, error ? error->message : "Önizleme sonucu geçersiz."); return; }
    view->preview = json_node_copy(result); JsonObject *plan = json_node_get_object(result); JsonArray *entries = json_object_get_array_member(plan, "entries");
    GtkListStore *store = g_object_get_data(G_OBJECT(view->preview_table), "list-store"); gtk_list_store_clear(store);
    for (guint i = 0; entries && i < json_array_get_length(entries); ++i) {
        JsonObject *entry = json_array_get_object_element(entries, i); GtkTreeIter iter;
        gchar *time = g_strdup_printf("%u · %s", i + 1, radios_json_string(entry, "start_utc", ""));
        gchar *track = g_strdup_printf("%s · %s", radios_json_string(entry, "title", ""), radios_json_string(entry, "artist", ""));
        gint64 deck_index = radios_json_int(entry, "deck_index", -1);
        gchar *deck = deck_index >= 0 ? g_strdup_printf("Oynatıcı %" G_GINT64_FORMAT, deck_index + 1) : g_strdup("Atanmadı");
        gint64 milliseconds = radios_json_int(entry, "duration_ms", 0);
        const gchar *modes[] = { "Parça sonu", "Crossfade", "Cue / auto-next", "Manuel" };
        guint mode = (guint)CLAMP(radios_json_int(entry, "transition", 0), 0, 3);
        gchar *duration = g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT " / %s · %.1f sn", milliseconds / 60000, (milliseconds / 1000) % 60,
            modes[mode], radios_json_int(entry, "overlap_ms", 0) / 1000.0);
        gtk_list_store_append(store, &iter); gtk_list_store_set(store, &iter, 0, time, 1, track, 2, radios_json_string(entry, "source_name", ""),
            3, deck, 4, duration, 5, radios_json_string(entry, "reason", ""), -1);
        g_free(time); g_free(track); g_free(deck); g_free(duration);
    }
    guint count = entries ? json_array_get_length(entries) : 0; gtk_widget_set_visible(view->preview_empty, !count); gtk_widget_set_visible(view->preview_table, count > 0);
    gchar *candidates = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(plan, "candidate_count", 0));
    gtk_label_set_text(GTK_LABEL(view->summary_candidates), candidates); g_free(candidates);
    gtk_widget_set_sensitive(view->preview_clear, TRUE); gtk_widget_set_sensitive(view->preview_apply, count > 0); gtk_widget_set_sensitive(view->preview_download, TRUE);
    JsonArray *diagnostics = json_object_get_array_member(plan, "diagnostics");
    const gchar *reason = diagnostics && json_array_get_length(diagnostics) ? radios_json_string(json_array_get_object_element(diagnostics, 0), "reason", "") : "";
    gchar *message = g_strdup_printf("%u öğe hazır · %u plan uyarısı. %s", count, diagnostics ? json_array_get_length(diagnostics) : 0, *reason ? reason : "Önizleme ses çalmaz.");
    automation_notice(view, message); g_free(message);
}
static void automation_preview_clear(GtkButton *button, gpointer data)
{
    (void)button; AutomationView *view = data; ++view->preview_generation; view->preview_busy = FALSE; g_clear_pointer(&view->preview, json_node_free);
    if (view->preview_button) gtk_widget_set_sensitive(view->preview_button, TRUE);
    if (view->preview_apply) gtk_widget_set_sensitive(view->preview_apply, FALSE);
    if (view->preview_download) gtk_widget_set_sensitive(view->preview_download, FALSE);
    if (view->preview_clear) gtk_widget_set_sensitive(view->preview_clear, FALSE);
    if (view->preview_table) { gtk_list_store_clear(g_object_get_data(G_OBJECT(view->preview_table), "list-store")); gtk_widget_hide(view->preview_table); }
    if (view->preview_empty) gtk_widget_show(view->preview_empty);
    if (view->summary_candidates) gtk_label_set_text(GTK_LABEL(view->summary_candidates), view->ctx->playout ? "—" : "0");
}
static void automation_preview_apply(GtkButton *button, gpointer data)
{
    (void)button; AutomationView *view = data; if (!view->preview) return; GError *error = NULL;
    JsonArray *entries = json_object_get_array_member(json_node_get_object(view->preview), "entries");
    if (!radios_broadcast_append_playlist(view->ctx, entries, &error)) automation_notice(view, error ? error->message : "Önizleme program sırasına aktarılamadı.");
    else { automation_notice(view, "Doğrulanmış önizleme program sırasına ekleniyor."); automation_preview_clear(NULL, view); }
    g_clear_error(&error);
}
static void automation_preview_download(GtkButton *button, gpointer data)
{
    (void)button; AutomationView *view = data; if (!view->preview) return;
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Planı indir", view->ctx->parent, GTK_FILE_CHOOSER_ACTION_SAVE,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE); radios_ui_file_chooser_default_folder(GTK_FILE_CHOOSER(dialog), view->ctx);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), "RadioS-plan.json");
    JsonGenerator *generator = json_generator_new(); json_generator_set_root(generator, view->preview); json_generator_set_pretty(generator, TRUE);
    g_object_set_data_full(G_OBJECT(dialog), "automation-export-data", json_generator_to_data(generator, NULL), g_free); g_object_unref(generator);
    g_object_set_data(G_OBJECT(dialog), "automation-plan-export", GINT_TO_POINTER(1));
    g_signal_connect(dialog, "response", G_CALLBACK(automation_export_response), view);
    automation_dialog_follow_page(view, dialog); gtk_widget_show(dialog);
}
static void automation_live_action(GtkButton *button, gpointer data)
{
    AutomationView *view = data; if (view->disposed || !view->ctx->playout) return; GError *error = NULL; gboolean accepted;
    if (GTK_WIDGET(button) == view->live_start) {
        if (!automation_validate(view)) return;
        JsonNode *config = automation_config(view);
        const gchar *origin = radios_data_document_revision(view->document) > 0 ? radios_data_document_id(view->document) : NULL;
        accepted = radios_playout_ui_start_automation_with_loudness_origin(view->ctx, view->widget, json_node_get_object(config), origin, FALSE, &error);
        json_node_free(config);
    } else if (GTK_WIDGET(button) == view->live_next) {
        RadioSPlayoutSnapshot snapshot = {0};
        if (!radios_playout_ui_get_snapshot(view->ctx, &snapshot) || !snapshot.automation) {
            automation_notice(view, "Bu geçiş kontrolü yalnızca çalışan AutoDJ içindir."); return;
        }
        accepted = radios_playout_ui_action(view->ctx, RADIOS_PLAYOUT_NEXT, &error);
    } else {
        RadioSPlayoutSnapshot snapshot = {0}; radios_playout_ui_get_snapshot(view->ctx, &snapshot);
        accepted = radios_playout_ui_action(view->ctx, GTK_WIDGET(button) == view->live_stop ? RADIOS_PLAYOUT_STOP :
            snapshot.state == RADIOS_PLAYOUT_PAUSED ? RADIOS_PLAYOUT_RESUME : RADIOS_PLAYOUT_PAUSE, &error);
    }
    if (!accepted) automation_notice(view, error ? error->message : "Otomasyon komutu kabul edilmedi.");
    g_clear_error(&error);
}
static gboolean automation_playout_tick(gpointer data)
{
    AutomationView *view = data; if (view->disposed) return G_SOURCE_REMOVE; RadioSPlayoutSnapshot snapshot = {0};
    if (!radios_playout_ui_get_snapshot(view->ctx, &snapshot)) return G_SOURCE_CONTINUE;
    gchar *text = snapshot.error[0] ? g_strdup(snapshot.error) : g_strdup_printf("%s · %u/%u · %s", radios_playout_ui_state_name(snapshot.state), snapshot.total ? snapshot.index + 1 : 0, snapshot.total, snapshot.title);
    if (!snapshot.error[0] && snapshot.deck >= 0) {
        const gchar *modes[] = { "Parça sonu", "Crossfade", "Cue / auto-next", "Manuel" };
        gchar *route = snapshot.next_deck >= 0 ? g_strdup_printf("Deck %d → Deck %d", snapshot.deck + 1, snapshot.next_deck + 1) :
            g_strdup_printf("Deck %d", snapshot.deck + 1);
        gchar *detail = g_strdup_printf("%s · %s · %.1f sn · %s", route, modes[MIN(snapshot.transition, 3)], snapshot.overlap_ms / 1000.0, text);
        g_free(route); g_free(text); text = detail;
    }
    gtk_label_set_text(GTK_LABEL(view->live_state), text); g_free(text);
    gboolean active = snapshot.state != RADIOS_PLAYOUT_IDLE && snapshot.state != RADIOS_PLAYOUT_FINISHED && snapshot.state != RADIOS_PLAYOUT_ERROR;
    gtk_widget_set_sensitive(view->live_start, radios_data_connected(view->ctx) && !active);
    gtk_widget_set_sensitive(view->live_stop, active);
    gtk_widget_set_sensitive(view->live_pause, snapshot.state == RADIOS_PLAYOUT_PAUSED || snapshot.state == RADIOS_PLAYOUT_PLAYING || snapshot.state == RADIOS_PLAYOUT_OVERLAP);
    gtk_widget_set_sensitive(view->live_next, snapshot.automation && (snapshot.state == RADIOS_PLAYOUT_MANUAL_WAIT || snapshot.state == RADIOS_PLAYOUT_PAUSED ||
        snapshot.state == RADIOS_PLAYOUT_PLAYING || snapshot.state == RADIOS_PLAYOUT_OVERLAP));
    gtk_button_set_label(GTK_BUTTON(view->live_pause), snapshot.state == RADIOS_PLAYOUT_PAUSED ? "Devam" : "Duraklat");
    return G_SOURCE_CONTINUE;
}
static void automation_destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; AutomationView *view = data; view->disposed = TRUE;
    if (view->playout_timer) { g_source_remove(view->playout_timer); view->playout_timer = 0; }
}

static GtkWidget *
automation_fact(GtkWidget *body, const gchar *name, const gchar *value)
{
    GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 15, "automation-fact");
    GtkWidget *result = radios_ui_label(value, "automation-fact-value");
    gtk_label_set_xalign(GTK_LABEL(result), 1.0f);
    gtk_widget_set_margin_top(row, 5);
    gtk_widget_set_margin_bottom(row, 5);
    gtk_label_set_max_width_chars(GTK_LABEL(result), 25);
    gtk_label_set_ellipsize(GTK_LABEL(result), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(row), radios_ui_label(name, "automation-fact-label"), TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(row), result, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), row, FALSE, FALSE, 0);
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(separator), "automation-fact-border");
    gtk_box_pack_start(GTK_BOX(body), separator, FALSE, FALSE, 0);
    return result;
}

JsonNode *radios_automation_rules_configuration(GtkWidget *legacy_rules)
{
    AutomationView *view = g_object_get_data(G_OBJECT(legacy_rules), "automation-view");
    return view ? automation_config(view) : NULL;
}

GtkWidget *
radios_automation_new(RadioSViewContext *ctx)
{
    AutomationView *view = g_new0(AutomationView, 1);
    /* Spacing comes from the design's own margins (automation-workspace.css). */
    GtkWidget *page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "task-view automation-workspace ac-autodj");
    GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_VERTICAL, 5, "task-view-heading automation-heading");
    GtkWidget *text = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, NULL);
    GtkWidget *body, *head, *card, *grid;
    view->ctx = ctx;
    view->widget = page;
    view->sources = g_ptr_array_new_with_free_func(automation_source_free);
    view->fields = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    view->selected_decks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    view->deck_policies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_object_unref);
    g_object_set_data_full(G_OBJECT(page), "automation-view", view, automation_free);
    g_signal_connect(page, "destroy", G_CALLBACK(automation_destroyed), view);
    gtk_box_pack_start(GTK_BOX(text), radios_ui_label("AutoDJ kuralları", "h2 heading"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(heading), text, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), heading, FALSE, FALSE, 0);
    GtkWidget *state = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 7, "automation-state-strip");
    gtk_box_pack_start(GTK_BOX(state), radios_ui_label("●", "automation-state-dot"), FALSE, FALSE, 0);
    view->live_state = radios_ui_label("Planlayıcı beklemede", "strong"); gtk_widget_set_name(view->live_state, "automation-live-state");
    gtk_label_set_ellipsize(GTK_LABEL(view->live_state), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(view->live_state), 36);
    gtk_box_pack_start(GTK_BOX(state), view->live_state, FALSE, FALSE, 0);
    gtk_widget_set_tooltip_text(state, ctx->playout ? "Yerel ses çıkışı" : "Canlı yayın motoru bağlı değil.");
    gtk_box_pack_start(GTK_BOX(page), state, FALSE, FALSE, 0);
    GtkWidget *rule = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "automation-rule-bar");
    GtkWidget *rule_name = radios_ui_box(GTK_ORIENTATION_VERTICAL, 7, NULL);
    view->name = automation_register(view, "name", automation_entry("Genel rotasyon"));
    gtk_entry_set_max_length(GTK_ENTRY(view->name), 80);
    gtk_entry_set_width_chars(GTK_ENTRY(view->name), 20);
    gtk_widget_set_hexpand(view->name, TRUE);
    gtk_box_pack_start(GTK_BOX(rule_name), radios_ui_label("Rotasyon adı", NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rule_name), view->name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rule), rule_name, FALSE, FALSE, 0);
    GtkWidget *rule_actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 7, "automation-actions");
    GtkWidget *save = radios_ui_button("Kuralları kaydet");
    GtkWidget *export = radios_ui_button("Kuralları indir");
    gtk_widget_set_name(save, "automation-save");
    gtk_widget_set_name(export, "automation-export");
    gtk_style_context_add_class(gtk_widget_get_style_context(save), "task-primary");
    gtk_widget_set_valign(rule_actions, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(rule_actions), save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rule_actions), export, FALSE, FALSE, 0);
    g_signal_connect(save, "clicked", G_CALLBACK(automation_save), view);
    g_signal_connect(export, "clicked", G_CALLBACK(automation_export), view);
    gtk_box_pack_end(GTK_BOX(rule), rule_actions, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), rule, FALSE, FALSE, 0);
    GtkWidget *tabs = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, "automation-tabs");
    const gchar *tab_names[] = { "Rotasyon", "Saat ve geçiş", "Deck ve güvenlik" };
    for (guint i = 0; i < G_N_ELEMENTS(view->tabs); ++i) {
        view->tabs[i] = gtk_toggle_button_new_with_label(tab_names[i]);
        g_object_set_data(G_OBJECT(view->tabs[i]), "automation-tab", GUINT_TO_POINTER(i));
        g_signal_connect(view->tabs[i], "clicked", G_CALLBACK(automation_tab), view);
        gtk_box_pack_start(GTK_BOX(tabs), view->tabs[i], TRUE, TRUE, 0);
    }
    gtk_box_pack_start(GTK_BOX(page), tabs, FALSE, FALSE, 0);
    GtkWidget *layout = radios_ui_box(GTK_ORIENTATION_VERTICAL, 10, "automation-layout");
    view->stack = gtk_stack_new();
    gtk_stack_set_hhomogeneous(GTK_STACK(view->stack), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(view->stack), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(view->stack), "automation-pane-stack");
    GtkWidget *rotation = radios_ui_box(GTK_ORIENTATION_VERTICAL, 17, "automation-pane");
    card = automation_card("Kaynak playlistler", NULL, &body, &head);
    GtkWidget *source_add = radios_ui_button("+ Playlist ekle");
    view->source_add = source_add;
    gtk_widget_set_name(source_add, "automation-source-add");
    gtk_widget_set_tooltip_text(source_add, "Rotasyona kaynak playlist ekle");
    g_signal_connect(source_add, "clicked", G_CALLBACK(automation_add_source), view);
    gtk_box_pack_end(GTK_BOX(head), source_add, FALSE, FALSE, 0);
    view->sources_box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "automation-source-table");
    gtk_box_pack_start(GTK_BOX(body), view->sources_box, FALSE, FALSE, 0);
    GtkWidget *empty = radios_ui_label("Henüz kaynak yok.", "automation-empty");
    gtk_label_set_max_width_chars(GTK_LABEL(empty), 38);
    view->sources_empty = empty;
    gtk_label_set_xalign(GTK_LABEL(empty), 0.5f);
    gtk_label_set_justify(GTK_LABEL(empty), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start(GTK_BOX(body), empty, FALSE, FALSE, 0);
    grid = automation_grid(2);
    const gchar *source_modes[] = { "Sırayla tur", "Ağırlıklı rastgele", "Liste bitince sonraki", NULL };
    const gchar *track_modes[] = { "Rastgele", "Playlist sırası", "En az planlanan", NULL };
    automation_field(grid, "Playlistler arası seçim", automation_register(view, "sourceMode", radios_ui_combo(source_modes, 0)), 0, 0);
    GtkWidget *track_mode = radios_ui_combo(track_modes, 0);
    automation_register(view, "trackMode", track_mode);
    automation_field(grid, "Playlist içinden parça seçimi", track_mode, 1, 0);
    gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rotation), card, FALSE, FALSE, 0);
    card = automation_card("Tekrar koruması", NULL, &body, NULL);
    grid = automation_grid(2);
    view->track_gap = automation_register(view, "trackGap", automation_spin(0, 10080, 1, 60));
    view->artist_gap = automation_register(view, "artistGap", automation_spin(0, 10080, 1, 30));
    automation_field(grid, "Aynı parça · dakika", view->track_gap, 0, 0);
    automation_field(grid, "Aynı sanatçı · dakika", view->artist_gap, 1, 0);
    automation_field(grid, "Aynı albüm · dakika", automation_register(view, "albumGap", automation_spin(0, 10080, 1, 0)), 0, 1);
    gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
    automation_register(view, "includeProgramme", automation_check(body, "Mevcut program sırasını tekrar hesabına kat", TRUE));
    gtk_box_pack_start(GTK_BOX(rotation), card, FALSE, FALSE, 0);
    card = automation_card("Jingle / istasyon kimliği", NULL, &body, NULL);
    automation_register(view, "jingleEnabled", automation_check(body, "Düzenli jingle ekle", FALSE));
    grid = automation_grid(2);
    const gchar *playlists[] = { "Playlist seç", NULL };
    automation_field(grid, "Jingle playlisti", automation_register(view, "jinglePlaylist", radios_ui_combo(playlists, 0)), 0, 0);
    automation_field(grid, "Kaç müzik parçasında bir?", automation_register(view, "jingleEvery", automation_spin(1, 100, 1, 3)), 1, 0);
    gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rotation), card, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(view->stack), rotation, "rotation");

    GtkWidget *timing = radios_ui_box(GTK_ORIENTATION_VERTICAL, 17, "automation-pane");
    card = automation_card("Yayın saati ve gün bölümü", NULL, &body, NULL);
    grid = automation_grid(2);
    GDateTime *today = g_date_time_new_now_utc();
    gchar *date = g_date_time_format(today, "%Y-%m-%d");
    GtkWidget *date_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
    GtkWidget *date_entry = automation_register(view, "planDate", automation_entry(date));
    GtkWidget *date_button = radios_ui_button("▦");
    gtk_widget_set_tooltip_text(date_button, "Plan tarihini seç");
    g_signal_connect(date_button, "clicked", G_CALLBACK(automation_calendar), view);
    gtk_box_pack_start(GTK_BOX(date_row), date_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(date_row), date_button, FALSE, FALSE, 0);
    automation_field(grid, "Plan tarihi · UTC", date_row, 0, 0);
    g_free(date);
    g_date_time_unref(today);
    automation_field(grid, "Plan başlangıcı · UTC", automation_register(view, "startTime", automation_entry("08:00")), 1, 0);
    automation_field(grid, "Planlanacak müzik parçası", automation_register(view, "previewCount", automation_spin(1, 200, 1, 20)), 0, 1);
    automation_field(grid, "Gün bölümü başlangıcı · UTC", automation_register(view, "windowStart", automation_entry("00:00")), 1, 1);
    automation_field(grid, "Gün bölümü bitişi · UTC", automation_register(view, "windowEnd", automation_entry("00:00")), 0, 2);
    gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
    GtkWidget *days_frame = gtk_frame_new("Kullanılacak günler");
    GtkWidget *days = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "automation-weekdays");
    gtk_container_set_border_width(GTK_CONTAINER(days), 8);
    const gchar *weekdays[] = { "Pzt", "Sal", "Çar", "Per", "Cum", "Cmt", "Paz" };
    for (guint i = 0; i < G_N_ELEMENTS(weekdays); ++i) {
        GtkWidget *day = gtk_check_button_new_with_label(weekdays[i]);
        gchar *name = g_strdup_printf("automation-weekday-%u", i + 1); gtk_widget_set_name(day, name); g_free(name);
        view->weekdays[i] = day;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(day), TRUE);
        g_signal_connect(day, "toggled", G_CALLBACK(automation_changed), view);
        gtk_box_pack_start(GTK_BOX(days), day, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(days_frame), days);
    gtk_box_pack_start(GTK_BOX(body), days_frame, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(timing), card, FALSE, FALSE, 0);
    card = automation_card("Parça geçişleri", NULL, &body, NULL);
    grid = automation_grid(2);
    const gchar *transitions[] = { "Açık · parça biter bitmez", "Açık · sabit bindirme / crossfade", "Açık · cue / auto-next işaretleri", "Kapalı · manuel geçiş", NULL };
    const gchar *boundaries[] = { "Parça geçiş kuralını kullan", "Mevcut parçayı bitir, sonra geç", "Playlistler arasında bindirme", "Cue / auto-next işaretinde geç", "Operatör geçişini bekle", NULL };
    automation_field(grid, "Otomatik parça geçişi", automation_register(view, "transition", radios_ui_combo(transitions, 0)), 0, 0);
    automation_field(grid, "Bindirme · saniye", automation_register(view, "overlap", automation_spin(0, 30, 0.1, 3)), 1, 0);
    automation_field(grid, "Playlist değişirken", automation_register(view, "boundaryTransition", radios_ui_combo(boundaries, 0)), 0, 1);
    automation_field(grid, "Playlist geçiş bindirmesi · saniye", automation_register(view, "boundaryOverlap", automation_spin(0, 30, 0.1, 3)), 1, 1);
    gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
    automation_register(view, "jingleDry", automation_check(body, "Jingle öncesi ve sonrası bindirmeyi kapat", TRUE));
    gtk_box_pack_start(GTK_BOX(timing), card, FALSE, FALSE, 0);
    card = automation_card("Tekrarlanabilir rastgelelik", NULL, &body, &head);
    GtkWidget *new_seed = radios_ui_button("Yeni anahtar");
    gtk_box_pack_end(GTK_BOX(head), new_seed, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), radios_ui_label("Rastgelelik anahtarı", NULL), FALSE, FALSE, 0);
    view->seed = automation_register(view, "seed", automation_entry("radio-s-1"));
    gtk_entry_set_max_length(GTK_ENTRY(view->seed), 64);
    gtk_box_pack_start(GTK_BOX(body), view->seed, FALSE, FALSE, 0);
    g_signal_connect(new_seed, "clicked", G_CALLBACK(automation_new_seed), view);
    gtk_box_pack_start(GTK_BOX(timing), card, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(view->stack), timing, "timing");

    GtkWidget *decks = radios_ui_box(GTK_ORIENTATION_VERTICAL, 17, "automation-pane");
    card = automation_card("AutoDJ · Deck ayrıştırma ve geçiş", NULL, &body, NULL);
    grid = automation_grid(2);
    const gchar *deck_modes[] = { "Seçili deckleri sırayla kullan", "Tek deck kullan", NULL };
    const gchar *jingle_decks[] = { "Müzik deck sırasını kullan", NULL };
    automation_field(grid, "Atama yöntemi", automation_register(view, "deckMode", radios_ui_combo(deck_modes, 0)), 0, 0);
    automation_field(grid, "Jingle decki · isteğe bağlı", automation_register(view, "jingleDeck", radios_ui_combo(jingle_decks, 0)), 1, 0);
    gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
    GtkWidget *deck_frame = gtk_frame_new("AutoDJ’ye ayrılacak deckler ve çıkış geçişleri");
    GtkWidget *deck_list = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "automation-deck-list");
    view->decks_box = deck_list;
    gtk_container_set_border_width(GTK_CONTAINER(deck_list), 15);
    gtk_box_pack_start(GTK_BOX(deck_list), radios_ui_label("Yayın motoru bağlı değil.", "automation-help"), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(deck_frame), deck_list);
    gtk_box_pack_start(GTK_BOX(body), deck_frame, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(decks), card, FALSE, FALSE, 0);
    card = automation_card("Boş kaynak ve kurala takılan parçalar", NULL, &body, NULL);
    grid = automation_grid(2);
    const gchar *fallbacks[] = { "Planı durdur ve sebebini göster", "Sonraki uygun playlisti dene", "Yedek playlisti dene", NULL };
    automation_field(grid, "Uygun parça bulunamazsa", automation_register(view, "fallbackMode", radios_ui_combo(fallbacks, 0)), 0, 0);
    automation_field(grid, "Yedek playlist", automation_register(view, "fallbackPlaylist", radios_ui_combo(playlists, 0)), 1, 0);
    gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);
    automation_register(view, "requireMetadata", automation_check(body, "Sanatçı bilgisi olmayan müzikleri seçme", FALSE));
    gtk_box_pack_start(GTK_BOX(decks), card, FALSE, FALSE, 0);
    card = automation_card("Canlı AutoDJ", NULL, &body, NULL);
    GtkWidget *live = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 7, "automation-actions");
    GtkWidget *start = radios_ui_button("Başlat");
    GtkWidget *stop = radios_ui_button("Durdur");
    view->live_start = start; view->live_stop = stop; gtk_widget_set_name(start, "automation-live-start"); gtk_widget_set_name(stop, "automation-live-stop");
    gtk_widget_set_sensitive(start, FALSE);
    gtk_widget_set_sensitive(stop, FALSE);
    gtk_widget_set_tooltip_text(start, "Canlı yayın motoru bağlantısı gerekli");
    gtk_widget_set_tooltip_text(stop, "Canlı yayın motoru bağlantısı gerekli");
    gtk_box_pack_start(GTK_BOX(live), start, FALSE, FALSE, 0);
    if (ctx->playout) {
        view->live_pause = radios_ui_button("Duraklat"); gtk_widget_set_name(view->live_pause, "automation-live-pause");
        gtk_box_pack_start(GTK_BOX(live), view->live_pause, FALSE, FALSE, 0); g_signal_connect(view->live_pause, "clicked", G_CALLBACK(automation_live_action), view);
        view->live_next = radios_ui_button("Sonraki"); gtk_widget_set_name(view->live_next, "automation-live-next");
        gtk_box_pack_start(GTK_BOX(live), view->live_next, FALSE, FALSE, 0); g_signal_connect(view->live_next, "clicked", G_CALLBACK(automation_live_action), view);
        g_signal_connect(start, "clicked", G_CALLBACK(automation_live_action), view); g_signal_connect(stop, "clicked", G_CALLBACK(automation_live_action), view);
        gtk_widget_set_tooltip_text(start, "Doğrulanmış playlist medyasıyla yerel ses akışını başlat"); gtk_widget_set_tooltip_text(stop, "Yerel otomasyon akışını durdur");
    }
    gtk_box_pack_start(GTK_BOX(live), stop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), live, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(decks), card, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(view->stack), decks, "decks");
    gtk_box_pack_start(GTK_BOX(layout), view->stack, TRUE, TRUE, 0);

    GtkWidget *sidebar = radios_ui_box(GTK_ORIENTATION_VERTICAL, 17, "automation-sidebar");
    gtk_widget_set_hexpand(sidebar, TRUE);
    gtk_widget_set_valign(sidebar, GTK_ALIGN_START);
    card = automation_card("Akış özeti", NULL, &body, &head);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "automation-summary-card");
    view->summary_name = radios_ui_label("Genel rotasyon", "task-help");
    gtk_box_pack_start(GTK_BOX(body), view->summary_name, FALSE, FALSE, 0);
    GtkWidget *facts = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, "automation-facts");
    view->summary_sources = automation_fact(facts, "Kaynak playlist", "0");
    view->summary_candidates = automation_fact(facts, "Aday parça", ctx->playout ? "—" : "0");
    view->summary_mode = automation_fact(facts, "Parça seçimi", "Rastgele");
    view->summary_decks = automation_fact(facts, "Hedef deck", "Atanmadı");
    gtk_label_set_max_width_chars(GTK_LABEL(view->summary_decks), 28);
    view->summary_gaps = automation_fact(facts, "Tekrar · parça / sanatçı", "60 / 30 dk");
    view->summary_saved = automation_fact(facts, "Son kayıt", "Kaydedilmedi");
    gtk_box_pack_start(GTK_BOX(body), facts, FALSE, FALSE, 0);
    GtkWidget *plan_actions = radios_ui_box(GTK_ORIENTATION_VERTICAL, 9, "automation-plan-actions");
    GtkWidget *preview = radios_ui_button("Önizleme oluştur");
    view->preview_button = preview;
    gtk_widget_set_name(preview, "automation-preview");
    gtk_style_context_add_class(gtk_widget_get_style_context(preview), "task-primary");
    GtkWidget *clear = radios_ui_button("Önizlemeyi temizle");
    view->preview_clear = clear; gtk_widget_set_name(clear, "automation-preview-clear");
    g_signal_connect(clear, "clicked", G_CALLBACK(automation_preview_clear), view);
    gtk_widget_set_sensitive(clear, FALSE);
    gtk_box_pack_start(GTK_BOX(plan_actions), preview, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(plan_actions), clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), plan_actions, FALSE, FALSE, 0);
    g_signal_connect(preview, "clicked", G_CALLBACK(automation_preview), view);
    gtk_box_pack_start(GTK_BOX(sidebar), card, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(layout), sidebar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), layout, FALSE, FALSE, 0);
    card = automation_card("Üretilen yayın sırası", NULL, &body, &head);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "automation-preview-card");
    GtkWidget *preview_actions = radios_ui_box(GTK_ORIENTATION_VERTICAL, 5, "automation-actions");
    GtkWidget *download = radios_ui_button("Planı indir");
    GtkWidget *apply = radios_ui_button("Program sırasına ekle");
    view->preview_download = download; view->preview_apply = apply;
    gtk_widget_set_name(download, "automation-preview-download"); gtk_widget_set_name(apply, "automation-preview-apply");
    g_signal_connect(download, "clicked", G_CALLBACK(automation_preview_download), view); g_signal_connect(apply, "clicked", G_CALLBACK(automation_preview_apply), view);
    gtk_widget_set_sensitive(download, FALSE);
    gtk_widget_set_sensitive(apply, FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(apply), "task-primary");
    gtk_box_pack_start(GTK_BOX(preview_actions), download, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(preview_actions), apply, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(head), preview_actions, FALSE, FALSE, 0);
    GtkWidget *preview_empty = radios_ui_box(GTK_ORIENTATION_VERTICAL, 9, "automation-empty");
    view->preview_empty = preview_empty;
    GtkWidget *empty_icon = radios_ui_label("↻", "automation-empty-icon");
    gtk_label_set_xalign(GTK_LABEL(empty_icon), 0.5f);
    gtk_box_pack_start(GTK_BOX(preview_empty), empty_icon, FALSE, FALSE, 0);
    GtkWidget *empty_title = radios_ui_label("Henüz sıra üretilmedi", "strong");
    gtk_label_set_xalign(GTK_LABEL(empty_title), 0.5f);
    gtk_box_pack_start(GTK_BOX(preview_empty), empty_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), preview_empty, FALSE, FALSE, 0);
    const gchar *plan_headers[] = { "Sıra / saat", "Parça", "Kaynak", "Deck", "Süre / bindirme", "Seçim nedeni" };
    GtkWidget *plan_table = radios_ui_table(plan_headers, G_N_ELEMENTS(plan_headers));
    view->preview_table = plan_table; gtk_widget_set_name(plan_table, "automation-preview-table");
    gtk_style_context_add_class(gtk_widget_get_style_context(plan_table), "automation-preview-table");
    gtk_widget_set_no_show_all(plan_table, TRUE);
    gtk_widget_hide(plan_table);
    gtk_box_pack_start(GTK_BOX(body), plan_table, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), card, FALSE, FALSE, 0);
    view->status = radios_ui_label("", "task-feedback automation-status");
    gtk_box_pack_start(GTK_BOX(page), view->status, FALSE, FALSE, 0);
    g_signal_connect(view->name, "changed", G_CALLBACK(automation_name_changed), view);
    g_signal_connect(track_mode, "changed", G_CALLBACK(automation_mode_changed), view);
    g_signal_connect(view->track_gap, "value-changed", G_CALLBACK(automation_gaps_changed), view);
    g_signal_connect(view->artist_gap, "value-changed", G_CALLBACK(automation_gaps_changed), view);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->tabs[0]), TRUE);
    view->ready = TRUE;
    automation_update_controls(view);
    g_signal_connect(page, "map", G_CALLBACK(automation_mapped), view);
    if (ctx->data) view->document = radios_data_document_new(ctx, page, "automation", "main", automation_apply, view);
    if (ctx->playout) view->playout_timer = g_timeout_add(100, automation_playout_tick, view);
    return radios_automation_calendar_new(ctx, page);
}
