#include "radios/views.h"
#include "radios/widgets.h"
#include "radios/data.h"
#include "radios/advert_views.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *page,*view,*search,*filter,*counts,*title,*meta,*badge,*versions;
    GtkWidget *facts[7],*advanced[6],*actions[4],*version_add;
    GtkListStore *store;
    GPtrArray *records;
    JsonObject *selected;
    RadioSAdvertViews *workflow;
    GtkWidget *report_counters[6];
    gulong selection_handler;
    gboolean action_loading;
} AdvertData;

static void advert_refresh(RadioSViewContext *ctx,GtkWidget *page);
static void campaign_saved(RadioSViewContext *ctx,GtkWidget *owner,JsonNode *result,const GError *error,gpointer user);
static void campaign_selection(GtkTreeSelection *selection,gpointer data);
static void campaign_filter(GtkEditable *entry,gpointer data);
static void campaign_action(GtkButton *button,gpointer data);

static void
advert_data_free(gpointer data)
{
    AdvertData *state=data;
    if(state->selected != NULL)json_object_unref(state->selected);
    if(state->records != NULL)g_ptr_array_unref(state->records);
    g_free(state);
}

static void
style(GtkWidget *widget, const gchar *name)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}

static void
pack(GtkWidget *box, GtkWidget *widget, gboolean expand)
{
    gtk_box_pack_start(GTK_BOX(box), widget, expand, expand, 0);
}

static GtkWidget *
box(GtkOrientation orientation, gint spacing, const gchar *name)
{
    return radios_ui_box(orientation, spacing, name);
}

static GtkWidget *
action(GtkWidget *parent, const gchar *text, RadioSViewContext *ctx,
       const gchar *message, gboolean enabled)
{
    GtkWidget *widget = radios_ui_button(text);
    if (message != NULL)
        radios_ui_bind_notice(widget, ctx, message);
    gtk_widget_set_sensitive(widget, enabled);
    pack(parent, widget, FALSE);
    return widget;
}

static GtkWidget *
fact(GtkWidget *parent, const gchar *name, const gchar *value)
{
    GtkWidget *row = box(GTK_ORIENTATION_HORIZONTAL, 16, "advert-fact-row");
    gtk_widget_set_margin_top(row, 10);
    gtk_widget_set_margin_bottom(row, 10);
    pack(row, radios_ui_label(name, "advert-fact-name"), TRUE);
    GtkWidget *label = radios_ui_label(value, "advert-fact-value");
    gtk_label_set_xalign(GTK_LABEL(label), 1.0f);
    pack(row, label, TRUE);
    pack(parent, row, FALSE);
    return label;
}

static GtkWidget *
number(gdouble minimum, gdouble maximum, gdouble step, gdouble value)
{
    GtkWidget *widget = gtk_spin_button_new_with_range(minimum, maximum, step);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), value);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), step < 1 ? 2 : 0);
    return widget;
}

static GtkWidget *
find_field(GtkWidget *widget, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(widget), name))
        return widget;
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        GtkWidget *found = NULL;
        for (GList *item = children; item != NULL && found == NULL; item = item->next)
            found = find_field(item->data, name);
        g_list_free(children);
        return found;
    }
    return NULL;
}

static const gchar *
entry_text(GtkWidget *form, const gchar *name)
{
    GtkWidget *widget = find_field(form, name);
    return GTK_IS_ENTRY(widget) ? gtk_entry_get_text(GTK_ENTRY(widget)) : "";
}

static gboolean
date_valid(const gchar *text)
{
    gint year, month, day, consumed = 0;
    return g_regex_match_simple("^[0-9]{4}-[0-9]{2}-[0-9]{2}$", text, 0, 0) &&
        sscanf(text, "%4d-%2d-%2d%n", &year, &month, &day, &consumed) == 3 &&
        consumed == 10 && year >= 1 && year <= 9999 &&
        g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year);
}

static gboolean
form_error(GtkWidget *form, const gchar *message, const gchar *name)
{
    GtkWidget *status = g_object_get_data(G_OBJECT(form), "advert-status");
    gtk_label_set_text(GTK_LABEL(status), message);
    GtkWidget *field = name != NULL ? find_field(form, name) : NULL;
    if (field != NULL)
        gtk_widget_grab_focus(field);
    return FALSE;
}

static gboolean
required_entry(GtkWidget *form, const gchar *name, const gchar *label)
{
    gchar *value = g_strdup(entry_text(form, name));
    gboolean valid = *g_strstrip(value) != '\0';
    g_free(value);
    if (!valid) {
        gchar *message = g_strdup_printf("%s alanını doldurun.", label);
        form_error(form, message, name);
        g_free(message);
    }
    return valid;
}

static gboolean
time_valid(GtkWidget *form, const gchar *name, gboolean required)
{
    const gchar *value = entry_text(form, name);
    if (!required && *value == '\0')
        return TRUE;
    if (g_regex_match_simple("^(?:[01][0-9]|2[0-3]):[0-5][0-9]$", value, 0, 0))
        return TRUE;
    return form_error(form, "Saat alanını SS:DD biçiminde, 00:00–23:59 aralığında girin.", name);
}

static gboolean
validate_form(GtkWidget *dialog, GtkWidget *form)
{
    const gchar *name = gtk_widget_get_name(dialog);
    if (g_str_equal(name, "advert-campaign-dialog")) {
        const gchar *required[][2] = {
            {"advert-campaign-name", "Kampanya adı"},
            {"advert-campaign-spot", "Spot / ses adı"},
            {"advert-campaign-version", "Spot sürümü"}
        };
        for (guint i = 0; i < G_N_ELEMENTS(required); ++i)
            if (!required_entry(form, required[i][0], required[i][1]))
                return FALSE;
        const gchar *start = entry_text(form, "advert-campaign-start");
        const gchar *end = entry_text(form, "advert-campaign-end");
        if (!date_valid(start))
            return form_error(form, "Başlangıç tarihi geçerli YYYY-AA-GG biçiminde olmalı.", "advert-campaign-start");
        if (!date_valid(end) || strcmp(end, start) < 0)
            return form_error(form, "Bitiş tarihi geçerli olmalı ve başlangıçtan önce olmamalı.", "advert-campaign-end");
        if (!time_valid(form, "advert-campaign-slot", FALSE))
            return FALSE;
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(find_field(form, "advert-campaign-contact")));
        if (gtk_text_buffer_get_char_count(buffer) > 1000)
            return form_error(form, "İletişim / satış notu en fazla 1000 karakter olabilir.", "advert-campaign-contact");
    } else if (g_str_equal(name, "advert-break-dialog")) {
        if (!required_entry(form, "advert-k-name", "Kuşak adı") ||
            !time_valid(form, "advert-k-time", TRUE))
            return FALSE;
    } else if (g_str_equal(name, "advert-version-dialog")) {
        if (!required_entry(form, "advert-v-name", "Sürüm adı") ||
            !time_valid(form, "advert-v-from", FALSE) ||
            !time_valid(form, "advert-v-to", FALSE))
            return FALSE;
    } else if (g_str_equal(name, "advert-result-dialog")) {
        return form_error(form, "Sonuç kaydı için gerçekleşme tablosundan bir yayın seçin; veri servisi bekleniyor.", NULL);
    }
    return TRUE;
}

static void
form_field(GtkWidget *grid, guint index, const gchar *title,
           const gchar *name, GtkWidget *widget)
{
    GtkWidget *group = box(GTK_ORIENTATION_VERTICAL, 6, "advert-field");
    pack(group, radios_ui_label(title, "form-label"), FALSE);
    gtk_widget_set_name(widget, name);
    if (GTK_IS_ENTRY(widget) && !GTK_IS_SPIN_BUTTON(widget)) {
        gint maximum = 160;
        if (g_str_has_suffix(name, "start") || g_str_has_suffix(name, "end"))
            maximum = 10;
        else if (g_str_has_suffix(name, "time") || g_str_has_suffix(name, "slot") ||
                 g_str_has_suffix(name, "from") || g_str_has_suffix(name, "to"))
            maximum = 5;
        else if (g_str_equal(name, "advert-k-name"))
            maximum = 60;
        else if (g_str_has_suffix(name, "version"))
            maximum = 40;
        else if (g_str_has_suffix(name, "name") || g_str_has_suffix(name, "client") ||
                 g_str_has_suffix(name, "owner") || g_str_has_suffix(name, "reference"))
            maximum = 120;
        gtk_entry_set_max_length(GTK_ENTRY(widget), maximum);
    }
    pack(group, widget, FALSE);
    gtk_widget_set_hexpand(group, TRUE);
    gtk_grid_attach(GTK_GRID(grid), group, (gint)(index % 2),
                    (gint)(index / 2), 1, 1);
}

static GtkWidget *
field_grid(void)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 14);
    style(grid, "advert-fields");
    return grid;
}

static void
campaign_response(GtkDialog *dialog, gint response, gpointer data)
{
    RadioSViewContext *ctx = data;
    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }
    GtkWidget *form = g_object_get_data(G_OBJECT(dialog), "advert-form");
    if (!validate_form(GTK_WIDGET(dialog), form))
        return;
    if(radios_data_connected(ctx)) {
        JsonNode *body=radios_data_body(ctx);JsonObject *object=json_node_get_object(body);
        json_object_set_string_member(object,"name",entry_text(form,"advert-campaign-name"));
        JsonObject *fields=json_object_new();
        const gchar *ids[]={"client","owner","reference","start","end","target","spot","version","slot","competition"};
        const gchar *keys[]={"advertiser","owner","reference","start_date","end_date","target_hint","spot_title","version_label","slot","competition"};
        for(guint i=0;i<G_N_ELEMENTS(ids);++i){gchar *id=g_strdup_printf("advert-campaign-%s",ids[i]);json_object_set_string_member(fields,keys[i],entry_text(form,id));g_free(id);}
        const gchar *numbers[]={"duration","count","capacity","intro","outro","announcement"};
        const gchar *number_keys[]={"duration_seconds","planned_count","capacity_seconds","intro_seconds","outro_seconds","announcement_seconds"};
        for(guint i=0;i<G_N_ELEMENTS(numbers);++i){gchar *id=g_strdup_printf("advert-campaign-%s",numbers[i]);json_object_set_int_member(fields,number_keys[i],gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(find_field(form,id))));g_free(id);}
        json_object_set_double_member(fields,"price",gtk_spin_button_get_value(GTK_SPIN_BUTTON(find_field(form,"advert-campaign-price"))));
        gchar *currency=gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(find_field(form,"advert-campaign-currency")));json_object_set_string_member(fields,"currency",currency != NULL?currency:"TRY");g_free(currency);
        json_object_set_int_member(fields,"position",gtk_combo_box_get_active(GTK_COMBO_BOX(find_field(form,"advert-campaign-position"))));
        json_object_set_int_member(fields,"fallback",gtk_combo_box_get_active(GTK_COMBO_BOX(find_field(form,"advert-campaign-fallback"))));
        json_object_set_boolean_member(fields,"skip_empty",gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find_field(form,"advert-campaign-skip-empty"))));
        GtkTextBuffer *buffer=gtk_text_view_get_buffer(GTK_TEXT_VIEW(find_field(form,"advert-campaign-contact")));GtkTextIter start,end;gtk_text_buffer_get_bounds(buffer,&start,&end);gchar *contact=gtk_text_buffer_get_text(buffer,&start,&end,FALSE);json_object_set_string_member(fields,"contact",contact);g_free(contact);
        json_object_set_object_member(object,"fields",fields);
        const gchar *write_key=g_object_get_data(G_OBJECT(dialog),"advert-write-key");
        if(write_key == NULL){gchar *key=g_uuid_string_random();g_object_set_data_full(G_OBJECT(dialog),"advert-write-key",key,g_free);write_key=key;}
        json_object_set_string_member(object,"idempotency_key",write_key);
        JsonObject *record=g_object_get_data(G_OBJECT(dialog),"advert-record");
        if(record != NULL){json_object_set_string_member(object,"campaign_id",radios_json_string(record,"_id",""));json_object_set_int_member(object,"expected_revision",radios_json_int(record,"_rev",0));}
        gboolean accepted=radios_data_request(ctx,GTK_WIDGET(dialog),"POST","/v1/workflows/desktop/campaign-save",body,campaign_saved,NULL,NULL);json_node_free(body);
        if(accepted){gtk_dialog_set_response_sensitive(dialog,GTK_RESPONSE_ACCEPT,FALSE);form_error(form,"Kampanya taslağı veritabanına kaydediliyor…",NULL);}
        return;
    }
    const gchar *message = "Veri servisi henüz bağlı değil; kayıt yapılmadı. Alanlarınız bu pencerede korunuyor.";
    form_error(form, message, NULL);
    if (ctx->notify != NULL)
        ctx->notify(ctx->notify_data, message);
}

static GtkWidget *
editor_dialog(RadioSViewContext *ctx, const gchar *title, const gchar *id,
              const gchar *save_label, GtkWidget **out_form)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, ctx->parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Vazgeç", GTK_RESPONSE_CANCEL, save_label, GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 680, 740);
    style(dialog, "radios");
    style(dialog, "advert-dialog");
    gtk_widget_set_name(dialog, id);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *form = box(GTK_ORIENTATION_VERTICAL, 18, "advert-form");
    gtk_container_set_border_width(GTK_CONTAINER(form), 24);
    gtk_container_add(GTK_CONTAINER(scroll), form);
    pack(gtk_dialog_get_content_area(GTK_DIALOG(dialog)), scroll, TRUE);
    GtkWidget *status = radios_ui_label("", "advert-sub");
    gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
    pack(form, status, FALSE);
    g_object_set_data(G_OBJECT(form), "advert-status", status);
    g_object_set_data(G_OBJECT(dialog), "advert-form", form);
    g_signal_connect(dialog, "response", G_CALLBACK(campaign_response), ctx);
    *out_form = form;
    return dialog;
}

static void
campaign_saved(RadioSViewContext *ctx,GtkWidget *owner,JsonNode *result,const GError *error,gpointer user)
{
    (void)user;
    GtkWidget *form=g_object_get_data(G_OBJECT(owner),"advert-form");
    gtk_dialog_set_response_sensitive(GTK_DIALOG(owner),GTK_RESPONSE_ACCEPT,TRUE);
    if(error != NULL || result == NULL){form_error(form,error != NULL?error->message:"Veri servisi kayıt döndürmedi.",NULL);return;}
    GtkWidget *page=g_object_get_data(G_OBJECT(owner),"advert-page");
    if(page)radios_advert_page_refresh(ctx,page);
    gtk_widget_destroy(owner);
    if(ctx->notify != NULL)ctx->notify(ctx->notify_data,"Reklam kaydı veritabanına kaydedildi; ses ve yayın onayı verilmedi.");
}

static void
campaign_editor_record(RadioSViewContext *ctx,JsonObject *record,GtkWidget *page)
{
    GtkWidget *form;
    GtkWidget *dialog = editor_dialog(ctx, "Kampanya ekle", "advert-campaign-dialog", "Kampanyayı kaydet", &form);
    g_object_set_data(G_OBJECT(dialog),"advert-page",page);
    g_signal_connect_object(page,"destroy",G_CALLBACK(gtk_widget_destroy),dialog,G_CONNECT_SWAPPED);
    GtkWidget *fields = field_grid();
    form_field(fields, 0, "Kampanya adı", "advert-campaign-name", radios_ui_entry(NULL));
    form_field(fields, 1, "Reklamveren", "advert-campaign-client", radios_ui_entry("Belirtilmedi"));
    form_field(fields, 2, "Sorumlu kişi", "advert-campaign-owner", radios_ui_entry("Atanmadı"));
    form_field(fields, 3, "Sözleşme / sipariş referansı", "advert-campaign-reference", radios_ui_entry("Belirtilmedi"));
    form_field(fields, 4, "Başlangıç tarihi", "advert-campaign-start", radios_ui_entry("YYYY-AA-GG"));
    form_field(fields, 5, "Bitiş tarihi", "advert-campaign-end", radios_ui_entry("YYYY-AA-GG"));
    form_field(fields, 6, "Hedef istasyon / çıkış", "advert-campaign-target", radios_ui_entry("Atanmadı"));
    form_field(fields, 7, "Spot / ses adı", "advert-campaign-spot", radios_ui_entry(NULL));
    GtkWidget *version = radios_ui_entry(NULL);
    gtk_entry_set_text(GTK_ENTRY(version), "v1");
    form_field(fields, 8, "Spot sürümü", "advert-campaign-version", version);
    form_field(fields, 9, "Spot süresi · saniye", "advert-campaign-duration", number(1, 3600, 1, 30));
    form_field(fields, 10, "Planlanan yayın adedi", "advert-campaign-count", number(0, 100000, 1, 0));
    pack(form, fields, FALSE);
    GtkWidget *advanced = gtk_expander_new("Satış ve kuşak ayrıntıları");
    style(advanced, "advert-advanced-form");
    GtkWidget *advanced_body = box(GTK_ORIENTATION_VERTICAL, 14, NULL);
    fields = field_grid();
    const gchar *const currencies[] = {"TRY", "EUR", "USD", NULL};
    const gchar *const positions[] = {"Herhangi", "İlk", "Son", NULL};
    const gchar *const fallbacks[] = {"Eksik spotu atla", "Onaylı alternatifi kullan", "Operatör kararı bekle", NULL};
    form_field(fields, 0, "Sözleşme bedeli · isteğe bağlı", "advert-campaign-price", number(0, 1000000000, 0.01, 0));
    form_field(fields, 1, "Para birimi", "advert-campaign-currency", radios_ui_combo(currencies, 0));
    GtkWidget *notes = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(notes), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_size_request(notes, -1, 76);
    form_field(fields, 2, "İletişim / satış notu", "advert-campaign-contact", notes);
    form_field(fields, 3, "İlk reklam kuşağı · isteğe bağlı", "advert-campaign-slot", radios_ui_entry("SS:DD"));
    form_field(fields, 4, "Kuşak kapasitesi · saniye", "advert-campaign-capacity", number(0, 86400, 1, 0));
    form_field(fields, 5, "Intro · saniye", "advert-campaign-intro", number(0, 3600, 1, 0));
    form_field(fields, 6, "Outro · saniye", "advert-campaign-outro", number(0, 3600, 1, 0));
    form_field(fields, 7, "Sabit anons · saniye", "advert-campaign-announcement", number(0, 3600, 1, 0));
    form_field(fields, 8, "Rekabet grubu", "advert-campaign-competition", radios_ui_entry("Belirtilmedi"));
    form_field(fields, 9, "Kuşaktaki sıra", "advert-campaign-position", radios_ui_combo(positions, 0));
    form_field(fields, 10, "Dağıtım eksik olduğunda", "advert-campaign-fallback", radios_ui_combo(fallbacks, 0));
    pack(advanced_body, fields, FALSE);
    GtkWidget *skip = gtk_check_button_new_with_label("Boş kuşakta reklam jingle'ını atla");
    gtk_widget_set_name(skip, "advert-campaign-skip-empty");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(skip), TRUE);
    pack(advanced_body, skip, FALSE);
    gtk_container_add(GTK_CONTAINER(advanced), advanced_body);
    pack(form, advanced, FALSE);
    if(record != NULL){
        g_object_set_data_full(G_OBJECT(dialog),"advert-record",json_object_ref(record),(GDestroyNotify)json_object_unref);
        gtk_entry_set_text(GTK_ENTRY(find_field(form,"advert-campaign-name")),radios_json_string(record,"name",""));
        JsonObject *saved=json_object_has_member(record,"desktop_fields")?json_object_get_object_member(record,"desktop_fields"):NULL;
        if(saved != NULL){
            const gchar *ids[]={"client","owner","reference","start","end","target","spot","version","slot","competition"};
            const gchar *keys[]={"advertiser","owner","reference","start_date","end_date","target_hint","spot_title","version_label","slot","competition"};
            for(guint i=0;i<G_N_ELEMENTS(ids);++i){gchar *id=g_strdup_printf("advert-campaign-%s",ids[i]);gtk_entry_set_text(GTK_ENTRY(find_field(form,id)),radios_json_string(saved,keys[i],""));g_free(id);}
            const gchar *ids_number[]={"duration","count","capacity","intro","outro","announcement","price"};
            const gchar *keys_number[]={"duration_seconds","planned_count","capacity_seconds","intro_seconds","outro_seconds","announcement_seconds","price"};
            for(guint i=0;i<G_N_ELEMENTS(ids_number);++i){gchar *id=g_strdup_printf("advert-campaign-%s",ids_number[i]);gtk_spin_button_set_value(GTK_SPIN_BUTTON(find_field(form,id)),json_object_get_double_member(saved,keys_number[i]));g_free(id);}
            const gchar *currency=radios_json_string(saved,"currency","TRY");gtk_combo_box_set_active(GTK_COMBO_BOX(find_field(form,"advert-campaign-currency")),g_str_equal(currency,"EUR")?1:g_str_equal(currency,"USD")?2:0);
            gtk_combo_box_set_active(GTK_COMBO_BOX(find_field(form,"advert-campaign-position")),(gint)radios_json_int(saved,"position",0));
            gtk_combo_box_set_active(GTK_COMBO_BOX(find_field(form,"advert-campaign-fallback")),(gint)radios_json_int(saved,"fallback",0));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find_field(form,"advert-campaign-skip-empty")),json_object_get_boolean_member(saved,"skip_empty"));
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(find_field(form,"advert-campaign-contact"))),radios_json_string(saved,"contact",""),-1);
        }
        JsonNode *spots_node=json_object_get_member(record,"spots");JsonArray *spots=spots_node&&JSON_NODE_HOLDS_ARRAY(spots_node)?json_node_get_array(spots_node):NULL;gboolean approved_spot=FALSE;
        for(guint i=0;spots&&i<json_array_get_length(spots);++i)approved_spot|=g_str_equal(radios_json_string(json_array_get_object_element(spots,i),"status",""),"approved");
        if(approved_spot){const gchar *locked[]={"client","start","end","count","slot","capacity","competition","position"};
            for(guint i=0;i<G_N_ELEMENTS(locked);++i){gchar *id=g_strdup_printf("advert-campaign-%s",locked[i]);GtkWidget *input=find_field(form,id);g_free(id);gtk_widget_set_sensitive(input,FALSE);gtk_widget_set_tooltip_text(input,"Onaylı spotun trafik sözleşmesi korunur; bu alan mevcut onayı değiştirmez.");}
        }
    }
    gtk_widget_show_all(dialog);
}
static void campaign_editor(GtkButton *button,gpointer data)
{AdvertData *state=data;campaign_editor_record(state->ctx,g_object_get_data(G_OBJECT(button),"advert-record"),state->page);}

/* A new spot draft version of the selected draft campaign
 * (desktop/campaign-version); the campaign then points at it. */
static void
version_response(GtkDialog *dialog,gint response,gpointer data)
{
    RadioSViewContext *ctx=data;
    if(response != GTK_RESPONSE_ACCEPT){gtk_widget_destroy(GTK_WIDGET(dialog));return;}
    GtkWidget *form=g_object_get_data(G_OBJECT(dialog),"advert-form");
    if(!validate_form(GTK_WIDGET(dialog),form))return;
    if(!radios_data_connected(ctx)){form_error(form,"Veri servisi henüz bağlı değil; kayıt yapılmadı. Alanlarınız bu pencerede korunuyor.",NULL);return;}
    JsonObject *record=g_object_get_data(G_OBJECT(dialog),"advert-record");
    JsonNode *body=radios_data_body(ctx);JsonObject *object=json_node_get_object(body);
    json_object_set_string_member(object,"campaign_id",radios_json_string(record,"_id",""));
    json_object_set_int_member(object,"expected_revision",radios_json_int(record,"_rev",0));
    gchar *name=g_strstrip(g_strdup(entry_text(form,"advert-v-name")));json_object_set_string_member(object,"name",name);g_free(name);
    json_object_set_int_member(object,"duration_seconds",gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(find_field(form,"advert-v-duration"))));
    json_object_set_string_member(object,"valid_from_time",entry_text(form,"advert-v-from"));
    json_object_set_string_member(object,"valid_to_time",entry_text(form,"advert-v-to"));
    gchar *file=g_strstrip(g_strdup(entry_text(form,"advert-v-file")));json_object_set_string_member(object,"file_reference",file);g_free(file);
    const gchar *write_key=g_object_get_data(G_OBJECT(dialog),"advert-write-key");
    if(write_key == NULL){gchar *key=g_uuid_string_random();g_object_set_data_full(G_OBJECT(dialog),"advert-write-key",key,g_free);write_key=key;}
    json_object_set_string_member(object,"idempotency_key",write_key);
    if(radios_data_request(ctx,GTK_WIDGET(dialog),"POST","/v1/workflows/desktop/campaign-version",body,campaign_saved,NULL,NULL)){
        gtk_dialog_set_response_sensitive(dialog,GTK_RESPONSE_ACCEPT,FALSE);form_error(form,"Spot sürümü veritabanına kaydediliyor…",NULL);
    }
    json_node_free(body);
}
static void
version_editor(GtkButton *button,gpointer data)
{
    AdvertData *state=data;JsonObject *record=g_object_get_data(G_OBJECT(button),"advert-record");
    if(record == NULL)return;
    GtkWidget *dialog=gtk_dialog_new_with_buttons("Yeni spot sürümü",state->ctx->parent,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,
        "Vazgeç",GTK_RESPONSE_CANCEL,"Sürümü kaydet",GTK_RESPONSE_ACCEPT,NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog),560,-1);style(dialog,"radios");style(dialog,"advert-dialog");
    gtk_widget_set_name(dialog,"advert-version-dialog");
    GtkWidget *form=box(GTK_ORIENTATION_VERTICAL,18,"advert-form");gtk_container_set_border_width(GTK_CONTAINER(form),24);
    pack(gtk_dialog_get_content_area(GTK_DIALOG(dialog)),form,TRUE);
    GtkWidget *status=radios_ui_label("","advert-sub");gtk_label_set_line_wrap(GTK_LABEL(status),TRUE);pack(form,status,FALSE);
    g_object_set_data(G_OBJECT(form),"advert-status",status);g_object_set_data(G_OBJECT(dialog),"advert-form",form);
    g_object_set_data(G_OBJECT(dialog),"advert-page",state->page);
    g_object_set_data_full(G_OBJECT(dialog),"advert-record",json_object_ref(record),(GDestroyNotify)json_object_unref);
    g_signal_connect_object(state->page,"destroy",G_CALLBACK(gtk_widget_destroy),dialog,G_CONNECT_SWAPPED);
    JsonObject *saved=json_object_has_member(record,"desktop_fields")?json_object_get_object_member(record,"desktop_fields"):NULL;
    GtkWidget *fields=field_grid();
    GtkWidget *name=radios_ui_entry(NULL);
    form_field(fields,0,"Sürüm adı","advert-v-name",name);gtk_entry_set_max_length(GTK_ENTRY(name),80);
    form_field(fields,1,"Spot süresi · saniye","advert-v-duration",number(1,600,1,CLAMP(saved?radios_json_int(saved,"duration_seconds",30):30,1,600)));
    form_field(fields,2,"Geçerlilik başlangıcı · isteğe bağlı","advert-v-from",radios_ui_entry("SS:DD"));
    form_field(fields,3,"Geçerlilik bitişi · isteğe bağlı","advert-v-to",radios_ui_entry("SS:DD"));
    GtkWidget *file=radios_ui_entry(NULL);
    form_field(fields,4,"Ses dosyası referansı · isteğe bağlı","advert-v-file",file);gtk_entry_set_max_length(GTK_ENTRY(file),4096);
    pack(form,fields,FALSE);
    g_signal_connect(dialog,"response",G_CALLBACK(version_response),state->ctx);
    gtk_widget_show_all(dialog);
}

static void
campaign_search(GtkEntry *entry, gpointer data)
{
    RadioSViewContext *ctx = data;
    (void)entry;
    if (!radios_data_connected(ctx) && ctx->notify != NULL)
        ctx->notify(ctx->notify_data, "Kampanya araması için veri servisi bağlantısı gerekiyor.");
}

static void
set_number_label(GtkWidget *label,gint64 value,const gchar *suffix)
{
    gchar *text=g_strdup_printf("%" G_GINT64_FORMAT " %s",value,suffix);gtk_label_set_text(GTK_LABEL(label),text);g_free(text);
}

static void
campaign_selection(GtkTreeSelection *selection,gpointer data)
{
    AdvertData *state=data;GtkTreeModel *model;GtkTreeIter iter;
    if(state->selected != NULL){json_object_unref(state->selected);state->selected=NULL;}
    if(gtk_tree_selection_get_selected(selection,&model,&iter)){
        gchar *key=NULL;gtk_tree_model_get(model,&iter,4,&key,-1);
        for(guint i=0;key != NULL && i<state->records->len;++i){JsonObject *record=g_ptr_array_index(state->records,i);if(g_str_equal(key,radios_json_string(record,"_id",""))){state->selected=json_object_ref(record);break;}}
        g_free(key);
    }
    JsonObject *record=state->selected;
    for(guint i=0;i<4;++i)gtk_widget_set_sensitive(state->actions[i],FALSE);
    if(state->version_add){gtk_widget_set_sensitive(state->version_add,FALSE);g_object_set_data(G_OBJECT(state->version_add),"advert-record",NULL);}
    if(record == NULL){gtk_label_set_text(GTK_LABEL(state->title),"Kampanya seçilmedi");radios_advert_views_select(state->workflow,NULL);return;}
    const gchar *status=radios_json_string(record,"status","");gboolean draft=g_str_equal(status,"draft");
    if(state->version_add){
        gtk_widget_set_sensitive(state->version_add,draft&&*radios_json_string(record,"desktop_item_id",""));
        g_object_set_data_full(G_OBJECT(state->version_add),"advert-record",json_object_ref(record),(GDestroyNotify)json_object_unref);
    }
    gtk_label_set_text(GTK_LABEL(state->title),radios_json_string(record,"name",""));gtk_label_set_text(GTK_LABEL(state->meta),radios_json_string(record,"advertiser_name",""));gtk_label_set_text(GTK_LABEL(state->badge),status);
    gboolean editable=draft||g_str_equal(status,"approved")||g_str_equal(status,"active")||g_str_equal(status,"paused");
    gtk_widget_set_sensitive(state->actions[0],editable&&json_object_has_member(record,"desktop_fields"));gtk_widget_set_sensitive(state->actions[1],draft);gtk_widget_set_sensitive(state->actions[2],editable);
    for(guint i=0;i<4;++i){g_object_set_data_full(G_OBJECT(state->actions[i]),"advert-record",json_object_ref(record),(GDestroyNotify)json_object_unref);gtk_widget_set_tooltip_text(state->actions[i],i==3?"Araç bölümünden gerçek planı olan bir kuşak seçin.":NULL);}
    JsonObject *fields=json_object_has_member(record,"desktop_fields")?json_object_get_object_member(record,"desktop_fields"):NULL;
    const gchar *text_keys[]={"advertiser","reference","target_hint"};
    for(guint i=0;i<3;++i)gtk_label_set_text(GTK_LABEL(state->facts[i]),fields != NULL?radios_json_string(fields,text_keys[i],"—"):i==0?radios_json_string(record,"advertiser_name","—"):"—");
    set_number_label(state->facts[3],fields != NULL?radios_json_int(fields,"planned_count",0):0,"kampanya hedefi");
    JsonNode *binding=json_object_get_member(record,"desktop_binding");
    gtk_label_set_text(GTK_LABEL(state->facts[4]),fields != NULL?radios_json_string(fields,"slot",""):"—");
    gtk_label_set_text(GTK_LABEL(state->facts[5]),binding&&JSON_NODE_HOLDS_OBJECT(binding)?"İçe alınmış sürüm bağı var · kuşakta yeniden doğrulanır":"Doğrulanmış ses bağı yok");
    gtk_label_set_text(GTK_LABEL(state->facts[6]),"Yerleşim ve yerel tüketim raporda ayrı gösterilir");
    if(fields != NULL){
        gchar number[G_ASCII_DTOSTR_BUF_SIZE];g_ascii_dtostr(number,sizeof number,json_object_get_double_member(fields,"price"));gchar *price=g_strdup_printf("%s %s",number,radios_json_string(fields,"currency","TRY"));gtk_label_set_text(GTK_LABEL(state->advanced[0]),price);g_free(price);
        gtk_label_set_text(GTK_LABEL(state->advanced[1]),radios_json_string(fields,"contact",""));set_number_label(state->advanced[2],radios_json_int(fields,"capacity_seconds",0),"sn");
        gchar *times=g_strdup_printf("%" G_GINT64_FORMAT " / %" G_GINT64_FORMAT " / %" G_GINT64_FORMAT " sn",radios_json_int(fields,"intro_seconds",0),radios_json_int(fields,"outro_seconds",0),radios_json_int(fields,"announcement_seconds",0));gtk_label_set_text(GTK_LABEL(state->advanced[3]),times);g_free(times);
        gtk_label_set_text(GTK_LABEL(state->advanced[4]),radios_json_string(fields,"competition",""));gtk_label_set_text(GTK_LABEL(state->advanced[5]),json_object_get_boolean_member(fields,"skip_empty")?"Boş kuşakta jingle atlanır":"Boş kuşak davranışı tanımlı");
        gchar *version=g_strdup_printf("%s · taslak sürüm · medya doğrulanmadı",radios_json_string(fields,"version_label",""));gtk_label_set_text(GTK_LABEL(state->versions),version);g_free(version);
    }
    radios_advert_views_select(state->workflow, record);
}

static void
campaign_filter(GtkEditable *entry,gpointer data)
{
    AdvertData *state=data;if(state->store == NULL)return;
    gchar *selected=state->selected != NULL?g_strdup(radios_json_string(state->selected,"_id","")):NULL;
    gchar *query=g_utf8_casefold(gtk_entry_get_text(GTK_ENTRY(entry)),-1);
    if(state->selection_handler)g_signal_handler_block(gtk_tree_view_get_selection(GTK_TREE_VIEW(state->view)),state->selection_handler);
    gtk_list_store_clear(state->store);guint shown=0,drafts=0,approved=0;
    const gchar *status_filter=state->filter?gtk_combo_box_get_active_id(GTK_COMBO_BOX(state->filter)):"";
    for(guint i=0;i<state->records->len;++i){JsonObject *record=g_ptr_array_index(state->records,i);
        JsonObject *fields=json_object_has_member(record,"desktop_fields")?json_object_get_object_member(record,"desktop_fields"):NULL;
        const gchar *record_status=radios_json_string(record,"status","");drafts+=g_str_equal(record_status,"draft");approved+=g_str_equal(record_status,"approved");
        gchar *text=g_strdup_printf("%s %s %s",radios_json_string(record,"name",""),radios_json_string(record,"advertiser_name",""),radios_json_string(fields,"spot_title",""));gchar *folded=g_utf8_casefold(text,-1);g_free(text);
        gboolean match=(*query=='\0'||strstr(folded,query)!=NULL)&&(!status_filter||!*status_filter||g_str_equal(status_filter,record_status));g_free(folded);if(!match)continue;++shown;
        gchar *interval=g_strdup_printf("%s — %s",radios_json_string(record,"starts_at",""),radios_json_string(record,"ends_at",""));gchar *count=g_strdup_printf("%" G_GINT64_FORMAT " hedef",fields != NULL?radios_json_int(fields,"planned_count",0):0);
        GtkTreeIter iter;gtk_list_store_append(state->store,&iter);gtk_list_store_set(state->store,&iter,0,radios_json_string(record,"name",""),1,interval,2,count,3,radios_json_string(record,"status",""),4,radios_json_string(record,"_id",""),-1);g_free(interval);g_free(count);
        if(selected != NULL&&g_str_equal(selected,radios_json_string(record,"_id","")))gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(state->view)),&iter);
    }
    if(state->selection_handler)g_signal_handler_unblock(gtk_tree_view_get_selection(GTK_TREE_VIEW(state->view)),state->selection_handler);
    campaign_selection(gtk_tree_view_get_selection(GTK_TREE_VIEW(state->view)),state);
    if(state->counts){gchar *counts=g_strdup_printf("%u kayıt · %u onaylı · %u onay bekliyor",shown,approved,drafts);gtk_label_set_text(GTK_LABEL(state->counts),counts);g_free(counts);}
    g_free(selected);g_free(query);
}

static void campaign_status_filter(GtkComboBox *combo,gpointer data)
{ (void)combo;AdvertData *state=data;campaign_filter(GTK_EDITABLE(state->search),state); }

static void
campaign_action_response(GtkDialog *dialog,gint response,gpointer data)
{
    RadioSViewContext *ctx=data;
    if(response != GTK_RESPONSE_ACCEPT){gtk_widget_destroy(GTK_WIDGET(dialog));return;}
    JsonObject *record=g_object_get_data(G_OBJECT(dialog),"advert-record");guint action_id=GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(dialog),"advert-action"));JsonNode *body=radios_data_body(ctx);JsonObject *object=json_node_get_object(body);
    json_object_set_string_member(object,"campaign_id",radios_json_string(record,"_id",""));json_object_set_int_member(object,"expected_revision",radios_json_int(record,"_rev",0));
    const gchar *path="/v1/workflows/desktop-advert/campaign-state";
    json_object_set_string_member(object,"state",action_id==2?"approve":"cancel");
    const gchar *key=g_object_get_data(G_OBJECT(dialog),"advert-action-key");if(key==NULL){gchar *created=g_uuid_string_random();g_object_set_data_full(G_OBJECT(dialog),"advert-action-key",created,g_free);key=created;}json_object_set_string_member(object,"idempotency_key",key);
    if(radios_data_request(ctx,GTK_WIDGET(dialog),"POST",path,body,campaign_saved,NULL,NULL))gtk_dialog_set_response_sensitive(dialog,GTK_RESPONSE_ACCEPT,FALSE);
    json_node_free(body);
}

typedef struct { guint action_id;gchar campaign_id[37]; } CampaignActionLoad;
static void
campaign_action_loaded(RadioSViewContext *ctx,GtkWidget *page,JsonNode *result,const GError *error,gpointer data)
{
    AdvertData *state=g_object_get_data(G_OBJECT(page),"advert-data");CampaignActionLoad *load=data;state->action_loading=FALSE;
    campaign_selection(gtk_tree_view_get_selection(GTK_TREE_VIEW(state->view)),state);
    if(error||!result||!JSON_NODE_HOLDS_OBJECT(result)){if(ctx->notify)ctx->notify(ctx->notify_data,error?error->message:"Kampanya okunamadı.");radios_advert_page_refresh(ctx,page);return;}
    JsonObject *record=json_object_get_object_member(json_node_get_object(result),"campaign");
    if(!record||!state->selected||!g_str_equal(load->campaign_id,radios_json_string(state->selected,"_id","")))return;
    guint action_id=load->action_id;
    if(action_id==1){campaign_editor_record(ctx,record,page);return;}
    if(action_id==2&&!g_str_equal(radios_json_string(record,"status",""),"draft")){radios_advert_page_refresh(ctx,page);return;}
    GtkWidget *dialog=gtk_dialog_new_with_buttons(action_id==2?"Kampanya sözleşmesini onayla":"Kampanyayı iptal et",state->ctx->parent,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,"Vazgeç",GTK_RESPONSE_CANCEL,action_id==2?"Onayla":"İptal et",GTK_RESPONSE_ACCEPT,NULL);
    gtk_widget_set_name(dialog,"advert-campaign-action-dialog");
    g_object_set_data(G_OBJECT(dialog),"advert-page",page);g_signal_connect_object(page,"destroy",G_CALLBACK(gtk_widget_destroy),dialog,G_CONNECT_SWAPPED);
    GtkWidget *form=box(GTK_ORIENTATION_VERTICAL,12,"advert-form");GtkWidget *status=radios_ui_label("","advert-sub");gtk_label_set_line_wrap(GTK_LABEL(status),TRUE);pack(form,status,FALSE);pack(gtk_dialog_get_content_area(GTK_DIALOG(dialog)),form,FALSE);
    g_object_set_data(G_OBJECT(form),"advert-status",status);g_object_set_data(G_OBJECT(dialog),"advert-form",form);g_object_set_data(G_OBJECT(dialog),"advert-action",GUINT_TO_POINTER(action_id));g_object_set_data_full(G_OBJECT(dialog),"advert-record",json_object_ref(record),(GDestroyNotify)json_object_unref);
    g_signal_connect(dialog,"response",G_CALLBACK(campaign_action_response),state->ctx);gtk_widget_show_all(dialog);
}
static void
campaign_action(GtkButton *button,gpointer data)
{
    AdvertData *state=data;if(!state->selected||state->action_loading)return;
    guint action_id=GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button),"advert-action"));
    CampaignActionLoad *load=g_new0(CampaignActionLoad,1);load->action_id=action_id;g_strlcpy(load->campaign_id,radios_json_string(state->selected,"_id",""),sizeof load->campaign_id);
    JsonNode *body=radios_data_body(state->ctx);json_object_set_string_member(json_node_get_object(body),"campaign_id",load->campaign_id);
    if(radios_data_request(state->ctx,state->page,"POST","/v1/workflows/desktop-advert/campaign-get",body,campaign_action_loaded,load,g_free)){state->action_loading=TRUE;gtk_widget_set_sensitive(GTK_WIDGET(button),FALSE);}
    json_node_free(body);
}

static void
advert_loaded(RadioSViewContext *ctx,GtkWidget *page,JsonNode *result,const GError *error,gpointer unused)
{
    (void)unused;AdvertData *state=g_object_get_data(G_OBJECT(page),"advert-data");
    if(error != NULL || result == NULL || !JSON_NODE_HOLDS_OBJECT(result)){if(ctx->notify != NULL)ctx->notify(ctx->notify_data,error != NULL?error->message:"Kampanyalar okunamadı.");return;}
    JsonArray *records=json_object_get_array_member(json_node_get_object(result),"records");g_ptr_array_set_size(state->records,0);
    for(guint i=0;records != NULL&&i<json_array_get_length(records);++i)g_ptr_array_add(state->records,json_object_ref(json_array_get_object_element(records,i)));
    campaign_filter(GTK_EDITABLE(state->search),state);
}

static void
advert_refresh(RadioSViewContext *ctx,GtkWidget *page)
{
    if(!radios_data_connected(ctx))return;
    AdvertData *state=g_object_get_data(G_OBJECT(page),"advert-data");
    radios_advert_views_refresh(state->workflow);
    JsonNode *body=radios_data_body(ctx);
    radios_data_request(ctx,page,"POST","/v1/workflows/desktop/campaign-list",body,advert_loaded,NULL,NULL);json_node_free(body);
}
void radios_advert_page_refresh(RadioSViewContext *ctx,GtkWidget *page)
{advert_refresh(ctx,page);}

static GtkWidget *
campaigns(RadioSViewContext *ctx,AdvertData *state)
{
    GtkWidget *card = box(GTK_ORIENTATION_VERTICAL, 12, "advert-card");
    GtkWidget *heading = box(GTK_ORIENTATION_HORIZONTAL, 12, "advert-toolbar");
    pack(heading, radios_ui_label("Kampanyalar", "heading"), TRUE);
    GtkWidget *add = action(heading, "+ Kampanya", ctx, NULL, TRUE);
    style(add, "advert-primary");
    g_signal_connect(add, "clicked", G_CALLBACK(campaign_editor), state);
    gtk_widget_set_name(add,"advert-campaign-add");
    pack(card, heading, FALSE);
    pack(card, radios_ui_label("Kampanya ara", "advert-search-label"), FALSE);
    GtkWidget *search = radios_ui_entry("Kampanya, reklamveren veya spot");
    gtk_widget_set_name(search, "advert-campaign-search");
    g_signal_connect(search, "activate", G_CALLBACK(campaign_search), ctx);
    state->search=search;
    g_signal_connect(search,"changed",G_CALLBACK(campaign_filter),state);
    pack(card, search, FALSE);
    GtkWidget *filters=box(GTK_ORIENTATION_HORIZONTAL,8,"advert-toolbar");
    state->filter=gtk_combo_box_text_new();gtk_widget_set_name(state->filter,"advert-campaign-filter");
    const gchar *filter_ids[]={"","draft","approved","paused","cancelled"};
    const gchar *filter_names[]={"Tümü","Onay bekleyen","Onaylı","Askıda","İptal edilen"};
    for(guint i=0;i<G_N_ELEMENTS(filter_ids);++i)gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->filter),filter_ids[i],filter_names[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->filter),0);g_signal_connect(state->filter,"changed",G_CALLBACK(campaign_status_filter),state);
    pack(filters,state->filter,FALSE);state->counts=radios_ui_label("—","advert-sub");gtk_widget_set_name(state->counts,"advert-campaign-counts");pack(filters,state->counts,TRUE);pack(card,filters,FALSE);
    const gchar *headers[] = {"Kampanya / spot", "Geçerlilik", "Plan", "Onay"};
    GtkWidget *table = radios_ui_table(headers, G_N_ELEMENTS(headers));
    style(table, "advert-table-scroll");
    GtkWidget *view = g_object_get_data(G_OBJECT(table), "tree-view");
    state->view=view;
    state->store=gtk_list_store_new(5,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING);
    gtk_tree_view_set_model(GTK_TREE_VIEW(view),GTK_TREE_MODEL(state->store));
    g_object_set_data(G_OBJECT(table),"list-store",state->store);g_object_unref(state->store);
    gtk_widget_set_name(view,"advert-campaign-table");
    state->selection_handler=g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)),"changed",G_CALLBACK(campaign_selection),state);
    style(view, "advert-table");
    pack(card, table, TRUE);
    gtk_widget_set_valign(card, GTK_ALIGN_START);
    return card;
}

static GtkWidget *
details(RadioSViewContext *ctx,AdvertData *state)
{
    GtkWidget *card = box(GTK_ORIENTATION_VERTICAL, 14, "advert-card");
    style(card, "advert-details");
    GtkWidget *heading = box(GTK_ORIENTATION_HORIZONTAL, 12, "advert-toolbar");
    GtkWidget *name = box(GTK_ORIENTATION_VERTICAL, 6, NULL);
    state->title=radios_ui_label("Kampanya seçilmedi", "heading");pack(name,state->title,FALSE);
    state->meta=radios_ui_label("—", "advert-meta");pack(name,state->meta,FALSE);
    pack(heading, name, TRUE);
    state->badge=radios_ui_label("—", "advert-badge");pack(heading,state->badge,FALSE);
    pack(card, heading, FALSE);
    GtkWidget *actions = box(GTK_ORIENTATION_HORIZONTAL, 7, "advert-actions");
    const gchar *names[] = {"Düzenle", "Onayla", "Sil", "Planı indir"};
    const gchar *ids[] = {"advert-campaign-edit","advert-campaign-approve","advert-campaign-cancel","advert-plan-export"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *button = action(actions, names[i], ctx, NULL, FALSE);
        gtk_widget_set_name(button,ids[i]);
        state->actions[i]=button;
        g_object_set_data(G_OBJECT(button),"advert-action",GUINT_TO_POINTER(i+1));
        if(i<3)g_signal_connect(button,"clicked",G_CALLBACK(campaign_action),state);
        gtk_widget_set_tooltip_text(button, "Önce kampanya listesinden bir kayıt seçin; veri servisi bekleniyor.");
    }
    pack(card, actions, FALSE);
    GtkWidget *facts = box(GTK_ORIENTATION_VERTICAL, 0, "advert-facts");
    const gchar *fact_names[]={"Reklamveren / sorumlu","Sözleşme / sipariş","Hedef istasyon / çıkış","Planlanan yayın","İlk kuşak planı","Ana / yedek dosya","Aktif listeye uygulanma"};
    for(guint i=0;i<G_N_ELEMENTS(fact_names);++i)state->facts[i]=fact(facts,fact_names[i],"—");
    pack(card, facts, FALSE);
    GtkWidget *advanced = gtk_expander_new("Sözleşme ve kuşak ayrıntıları");
    style(advanced, "advert-advanced-details");
    facts = box(GTK_ORIENTATION_VERTICAL, 0, "advert-facts");
    const gchar *advanced_names[]={"Sözleşme bedeli","İletişim / satış notu","Kuşak kapasitesi","Intro / outro / sabit anons","Rekabet grubu / sıra","Boş kuşak / dağıtım eksikliği"};
    for(guint i=0;i<G_N_ELEMENTS(advanced_names);++i)state->advanced[i]=fact(facts,advanced_names[i],"—");
    gtk_container_add(GTK_CONTAINER(advanced), facts);
    pack(card, advanced, FALSE);
    GtkWidget *versions = gtk_expander_new("Sürüm geçmişi");
    style(versions, "advert-version-history");
    GtkWidget *history=box(GTK_ORIENTATION_VERTICAL,8,NULL);
    state->versions=radios_ui_label("—",NULL);pack(history,state->versions,FALSE);
    GtkWidget *history_actions=box(GTK_ORIENTATION_HORIZONTAL,7,"advert-actions");
    state->version_add=action(history_actions,"Yeni spot sürümü",ctx,NULL,FALSE);gtk_widget_set_name(state->version_add,"advert-version-add");
    gtk_widget_set_tooltip_text(state->version_add,"Taslak kampanyaya yeni spot sürümü ekler.");
    g_signal_connect(state->version_add,"clicked",G_CALLBACK(version_editor),state);
    pack(history,history_actions,FALSE);gtk_container_add(GTK_CONTAINER(versions),history);
    pack(card, versions, FALSE);
    GtkWidget *report = box(GTK_ORIENTATION_VERTICAL, 12, "advert-reconciliation");
    GtkWidget *report_title = box(GTK_ORIENTATION_HORIZONTAL, 0, "advert-toolbar");
    pack(report_title, radios_ui_label("Gerçekleşme ve mutabakat", "heading"), FALSE);
    pack(report, report_title, FALSE);
    GtkWidget *summary = box(GTK_ORIENTATION_HORIZONTAL, 12, "advert-inline-summary");
    const gchar *states[] = {"Planlanan", "Tamamlanan", "Kesilen", "Atlanan", "Başarısız", "Belirsiz"};
    for (guint i = 0; i < G_N_ELEMENTS(states); ++i) {
        GtkWidget *group = box(GTK_ORIENTATION_VERTICAL, 4, NULL);
        pack(group, radios_ui_label(states[i], NULL), FALSE);
        state->report_counters[i]=radios_ui_label("—", NULL);
        pack(group, state->report_counters[i], FALSE);
        pack(summary, group, TRUE);
    }
    pack(report, summary, FALSE);
    const gchar *headers[] = {"Saat", "Sürüm", "Çıkış", "Sonuç / neden"};
    GtkWidget *table = radios_ui_table(headers, G_N_ELEMENTS(headers));
    style(table, "advert-table-scroll");
    style(g_object_get_data(G_OBJECT(table), "tree-view"), "advert-table");
    pack(report, table, FALSE);
    actions = box(GTK_ORIENTATION_HORIZONTAL, 7, "advert-actions");
    GtkWidget *download = action(actions, "Gerçekleşme kanıtını indir", ctx, NULL, FALSE);
    gtk_widget_set_tooltip_text(download, "İndirmek için seçili kampanyaya ait gerçek yayın kayıtları gerekir.");
    pack(report, actions, FALSE);
    radios_advert_views_bind_report(state->workflow,state->versions,state->report_counters,
        g_object_get_data(G_OBJECT(table),"tree-view"),download,state->actions[3],state->facts[5],state->facts[6]);
    pack(card, report, FALSE);
    gtk_widget_set_valign(card, GTK_ALIGN_START);
    return card;
}

GtkWidget *
radios_advert_new(RadioSViewContext *ctx)
{
    GtkWidget *page = box(GTK_ORIENTATION_VERTICAL, 20, "advert-workspace");
    AdvertData *state=g_new0(AdvertData,1);state->ctx=ctx;state->page=page;state->records=g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
    g_object_set_data_full(G_OBJECT(page),"advert-data",state,advert_data_free);
    gtk_widget_set_name(page, "advert-task-view");
    state->workflow=radios_advert_views_new(ctx,page);
    pack(page, radios_advert_views_notifications(state->workflow), FALSE);
    GtkWidget *grid = gtk_grid_new();
    style(grid, "advert-grid");
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    gtk_grid_attach(GTK_GRID(grid), campaigns(ctx,state), 0, 0, 7, 1);
    gtk_grid_attach(GTK_GRID(grid), details(ctx,state), 7, 0, 5, 1);
    pack(page, grid, FALSE);
    pack(page, radios_advert_views_tools(state->workflow), FALSE);
    if(ctx->data != NULL)radios_data_register_refresh(ctx,page,advert_refresh);
    return page;
}
