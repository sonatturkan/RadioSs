#include "radios/views.h"
#include "radios/widgets.h"
#include "radios/data.h"
#include "radios/audio_ui.h"
#include "radios/admin_ui.h"
#include "radios/workflow_hub.h"
#include "radios/external_access_ui.h"
#include "radios/external_access.h"
#include "radios/playout_ui.h"
#include "radios/programme_prerequisites.h"
#include "radios/midi_ui.h"
#include "radios/broadcast_output_ui.h"
#include <stdio.h>
#include <string.h>

static void server_editor(GtkButton *button, gpointer data);
static void technical_refresh(RadioSViewContext *ctx, GtkWidget *page);

static void
technical_saved(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result,
                const GError *error, gpointer unused)
{
    GtkWidget *form = g_object_get_data(G_OBJECT(owner), "technical-form");
    GtkWidget *status = g_object_get_data(G_OBJECT(form), "technical-status-label");
    (void)unused;
    gtk_dialog_set_response_sensitive(GTK_DIALOG(owner), GTK_RESPONSE_ACCEPT, TRUE);
    if (error != NULL || result == NULL || !JSON_NODE_HOLDS_OBJECT(result)) {
        gtk_label_set_text(GTK_LABEL(status), error != NULL ? error->message : "Veri servisi geçerli kayıt döndürmedi.");
        return;
    }
    JsonObject *object = json_node_get_object(result);
    if (g_str_equal(gtk_widget_get_name(owner), "technical-person-dialog")) {
        const gchar *file = radios_json_string(object, "credential_file", "");
        gchar *message = g_strdup_printf("Kullanıcı ve istasyon izinleri kaydedildi. Erişim anahtarı yalnız kullanıcıya açık dosyada saklandı: %s", file);
        gtk_label_set_text(GTK_LABEL(status), message);
        g_free(message);
        gtk_dialog_set_response_sensitive(GTK_DIALOG(owner), GTK_RESPONSE_ACCEPT, FALSE);
        gtk_widget_show_all(form);
    } else {
        gtk_widget_destroy(owner);
    }
    radios_data_refresh(ctx);
    if (ctx->notify != NULL) ctx->notify(ctx->notify_data, "Veritabanına kaydedildi.");
}

static gboolean
technical_submit(RadioSViewContext *ctx, GtkWidget *dialog, GtkWidget *form)
{
    if (!radios_data_connected(ctx)) return FALSE;
    JsonNode *body = radios_data_body(ctx);
    JsonObject *object = json_node_get_object(body);
    const gchar *key = g_object_get_data(G_OBJECT(dialog), "technical-write-key");
    if (key == NULL) {
        gchar *created = g_uuid_string_random();
        g_object_set_data_full(G_OBJECT(dialog), "technical-write-key", created, g_free);
        key = created;
    }
    json_object_set_string_member(object, "idempotency_key", key);
    const gchar *path;
    if (g_str_equal(gtk_widget_get_name(dialog), "technical-server-dialog")) {
        const gchar *architectures[] = {"local", "cloud", "hybrid"};
        const gchar *roles[] = {"primary", "backup", "management"};
        JsonObject *config = json_object_new();
        GtkWidget *widget = g_object_get_data(G_OBJECT(form), "technical-server-name");
        json_object_set_string_member(object, "kind", "server");
        json_object_set_string_member(object, "name", gtk_entry_get_text(GTK_ENTRY(widget)));
        const gchar *ids[] = {"technical-server-station", "technical-server-address", "technical-server-database"};
        const gchar *fields[] = {"target_name", "address", "database_profile"};
        for (guint i=0;i<G_N_ELEMENTS(ids);++i) {
            widget=g_object_get_data(G_OBJECT(form),ids[i]);
            json_object_set_string_member(config,fields[i],gtk_entry_get_text(GTK_ENTRY(widget)));
        }
        widget=g_object_get_data(G_OBJECT(form),"technical-server-architecture");
        json_object_set_string_member(config,"architecture",architectures[CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(widget)),0,2)]);
        widget=g_object_get_data(G_OBJECT(form),"technical-server-role");
        json_object_set_string_member(config,"role",roles[CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(widget)),0,2)]);
        widget=g_object_get_data(G_OBJECT(form),"technical-server-offline");
        json_object_set_int_member(config,"offline_minutes",gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget)));
        json_object_set_object_member(object,"config",config);
        JsonObject *record=g_object_get_data(G_OBJECT(dialog),"technical-record");
        if (record != NULL) {
            json_object_set_string_member(object,"document_id",radios_json_string(record,"_id",""));
            json_object_set_int_member(object,"expected_revision",radios_json_int(record,"_rev",0));
        }
        path="/v1/workflows/desktop/put";
    } else {
        GtkWidget *widget=g_object_get_data(G_OBJECT(form),"technical-person-name");
        json_object_set_string_member(object,"display_name",gtk_entry_get_text(GTK_ENTRY(widget)));
        widget=g_object_get_data(G_OBJECT(form),"technical-person-login");
        json_object_set_string_member(object,"username",gtk_entry_get_text(GTK_ENTRY(widget)));
        widget=g_object_get_data(G_OBJECT(form),"technical-person-role");
        const gchar *roles[] = {"operator","dj","manager","advert","custom"};
        json_object_set_string_member(object,"role",roles[CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(widget)),0,4)]);
        const gchar *ids[]={"technical-person-custom-role","technical-person-start","technical-person-end"};
        const gchar *names[]={"custom_role","shift_start","shift_end"};
        for (guint i=0;i<G_N_ELEMENTS(ids);++i) {
            widget=g_object_get_data(G_OBJECT(form),ids[i]);
            json_object_set_string_member(object,names[i],gtk_entry_get_text(GTK_ENTRY(widget)));
        }
        widget=g_object_get_data(G_OBJECT(form),"technical-person-period");
        json_object_set_boolean_member(object,"temporary",gtk_combo_box_get_active(GTK_COMBO_BOX(widget))==1);
        JsonArray *permissions=json_array_new();
        const gchar *permission_names[]={"view","operate","operate","edit","repair","manage","bulk_edit","bulk_edit"};
        gboolean used[8]={FALSE};
        for (guint i=0;i<8;++i) {
            gchar *id=g_strdup_printf("technical-person-permission-%u",i);
            widget=g_object_get_data(G_OBJECT(form),id);g_free(id);
            if (widget != NULL && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) {
                gboolean duplicate=FALSE;
                for (guint k=0;k<i;++k) if(used[k]&&g_str_equal(permission_names[k],permission_names[i])) duplicate=TRUE;
                if(!duplicate)json_array_add_string_element(permissions,permission_names[i]);
                used[i]=TRUE;
            }
        }
        json_object_set_array_member(object,"additional_permissions",permissions);
        /* The admin variant never returns the bearer key; the GUI shows
         * only the owner-private key file, as before. */
        path="/v1/workflows/desktop-admin/user-create";
    }
    gboolean queued=radios_data_request(ctx,dialog,"POST",path,body,technical_saved,NULL,NULL);
    json_node_free(body);
    if(queued) {
        gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT,FALSE);
        GtkWidget *status=g_object_get_data(G_OBJECT(form),"technical-status-label");
        gtk_label_set_text(GTK_LABEL(status),"Veritabanına kaydediliyor…");
    }
    return queued;
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
card(const gchar *title, GtkWidget **content, GtkWidget **heading)
{
    GtkWidget *widget = box(GTK_ORIENTATION_VERTICAL, 14, "technical-card");
    *heading = box(GTK_ORIENTATION_HORIZONTAL, 12, "technical-toolbar");
    pack(*heading, radios_ui_label(title, "heading"), TRUE);
    pack(widget, *heading, FALSE);
    *content = box(GTK_ORIENTATION_VERTICAL, 12, NULL);
    pack(widget, *content, FALSE);
    return widget;
}

static GtkWidget *
notice_button(GtkWidget *parent, const gchar *text, RadioSViewContext *ctx,
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
    GtkWidget *row = box(GTK_ORIENTATION_HORIZONTAL, 16, "technical-fact-row");
    gtk_widget_set_margin_top(row, 11);
    gtk_widget_set_margin_bottom(row, 11);
    pack(row, radios_ui_label(name, "technical-fact-name"), TRUE);
    GtkWidget *label = radios_ui_label(value, "technical-fact-value");
    gtk_label_set_xalign(GTK_LABEL(label), 1.0f);
    pack(row, label, TRUE);
    pack(parent, row, FALSE);
    return label;
}

static GtkWidget *
field(GtkWidget *parent, const gchar *label, GtkWidget *widget)
{
    GtkWidget *group = box(GTK_ORIENTATION_VERTICAL, 6, "technical-field");
    pack(group, radios_ui_label(label, "form-label"), FALSE);
    pack(group, widget, FALSE);
    pack(parent, group, FALSE);
    g_object_set_data(G_OBJECT(widget), "technical-field-group", group);
    return widget;
}

static GtkWidget *
named_field(GtkWidget *form, const gchar *title, const gchar *id,
            GtkWidget *widget, gint maximum)
{
    field(form, title, widget);
    gtk_widget_set_name(widget, id);
    if (GTK_IS_ENTRY(widget) && !GTK_IS_SPIN_BUTTON(widget))
        gtk_entry_set_max_length(GTK_ENTRY(widget), maximum);
    g_object_set_data(G_OBJECT(form), id, widget);
    return widget;
}

static GtkWidget *
get_field(GtkWidget *form, const gchar *id)
{
    return g_object_get_data(G_OBJECT(form), id);
}

static const gchar *
field_text(GtkWidget *form, const gchar *id)
{
    GtkWidget *widget = get_field(form, id);
    return widget != NULL ? gtk_entry_get_text(GTK_ENTRY(widget)) : "";
}

static gboolean
valid_url(const gchar *value, gboolean https_only)
{
    GUri *uri = g_uri_parse(value, G_URI_FLAGS_NONE, NULL);
    if (uri == NULL)
        return FALSE;
    const gchar *scheme = g_uri_get_scheme(uri);
    const gchar *host = g_uri_get_host(uri);
    gint port = g_uri_get_port(uri);
    gboolean valid = scheme != NULL && host != NULL && *host != '\0' &&
                     g_uri_get_userinfo(uri) == NULL &&
                     (port == -1 || (port >= 1 && port <= 65535)) &&
                     (g_ascii_strcasecmp(scheme, "https") == 0 ||
                      (!https_only && g_ascii_strcasecmp(scheme, "http") == 0));
    for (const gchar *p = host != NULL ? host : ""; *p != '\0'; ++p)
        if (g_ascii_isspace(*p))
            valid = FALSE;
    g_uri_unref(uri);
    return valid;
}

static gboolean
entry_filled(GtkWidget *form, const gchar *id)
{
    gchar *copy = g_strdup(field_text(form, id));
    gboolean filled = *g_strstrip(copy) != '\0';
    g_free(copy);
    return filled;
}

static gboolean
validation_error(GtkWidget *form, const gchar *message, const gchar *id)
{
    GtkLabel *status = g_object_get_data(G_OBJECT(form), "technical-status-label");
    if (status != NULL)
        gtk_label_set_text(status, message);
    GtkWidget *widget = get_field(form, id);
    if (widget != NULL)
        gtk_widget_grab_focus(widget);
    return FALSE;
}

static gboolean
datetime_valid(const gchar *text)
{
    gint year, month, day, hour, minute, consumed = 0;
    gchar separator;
    return strlen(text) == 16 &&
           sscanf(text, "%4d-%2d-%2d%c%2d:%2d%n", &year, &month, &day,
                   &separator, &hour, &minute, &consumed) == 6 && consumed == 16 &&
           (separator == 'T' || separator == ' ') && year > 0 && year <= 9999 &&
           month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
           g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year) &&
           hour >= 0 && hour < 24 && minute >= 0 && minute < 60;
}

static gboolean
validate_editor(GtkWidget *dialog, GtkWidget *form)
{
    if (g_str_equal(gtk_widget_get_name(dialog), "technical-server-dialog")) {
        if (!entry_filled(form, "technical-server-name"))
            return validation_error(form, "Sunucu adı zorunludur.", "technical-server-name");
        if (!entry_filled(form, "technical-server-station"))
            return validation_error(form, "İstasyon / hedef adı zorunludur.", "technical-server-station");
        if (!valid_url(field_text(form, "technical-server-address"), FALSE))
            return validation_error(form, "Kullanıcı adı/parola içermeyen HTTP veya HTTPS adresi girin.", "technical-server-address");
    } else {
        if (!entry_filled(form, "technical-person-name"))
            return validation_error(form, "Kişinin adı zorunludur.", "technical-person-name");
        if (!entry_filled(form, "technical-person-login"))
            return validation_error(form, "Kullanıcı adı / kimliği zorunludur.", "technical-person-login");
        gint role = gtk_combo_box_get_active(GTK_COMBO_BOX(get_field(form, "technical-person-role")));
        gint scope = gtk_combo_box_get_active(GTK_COMBO_BOX(get_field(form, "technical-person-scope")));
        gint period = gtk_combo_box_get_active(GTK_COMBO_BOX(get_field(form, "technical-person-period")));
        if (role == 4 && !entry_filled(form, "technical-person-custom-role"))
            return validation_error(form, "Özel rol adını doldurun.", "technical-person-custom-role");
        if (scope == 0 && !entry_filled(form, "technical-person-station"))
            return validation_error(form, "Hedef istasyon adını doldurun.", "technical-person-station");
        if (scope != 0 && gtk_combo_box_get_active(GTK_COMBO_BOX(get_field(form, "technical-person-target"))) <= 0)
            return validation_error(form, "Mevcut program veya oynatıcı seçilmedi; veri servisi bağlantısı bekleniyor.", "technical-person-target");
        const gchar *start = field_text(form, "technical-person-start");
        const gchar *end = field_text(form, "technical-person-end");
        if (*start != '\0' && !datetime_valid(start))
            return validation_error(form, "Vardiya başlangıcını YYYY-AA-GG SS:DD biçiminde girin.", "technical-person-start");
        if ((period == 1 || *end != '\0') && !datetime_valid(end))
            return validation_error(form, "Geçici atama için geçerli vardiya bitişi zorunludur.", "technical-person-end");
        if (*start != '\0' && *end != '\0') {
            gchar *normalized_start = g_strdup(start);
            gchar *normalized_end = g_strdup(end);
            normalized_start[10] = normalized_end[10] = 'T';
            gboolean ordered = strcmp(normalized_start, normalized_end) < 0;
            g_free(normalized_start);
            g_free(normalized_end);
            if (!ordered)
                return validation_error(form, "Vardiya bitişi başlangıçtan sonra olmalı.", "technical-person-end");
        }
    }
    return TRUE;
}

static GtkWidget *
number(gdouble minimum, gdouble maximum, gdouble value)
{
    GtkWidget *widget = gtk_spin_button_new_with_range(minimum, maximum, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), value);
    return widget;
}

static void
dialog_response(GtkDialog *dialog, gint response, gpointer data)
{
    RadioSViewContext *ctx = data;
    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }
    GtkWidget *form = g_object_get_data(G_OBJECT(dialog), "technical-form");
    if(radios_data_connected(ctx) && g_str_equal(gtk_widget_get_name(GTK_WIDGET(dialog)),"technical-person-dialog")) {
        JsonObject *context=radios_data_context(ctx);
        JsonObject *station=context != NULL?json_object_get_object_member(context,"station"):NULL;
        if(station != NULL)gtk_entry_set_text(GTK_ENTRY(get_field(form,"technical-person-station")),radios_json_string(station,"name",""));
    }
    if (!validate_editor(GTK_WIDGET(dialog), form))
        return;
    if (technical_submit(ctx, GTK_WIDGET(dialog), form))
        return;
    validation_error(form, "Veri servisi henüz bağlı değil; değişiklik kaydedilmedi. Girdiler korunuyor.", "");
    if (ctx->notify != NULL)
        ctx->notify(ctx->notify_data, "Veri servisi henüz bağlı değil; değişiklik kaydedilmedi.");
}

static GtkWidget *
editor_new(RadioSViewContext *ctx, const gchar *title, const gchar *save,
           GtkWidget **form)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, "Vazgeç", GTK_RESPONSE_CANCEL,
        save, GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 620, 640);
    style(dialog, "technical-dialog");
    style(dialog, "radios");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    *form = box(GTK_ORIENTATION_VERTICAL, 12, "technical-form");
    gtk_container_set_border_width(GTK_CONTAINER(*form), 20);
    gtk_container_add(GTK_CONTAINER(scroll), *form);
    pack(gtk_dialog_get_content_area(GTK_DIALOG(dialog)), scroll, TRUE);
    GtkWidget *status = radios_ui_label("", "technical-status");
    pack(*form, status, FALSE);
    g_object_set_data(G_OBJECT(*form), "technical-status-label", status);
    g_object_set_data(G_OBJECT(dialog), "technical-form", *form);
    g_signal_connect(dialog, "response", G_CALLBACK(dialog_response), ctx);
    return dialog;
}

static void
server_editor(GtkButton *button, gpointer data)
{
    RadioSViewContext *ctx = data;
    GtkWidget *form;
    GtkWidget *dialog = editor_new(ctx, "Sunucu ekle", "Sunucuyu kaydet", &form);
    gtk_widget_set_name(dialog, "technical-server-dialog");
    const gchar *const architectures[] = {"Yerel", "Bulut", "Hibrit", NULL};
    const gchar *const roles[] = {"Ana yayın", "Yedek yayın", "Yönetim", NULL};
    (void)button;
    named_field(form, "Sunucu adı", "technical-server-name", radios_ui_entry(NULL), 100);
    named_field(form, "Çalışma düzeni", "technical-server-architecture", radios_ui_combo(architectures, 0), 0);
    named_field(form, "Sunucunun görevi", "technical-server-role", radios_ui_combo(roles, 0), 0);
    named_field(form, "İstasyon / hedef adı", "technical-server-station", radios_ui_entry(NULL), 100);
    named_field(form, "Sunucu adresi", "technical-server-address", radios_ui_entry("https://sunucu.istasyon.com"), 500);
    named_field(form, "Veritabanı profil adı · isteğe bağlı", "technical-server-database", radios_ui_entry("Ana arşiv"), 100);
    named_field(form, "Planlanan çevrimdışı kapsam · dakika", "technical-server-offline", number(0, 100800, 0), 0);
    if (radios_data_connected(ctx)) {
        JsonObject *context = radios_data_context(ctx);
        JsonObject *station = context != NULL ? json_object_get_object_member(context, "station") : NULL;
        if (station != NULL) gtk_entry_set_text(GTK_ENTRY(get_field(form, "technical-server-station")), radios_json_string(station,"name",""));
    }
    JsonObject *record = g_object_get_data(G_OBJECT(button),"technical-server-record");
    if (record != NULL) {
        g_object_set_data_full(G_OBJECT(dialog),"technical-record",json_object_ref(record),(GDestroyNotify)json_object_unref);
        JsonObject *config = json_object_get_object_member(record,"config");
        gtk_entry_set_text(GTK_ENTRY(get_field(form,"technical-server-name")),radios_json_string(record,"name",""));
        const gchar *ids[]={"technical-server-station","technical-server-address","technical-server-database"};
        const gchar *keys[]={"target_name","address","database_profile"};
        for(guint i=0;i<3;++i)gtk_entry_set_text(GTK_ENTRY(get_field(form,ids[i])),radios_json_string(config,keys[i],""));
        const gchar *architecture=radios_json_string(config,"architecture","local");
        const gchar *role=radios_json_string(config,"role","primary");
        gtk_combo_box_set_active(GTK_COMBO_BOX(get_field(form,"technical-server-architecture")),g_str_equal(architecture,"cloud")?1:g_str_equal(architecture,"hybrid")?2:0);
        gtk_combo_box_set_active(GTK_COMBO_BOX(get_field(form,"technical-server-role")),g_str_equal(role,"backup")?1:g_str_equal(role,"management")?2:0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(get_field(form,"technical-server-offline")),radios_json_int(config,"offline_minutes",0));
    }
    gtk_widget_show_all(dialog);
}

static void
person_fields_changed(GtkComboBox *combo, gpointer data)
{
    GtkWidget *form = data;
    (void)combo;
    gint role = gtk_combo_box_get_active(GTK_COMBO_BOX(get_field(form, "technical-person-role")));
    gint scope = gtk_combo_box_get_active(GTK_COMBO_BOX(get_field(form, "technical-person-scope")));
    gint period = gtk_combo_box_get_active(GTK_COMBO_BOX(get_field(form, "technical-person-period")));
    GtkWidget *custom = get_field(form, "technical-person-custom-role");
    GtkWidget *station = get_field(form, "technical-person-station");
    GtkWidget *target = get_field(form, "technical-person-target");
    GtkWidget *widgets[] = {custom, station, target};
    gboolean visible[] = {role == 4, scope == 0, scope != 0};
    for (guint i = 0; i < G_N_ELEMENTS(widgets); ++i) {
        GtkWidget *group = g_object_get_data(G_OBJECT(widgets[i]), "technical-field-group");
        gtk_widget_set_visible(group, visible[i]);
        gtk_widget_set_sensitive(widgets[i], visible[i]);
    }
    gtk_widget_set_tooltip_text(get_field(form, "technical-person-end"), period == 1 ?
        "Geçici atamada vardiya bitişi zorunludur." : "Kalıcı atamada vardiya bitişi isteğe bağlıdır.");
}

static void
person_editor(GtkButton *button, gpointer data)
{
    RadioSViewContext *ctx = data;
    GtkWidget *form;
    GtkWidget *dialog = editor_new(ctx, "Yayın kişisi ata", "Atamayı kaydet", &form);
    gtk_widget_set_name(dialog, "technical-person-dialog");
    const gchar *const roles[] = {"Yayın operatörü", "Sunucu / DJ", "Teknik yönetici",
                                 "Reklam sorumlusu", "Özel rol", NULL};
    const gchar *const scopes[] = {"İstasyon", "Program", "Oynatıcı", NULL};
    const gchar *const servers[] = {"Atanmadı", NULL};
    const gchar *const periods[] = {"Kalıcı", "Geçici", NULL};
    (void)button;
    named_field(form, "Kişinin adı", "technical-person-name", radios_ui_entry(NULL), 100);
    named_field(form, "Kullanıcı adı / kimliği", "technical-person-login", radios_ui_entry(NULL), 120);
    GtkWidget *role = named_field(form, "Rol", "technical-person-role", radios_ui_combo(roles, 0), 0);
    GtkWidget *custom = named_field(form, "Özel rol adı", "technical-person-custom-role", radios_ui_entry(NULL), 100);
    GtkWidget *scope = named_field(form, "Atama kapsamı", "technical-person-scope", radios_ui_combo(scopes, 0), 0);
    GtkWidget *station = named_field(form, "Hedef istasyon adı", "technical-person-station", radios_ui_entry(NULL), 100);
    const gchar *const targets[] = {"Hedef bulunamadı · veri servisi bekleniyor", NULL};
    GtkWidget *target = named_field(form, "Hedef program / oynatıcı", "technical-person-target", radios_ui_combo(targets, 0), 0);
    named_field(form, "Atanacağı sunucu · isteğe bağlı", "technical-person-server", radios_ui_combo(servers, 0), 0);
    named_field(form, "Vardiya başlangıcı · isteğe bağlı", "technical-person-start", radios_ui_entry("YYYY-AA-GG SS:DD"), 16);
    named_field(form, "Vardiya bitişi", "technical-person-end", radios_ui_entry("YYYY-AA-GG SS:DD"), 16);
    GtkWidget *period = named_field(form, "Atama süresi", "technical-person-period", radios_ui_combo(periods, 0), 0);
    GtkWidget *permissions = box(GTK_ORIENTATION_VERTICAL, 8, "technical-checks");
    pack(permissions, radios_ui_label("Seçilen kapsamda izinler", NULL), FALSE);
    const gchar *names[] = {"İzleme", "Yayın kontrolü", "CUE / ön dinleme",
                           "Program düzenleme", "Hata düzeltme", "Servis yönetimi",
                           "Geçici toplu düzenleme", "Kalıcı toplu düzenleme"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *check = gtk_check_button_new_with_label(names[i]);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), i == 0);
        gchar *id=g_strdup_printf("technical-person-permission-%u",i);
        g_object_set_data(G_OBJECT(form),id,check);gtk_widget_set_name(check,id);g_free(id);
        pack(permissions, check, FALSE);
    }
    pack(form, permissions, FALSE);
    gtk_widget_show_all(dialog);
    GtkWidget *conditional[] = {custom, station, target};
    for (guint i = 0; i < G_N_ELEMENTS(conditional); ++i)
        gtk_widget_set_no_show_all(g_object_get_data(G_OBJECT(conditional[i]), "technical-field-group"), TRUE);
    g_signal_connect(role, "changed", G_CALLBACK(person_fields_changed), form);
    g_signal_connect(scope, "changed", G_CALLBACK(person_fields_changed), form);
    g_signal_connect(period, "changed", G_CALLBACK(person_fields_changed), form);
    person_fields_changed(NULL, form);
    if (ctx->data != NULL) {
        JsonObject *context=radios_data_context(ctx);
        JsonObject *station_record=context != NULL?json_object_get_object_member(context,"station"):NULL;
        if(station_record != NULL)gtk_entry_set_text(GTK_ENTRY(station),radios_json_string(station_record,"name",""));
        gtk_editable_set_editable(GTK_EDITABLE(station),FALSE);
        gtk_widget_set_sensitive(scope,FALSE);
        gtk_widget_set_sensitive(period,FALSE);
        gtk_widget_set_sensitive(get_field(form,"technical-person-start"),FALSE);
        gtk_widget_set_sensitive(get_field(form,"technical-person-end"),FALSE);
        gtk_widget_set_tooltip_text(scope,"İzinler bu istasyon kapsamında uygulanır. Program ve aygıt yetkileri 13. aşamada bağlanacak.");
        gtk_widget_set_tooltip_text(period,"Vardiya ve geçici görev zamanları 13. aşamada uygulanacak. Yeni erişim anahtarı 30 gün geçerlidir.");
    }
}

static GtkWidget *
server_pane(RadioSViewContext *ctx, GtkWidget *page)
{
    GtkWidget *pane = box(GTK_ORIENTATION_VERTICAL, 14, "technical-pane");
    gtk_widget_set_name(pane, "technical-servers-pane");
    GtkWidget *content, *heading;
    GtkWidget *servers = card("Sunucu bilgisayarlar", &content, &heading);
    GtkWidget *add = notice_button(heading, "+ Sunucu ekle", ctx, NULL, TRUE);
    g_signal_connect(add, "clicked", G_CALLBACK(server_editor), ctx);
    GtkWidget *empty = radios_ui_label("Sunucu tanımlanmadı", "technical-empty");
    gtk_label_set_xalign(GTK_LABEL(empty), 0.5f);
    pack(content, empty, FALSE);
    GtkWidget *list=gtk_list_box_new();
    gtk_widget_set_name(list,"technical-server-list");
    g_object_set_data(G_OBJECT(page),"technical-server-list",list);
    g_object_set_data(G_OBJECT(page),"technical-server-empty",empty);
    pack(content,list,FALSE);
    GtkWidget *facts = box(GTK_ORIENTATION_VERTICAL, 0, "technical-facts");
    g_object_set_data(G_OBJECT(page), "admin-server-status", fact(facts, "Yerel veri servisi", "Henüz denetlenmedi"));
    g_object_set_data(G_OBJECT(page), "admin-database-status", fact(facts, "Veritabanı bütünlüğü", "Henüz denetlenmedi"));
    fact(facts, "Kesinti sonrası ses", "Operatör başlatır");
    pack(content, facts, FALSE);
    GtkWidget *actions = box(GTK_ORIENTATION_HORIZONTAL, 7, "technical-actions");
    GtkWidget *test = notice_button(actions, "Bağlantıları test et", ctx, NULL, TRUE);
    g_signal_connect(test, "clicked", G_CALLBACK(radios_admin_ui_refresh), page);
    pack(content, actions, FALSE);
    pack(pane, servers, FALSE);
    g_object_set_data(G_OBJECT(page), "admin-server-content", content);

    GtkWidget *details = gtk_expander_new("Yerel proje ve çalışma yolları");
    GtkWidget *paths = box(GTK_ORIENTATION_VERTICAL, 7, "technical-facts");
    const RadioSConfig *config = ctx->config;
    fact(paths, "Proje", config->project_root);
    fact(paths, "Yapılandırma", config->config_file);
    fact(paths, "Veritabanı dosyası", config->database_file);
    fact(paths, "Veri servisi adresi", config->service_url);
    fact(paths, "Kayıt dizini", config->recording_root);
    fact(paths, "Yedek dizini", config->backup_root);
    GtkWidget *scan_row = box(GTK_ORIENTATION_HORIZONTAL, 8, "technical-actions");
    GtkWidget *scan = notice_button(scan_row, "Yerel dosyaları denetle", ctx, NULL, TRUE);
    GtkWidget *diagnostic = radios_ui_label("—", "technical-status");
    pack(scan_row, diagnostic, TRUE);
    pack(paths, scan_row, FALSE);
    gtk_container_add(GTK_CONTAINER(details), paths);
    pack(pane, details, FALSE);
    g_object_set_data(G_OBJECT(page), "scan-button", scan);
    g_object_set_data(G_OBJECT(page), "diagnostic-label", diagnostic);
    pack(pane, radios_broadcast_runs_view_new(ctx), FALSE); /* R10.1 Son çalıştırmalar; reads only while shown */
    return pane;
}

static GtkWidget *
people_pane(RadioSViewContext *ctx, GtkWidget *page)
{
    GtkWidget *pane = box(GTK_ORIENTATION_VERTICAL, 14, "technical-pane");
    gtk_widget_set_name(pane, "technical-people-pane");
    GtkWidget *content, *heading;
    GtkWidget *people = card("Yayın kişileri ve yetkileri", &content, &heading);
    GtkWidget *add = notice_button(heading, "+ Kişi ata", ctx, NULL, TRUE);
    g_signal_connect(add, "clicked", G_CALLBACK(person_editor), ctx);
    GtkWidget *empty = radios_ui_label("Kişi ataması yok", "technical-empty");
    gtk_label_set_xalign(GTK_LABEL(empty), 0.5f);
    pack(content, empty, FALSE);
    GtkWidget *list=gtk_list_box_new();
    gtk_widget_set_name(list,"technical-user-list");
    g_object_set_data(G_OBJECT(page),"technical-user-list",list);
    g_object_set_data(G_OBJECT(page),"technical-user-empty",empty);
    pack(content,list,FALSE);
    pack(pane, people, FALSE);
    return pane;
}

static GtkWidget *
web_entry(GtkWidget *form, const gchar *title, const gchar *id, const gchar *placeholder)
{
    return named_field(form, title, id, radios_ui_entry(placeholder), 500);
}

static void
web_save(GtkButton *button_widget, gpointer data)
{
    GtkWidget *form = data;
    RadioSViewContext *ctx = g_object_get_data(G_OBJECT(form), "technical-context");
    (void)button_widget;
    if (!radios_external_https_url_valid(field_text(form, "technical-management-url"))) {
        validation_error(form, "Yönetim paneli için kullanıcı bilgisi ve # bölümü içermeyen HTTPS adresi girin.", "technical-management-url");
        return;
    }
    const gchar *listener = field_text(form, "technical-listener-url");
    const gchar *site = field_text(form, "technical-site-url");
    if (*listener != '\0' && !valid_url(listener, FALSE)) {
        validation_error(form, "Dinleyici akışı HTTP veya HTTPS adresi olmalı.", "technical-listener-url");
        return;
    }
    if (*site != '\0' && !valid_url(site, TRUE)) {
        validation_error(form, "İstasyon sitesi HTTPS adresi olmalı.", "technical-site-url");
        return;
    }
    if (!g_regex_match_simple("^/(?!/)[^\\s\\\\]*$", field_text(form, "technical-site-api-path"), 0, 0)) {
        validation_error(form, "Site yolu tek / ile başlamalı; boşluk ve ters eğik çizgi içermemeli.", "technical-site-api-path");
        return;
    }
    const gchar *ids[] = {"technical-site-search", "technical-site-requests", "technical-site-adverts"};
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) {
        if (*site == '\0' && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(get_field(form, ids[i])))) {
            validation_error(form, "Site işlevleri için HTTPS istasyon sitesi adresi zorunludur.", "technical-site-url");
            return;
        }
    }
    RadioSDocument *document=g_object_get_data(G_OBJECT(form),"technical-web-document");
    if(document != NULL && radios_data_connected(ctx)) {
        JsonNode *node=json_node_new(JSON_NODE_OBJECT);JsonObject *config=json_object_new();json_node_take_object(node,config);
        const gchar *entry_ids[]={"technical-management-url","technical-listener-url","technical-site-url","technical-site-api-path"};
        const gchar *keys[]={"management_url","listener_url","site_url","api_path"};
        for(guint i=0;i<4;++i)json_object_set_string_member(config,keys[i],field_text(form,entry_ids[i]));
        json_object_set_int_member(config,"auth_mode",gtk_combo_box_get_active(GTK_COMBO_BOX(get_field(form,"technical-web-auth"))));
        json_object_set_int_member(config,"session_minutes",gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(get_field(form,"technical-web-session"))));
        json_object_set_int_member(config,"default_permission",gtk_combo_box_get_active(GTK_COMBO_BOX(get_field(form,"technical-web-permission"))));
        const gchar *bool_keys[]={"search","requests","adverts"};
        for(guint i=0;i<3;++i)json_object_set_boolean_member(config,bool_keys[i],gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(get_field(form,ids[i]))));
        gboolean accepted=radios_data_document_save(document,node);json_node_free(node);
        validation_error(form,accepted?"Web erişim profili veritabanına kaydediliyor…":"Web profili henüz yüklenmedi; girdiler korunuyor.","");
        return;
    }
    validation_error(form, "Veri servisi bağlı değil; web erişimi kaydedilmedi. Girdiler korunuyor.", "");
    if (ctx->notify != NULL)
        ctx->notify(ctx->notify_data, "Veri servisi bağlı değil; web erişimi kaydedilmedi.");
}

static void
web_loaded(RadioSViewContext *ctx, GtkWidget *form, JsonObject *config, gpointer unused)
{
    (void)ctx;(void)unused;
    g_object_set_data(G_OBJECT(form),"technical-web-applying",GINT_TO_POINTER(1));
    const gchar *ids[]={"technical-management-url","technical-listener-url","technical-site-url","technical-site-api-path"};
    const gchar *keys[]={"management_url","listener_url","site_url","api_path"};
    for(guint i=0;i<4;++i)gtk_entry_set_text(GTK_ENTRY(get_field(form,ids[i])),radios_json_string(config,keys[i],""));
    gtk_combo_box_set_active(GTK_COMBO_BOX(get_field(form,"technical-web-auth")),(gint)radios_json_int(config,"auth_mode",0));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(get_field(form,"technical-web-session")),radios_json_int(config,"session_minutes",30));
    gtk_combo_box_set_active(GTK_COMBO_BOX(get_field(form,"technical-web-permission")),(gint)radios_json_int(config,"default_permission",0));
    const gchar *check_ids[]={"technical-site-search","technical-site-requests","technical-site-adverts"};
    const gchar *check_keys[]={"search","requests","adverts"};
    for(guint i=0;i<3;++i)gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(get_field(form,check_ids[i])),json_object_get_boolean_member(config,check_keys[i]));
    g_object_set_data(G_OBJECT(form),"technical-web-applying",NULL);
    validation_error(form,"Kaydedilen web erişim profili yüklendi. Dış web servisi bağlantısı henüz doğrulanmadı.","");
    radios_external_access_ui_refresh(form);
}

static void
web_changed(GtkWidget *widget,gpointer data)
{
    GtkWidget *form=data;(void)widget;
    if(!g_object_get_data(G_OBJECT(form),"technical-web-applying")) {
        radios_data_document_dirty(g_object_get_data(G_OBJECT(form),"technical-web-document"),TRUE);
        radios_external_access_ui_refresh(form);
    }
}

static GtkWidget *
web_pane(RadioSViewContext *ctx)
{
    GtkWidget *pane = box(GTK_ORIENTATION_HORIZONTAL, 18, "technical-grid");
    style(pane, "technical-web-grid");
    gtk_box_set_homogeneous(GTK_BOX(pane), TRUE);
    GtkWidget *content, *heading;
    GtkWidget *settings = card("Web üzerinden erişim", &content, &heading);
    style(content, "technical-form");
    g_object_set_data(G_OBJECT(content), "technical-context", ctx);
    GtkWidget *web_form = content;
    web_entry(content, "Yönetim paneli adresi", "technical-management-url", "https://panel.istasyon.com");
    web_entry(content, "Dinleyici akışı adresi · isteğe bağlı", "technical-listener-url", "https://yayin.istasyon.com/live");
    web_entry(content, "İstasyon sitesi · isteğe bağlı", "technical-site-url", "https://istasyon.com");
    GtkWidget *api_path = radios_ui_entry("/api/radios");
    gtk_entry_set_text(GTK_ENTRY(api_path), "/api/radios");
    named_field(content, "Site bağlantı yolu", "technical-site-api-path", api_path, 200);
    const gchar *const auth[] = {"Parola", "Parola + ikinci doğrulama", "Kurum kimliği", NULL};
    const gchar *const permissions[] = {"Yalnız izleme", "Yayın kontrolü", "Seçili kapsam ve yetkiler", NULL};
    named_field(content, "Kimlik doğrulama", "technical-web-auth", radios_ui_combo(auth, 0), 0);
    named_field(content, "Oturum süresi · dakika", "technical-web-session", number(1, 1440, 30), 0);
    named_field(content, "Varsayılan erişim", "technical-web-permission", radios_ui_combo(permissions, 0), 0);
    GtkWidget *checks = box(GTK_ORIENTATION_VERTICAL, 8, "technical-checks");
    pack(checks, radios_ui_label("İstasyon sitesi bağlantıları", NULL), FALSE);
    const gchar *check_names[] = {"Parça / albüm araması", "İstek / iletişim formu", "Şifreli reklam teslimi"};
    const gchar *ids[] = {"technical-site-search", "technical-site-requests", "technical-site-adverts"};
    for (guint i = 0; i < G_N_ELEMENTS(check_names); ++i) {
        GtkWidget *check = gtk_check_button_new_with_label(check_names[i]);
        gtk_widget_set_name(check, ids[i]);
        g_object_set_data(G_OBJECT(content), ids[i], check);
        pack(checks, check, FALSE);
    }
    pack(content, checks, FALSE);
    GtkWidget *actions = box(GTK_ORIENTATION_HORIZONTAL, 7, "technical-actions");
    GtkWidget *save = notice_button(actions, "Web erişimini kaydet", ctx, NULL, TRUE);
    gtk_widget_set_name(save, "technical-web-save");
    g_signal_connect(save, "clicked", G_CALLBACK(web_save), web_form);
    style(save, "technical-primary");
    pack(content, actions, FALSE);
    GtkWidget *web_status = radios_ui_label("", "technical-status");
    pack(content, web_status, FALSE);
    g_object_set_data(G_OBJECT(web_form), "technical-status-label", web_status);
    if(ctx->data != NULL) {
        RadioSDocument *document=radios_data_document_new(ctx,web_form,"web","Web erişimi",web_loaded,NULL);
        g_object_set_data(G_OBJECT(web_form),"technical-web-document",document);
        const gchar *entry_ids[]={"technical-management-url","technical-listener-url","technical-site-url","technical-site-api-path"};
        const gchar *combo_ids[]={"technical-web-auth","technical-web-permission"};
        const gchar *check_ids[]={"technical-site-search","technical-site-requests","technical-site-adverts"};
        for(guint i=0;i<4;++i)g_signal_connect(get_field(web_form,entry_ids[i]),"changed",G_CALLBACK(web_changed),web_form);
        for(guint i=0;i<2;++i)g_signal_connect(get_field(web_form,combo_ids[i]),"changed",G_CALLBACK(web_changed),web_form);
        for(guint i=0;i<3;++i)g_signal_connect(get_field(web_form,check_ids[i]),"toggled",G_CALLBACK(web_changed),web_form);
        g_signal_connect(get_field(web_form,"technical-web-session"),"value-changed",G_CALLBACK(web_changed),web_form);
    }
    pack(pane, settings, TRUE);
    GtkWidget *preview = card("Erişim özeti", &content, &heading);
    style(preview, "technical-preview");
    radios_external_access_ui_bind(ctx, web_form, content);
    gtk_widget_set_valign(preview, GTK_ALIGN_START);
    pack(pane, preview, TRUE);
    return pane;
}

static GtkWidget *
device_card(const gchar *title)
{
    GtkWidget *widget = box(GTK_ORIENTATION_VERTICAL, 10, "task-card");
    GtkWidget *heading = box(GTK_ORIENTATION_HORIZONTAL, 0, "task-card-heading");
    pack(heading, radios_ui_label(title, "heading"), FALSE);
    pack(widget, heading, FALSE);
    return widget;
}

static void
device_reset(GtkButton *button_widget, gpointer data)
{
    GtkWidget *grid = data;
    RadioSViewContext *ctx = g_object_get_data(G_OBJECT(grid), "technical-context");
    const gchar *names[] = {"task-main-output", "task-cue-output", "task-return-output",
                            "task-role", "task-default-profile"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i)
        gtk_combo_box_set_active(GTK_COMBO_BOX(get_field(grid, names[i])), 0);
    /* The MIDI map selector shows the saved active map; resetting the form
     * must not switch the controller off. */
    radios_midi_ui_technical_refresh(ctx);
    gtk_combo_box_set_active(GTK_COMBO_BOX(get_field(grid, "task-text-size")), 1);
    gtk_entry_set_text(GTK_ENTRY(get_field(grid, "task-api-endpoint")), ctx->config->service_url);
    gtk_entry_set_text(GTK_ENTRY(get_field(grid, "task-api-secret")), "");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(get_field(grid, "task-request-backup")), FALSE);
    (void)button_widget;
    if (ctx->notify != NULL)
        ctx->notify(ctx->notify_data, "Form alanları güvenli başlangıç değerlerine döndürüldü; kayıt yapılmadı.");
}

static void
device_register(GtkWidget *grid, GtkWidget *parent, const gchar *title,
                const gchar *id, GtkWidget *widget)
{
    named_field(parent, title, id, widget, 500);
    g_object_set_data(G_OBJECT(grid), id, widget);
}

static GtkWidget *
devices_pane(RadioSViewContext *ctx)
{
    GtkWidget *grid = gtk_grid_new();
    g_object_set_data(G_OBJECT(grid), "technical-context", ctx);
    gtk_widget_set_name(grid, "technical-devices-pane");
    style(grid, "task-technical-grid");
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    const gchar *const unassigned[] = {"Atanmadı", NULL};
    GtkWidget *audio = device_card("Ses çıkışları");
    device_register(grid, audio, "Yayın çıkışı", "task-main-output", radios_ui_combo(unassigned, 0));
    device_register(grid, audio, "CUE / Kulaklık çıkışı", "task-cue-output", radios_ui_combo(unassigned, 0));
    device_register(grid, audio, "Monitor çıkışı", "task-monitor-output", radios_ui_combo(unassigned, 0));
    device_register(grid, audio, "Mikrofon girişi", "task-microphone-input", radios_ui_combo(unassigned, 0));
    GtkWidget *microphone = gtk_toggle_button_new_with_label("Mikrofon kapalı"); gtk_widget_set_name(microphone, "task-microphone-gate");
    g_object_set_data(G_OBJECT(grid), "task-microphone-gate", microphone); pack(audio, microphone, FALSE);
    GtkWidget *gain = radios_knob_new("Mikrofon", -60, 30, 0.5, 0); gtk_widget_set_name(gain, "task-microphone-gain");
    g_object_set_data(G_OBJECT(grid), "task-microphone-gain", gain); pack(audio, gain, FALSE);
    device_register(grid, audio, "Uzak konuk geri dönüşü", "task-return-output", radios_ui_combo(unassigned, 0));
    gtk_widget_set_sensitive(get_field(grid, "task-return-output"), FALSE);
    GtkWidget *audio_actions = box(GTK_ORIENTATION_HORIZONTAL, 7, "task-actions");
    GtkWidget *refresh_audio = radios_ui_button("Aygıtları yenile"), *apply_audio = radios_ui_button("Ses ayarlarını uygula / kaydet");
    gtk_widget_set_name(refresh_audio, "task-audio-refresh"); gtk_widget_set_name(apply_audio, "task-audio-apply");
    g_object_set_data(G_OBJECT(grid), "task-audio-refresh", refresh_audio); g_object_set_data(G_OBJECT(grid), "task-audio-apply", apply_audio);
    pack(audio_actions, refresh_audio, FALSE); pack(audio_actions, apply_audio, FALSE); pack(audio, audio_actions, FALSE);
    GtkWidget *audio_status = radios_ui_label("Ses motoru bağlantısı bekleniyor.", "technical-status"); gtk_widget_set_name(audio_status, "task-audio-status");
    g_object_set_data(G_OBJECT(grid), "task-audio-status", audio_status); pack(audio, audio_status, FALSE);
    GtkWidget *test = notice_button(audio, "Çıkışları test et", ctx, NULL, FALSE);
    gtk_widget_set_tooltip_text(test, "Çıkış testi için gerçek ses motoru ve aygıt ataması gerekir.");
    gtk_grid_attach(GTK_GRID(grid), audio, 0, 0, 1, 1);
    GtkWidget *midi = device_card("MIDI ve erişim");
    const gchar *const mappings[] = {"Denetleyici bağlı değil", "Klavye eşlemesi", NULL};
    const gchar *const roles[] = {"Yayın operatörü", "Program planlayıcısı", "Teknik yönetici", NULL};
    device_register(grid, midi, "Denetleyici eşlemesi", "task-midi-profile", radios_ui_combo(mappings, 0));
    device_register(grid, midi, "Çalışma yetkisi", "task-role", radios_ui_combo(roles, 0));
    test = notice_button(midi, "Denetleyiciyi doğrula", ctx, NULL, FALSE);
    gtk_widget_set_name(test, "task-midi-verify");
    gtk_widget_set_tooltip_text(test, "Denetleyici doğrulaması için gerçek MIDI aygıt bağlantısı gerekir.");
    radios_midi_ui_bind_technical(ctx, get_field(grid, "task-midi-profile"), test);
    gtk_grid_attach(GTK_GRID(grid), midi, 1, 0, 1, 1);
    GtkWidget *network = device_card("Ağ ve API");
    GtkWidget *endpoint = radios_ui_entry("https://...");
    gtk_entry_set_text(GTK_ENTRY(endpoint), ctx->config->service_url);
    device_register(grid, network, "Servis adresi", "task-api-endpoint", endpoint);
    GtkWidget *secret = radios_ui_entry(NULL);
    gtk_entry_set_visibility(GTK_ENTRY(secret), FALSE);
    device_register(grid, network, "API anahtarı", "task-api-secret", secret);
    GtkWidget *backup = gtk_check_button_new_with_label("Yedek bağlantı kontrolü iste");
    gtk_widget_set_name(backup, "task-request-backup");
    g_object_set_data(G_OBJECT(grid), "task-request-backup", backup);
    pack(network, backup, FALSE);
    GtkWidget *actions = box(GTK_ORIENTATION_HORIZONTAL, 7, "task-actions");
    notice_button(actions, "Bağlantıyı test et", ctx, NULL, FALSE);
    notice_button(actions, "Geri dönüş testi", ctx, NULL, FALSE);
    pack(network, actions, FALSE);
    gtk_grid_attach(GTK_GRID(grid), network, 0, 1, 1, 1);
    GtkWidget *appearance = device_card("Görünüm tercihleri");
    const gchar *const sizes[] = {"Kompakt", "Standart", "Büyük", NULL};
    const gchar *const profiles[] = {"Son kullanılan profil", "Yayın", "Playlist", "Otomasyon",
                                    "DJ", "Acil", "Reklam", "Teknik yönetim", "Ses yönlendirme", NULL};
    device_register(grid, appearance, "Çalışma alanı yazı boyutu", "task-text-size", radios_ui_combo(sizes, 1));
    device_register(grid, appearance, "Başlangıç çalışma profili", "task-default-profile", radios_ui_combo(profiles, 0));
    actions = box(GTK_ORIENTATION_HORIZONTAL, 7, "task-actions");
    GtkWidget *save = notice_button(actions, "Tercihleri kaydet", ctx, NULL, TRUE);
    gtk_widget_set_name(save, "task-preferences-save");
    g_object_set_data(G_OBJECT(grid), "task-preferences-save", save);
    style(save, "technical-primary");
    GtkWidget *reset = notice_button(actions, "Güvenli başlangıca dön", ctx, NULL, TRUE);
    gtk_widget_set_name(reset, "task-preferences-reset");
    g_object_set_data(G_OBJECT(grid), "task-preferences-reset", reset);
    g_signal_connect(reset, "clicked", G_CALLBACK(device_reset), grid);
    pack(appearance, actions, FALSE);
    gtk_grid_attach(GTK_GRID(grid), appearance, 1, 1, 1, 1);
    radios_audio_ui_bind_settings(ctx, grid);
    return grid;
}

static void
clear_list(GtkWidget *list)
{
    GList *children=gtk_container_get_children(GTK_CONTAINER(list));
    for(GList *item=children;item != NULL;item=item->next)gtk_widget_destroy(item->data);
    g_list_free(children);
}

static void
server_deleted(RadioSViewContext *ctx,GtkWidget *dialog,JsonNode *result,const GError *error,gpointer unused)
{
    (void)unused;(void)result;
    if(error != NULL) {
        GtkWidget *label=g_object_get_data(G_OBJECT(dialog),"technical-delete-status");
        gtk_label_set_text(GTK_LABEL(label),error->message);
        gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog),GTK_RESPONSE_ACCEPT,TRUE);
        return;
    }
    gtk_widget_destroy(dialog);radios_data_refresh(ctx);
}

static void
server_delete_response(GtkDialog *dialog,gint response,gpointer data)
{
    RadioSViewContext *ctx=data;
    if(response != GTK_RESPONSE_ACCEPT){gtk_widget_destroy(GTK_WIDGET(dialog));return;}
    JsonObject *record=g_object_get_data(G_OBJECT(dialog),"technical-record");
    JsonNode *body=radios_data_body(ctx);JsonObject *object=json_node_get_object(body);
    json_object_set_string_member(object,"kind","server");
    json_object_set_string_member(object,"document_id",radios_json_string(record,"_id",""));
    json_object_set_int_member(object,"expected_revision",radios_json_int(record,"_rev",0));
    gchar *key=g_uuid_string_random();json_object_set_string_member(object,"idempotency_key",key);g_free(key);
    if(radios_data_request(ctx,GTK_WIDGET(dialog),"POST","/v1/workflows/desktop/delete",body,server_deleted,NULL,NULL))
        gtk_dialog_set_response_sensitive(dialog,GTK_RESPONSE_ACCEPT,FALSE);
    json_node_free(body);
}

static void
server_delete(GtkButton *button,gpointer data)
{
    RadioSViewContext *ctx=data;JsonObject *record=g_object_get_data(G_OBJECT(button),"technical-server-record");
    GtkWidget *dialog=gtk_dialog_new_with_buttons("Sunucu profilini kaldır",ctx->parent,GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,"Vazgeç",GTK_RESPONSE_CANCEL,"Kaldır",GTK_RESPONSE_ACCEPT,NULL);
    GtkWidget *label=radios_ui_label("","technical-status");
    gtk_label_set_line_wrap(GTK_LABEL(label),TRUE);pack(gtk_dialog_get_content_area(GTK_DIALOG(dialog)),label,FALSE);
    g_object_set_data(G_OBJECT(dialog),"technical-delete-status",label);
    g_object_set_data_full(G_OBJECT(dialog),"technical-record",json_object_ref(record),(GDestroyNotify)json_object_unref);
    g_signal_connect(dialog,"response",G_CALLBACK(server_delete_response),ctx);gtk_widget_show_all(dialog);
}

static void
servers_loaded(RadioSViewContext *ctx,GtkWidget *page,JsonNode *result,const GError *error,gpointer unused)
{
    (void)unused;
    GtkWidget *empty=g_object_get_data(G_OBJECT(page),"technical-server-empty");
    if(error != NULL || result == NULL || !JSON_NODE_HOLDS_OBJECT(result)){
        gtk_label_set_text(GTK_LABEL(empty),error != NULL?error->message:"Sunucu profilleri okunamadı.");return;
    }
    JsonObject *object=json_node_get_object(result);JsonArray *records=json_object_get_array_member(object,"records");
    GtkWidget *list=g_object_get_data(G_OBJECT(page),"technical-server-list");clear_list(list);
    for(guint i=0;records != NULL && i<json_array_get_length(records);++i){
        JsonObject *record=json_array_get_object_element(records,i);JsonObject *config=json_object_get_object_member(record,"config");
        GtkWidget *row=box(GTK_ORIENTATION_HORIZONTAL,10,"technical-server-row");GtkWidget *text=box(GTK_ORIENTATION_VERTICAL,4,NULL);
        pack(text,radios_ui_label(radios_json_string(record,"name",""),"heading"),FALSE);
        pack(text,radios_ui_label(radios_json_string(config,"address",""),"technical-status"),FALSE);
        pack(row,text,TRUE);
        GtkWidget *edit=radios_ui_button("Düzenle");GtkWidget *remove=radios_ui_button("Kaldır");
        g_object_set_data_full(G_OBJECT(edit),"technical-server-record",json_object_ref(record),(GDestroyNotify)json_object_unref);
        g_object_set_data_full(G_OBJECT(remove),"technical-server-record",json_object_ref(record),(GDestroyNotify)json_object_unref);
        g_signal_connect(edit,"clicked",G_CALLBACK(server_editor),ctx);g_signal_connect(remove,"clicked",G_CALLBACK(server_delete),ctx);
        pack(row,edit,FALSE);pack(row,remove,FALSE);gtk_container_add(GTK_CONTAINER(list),row);
    }
    gtk_widget_set_visible(empty,records == NULL || json_array_get_length(records)==0);gtk_widget_show_all(list);
}

static void
people_loaded(RadioSViewContext *ctx,GtkWidget *page,JsonNode *result,const GError *error,gpointer unused)
{
    (void)unused;
    GtkWidget *empty=g_object_get_data(G_OBJECT(page),"technical-user-empty");
    if(error != NULL || result == NULL || !JSON_NODE_HOLDS_OBJECT(result)){
        gtk_label_set_text(GTK_LABEL(empty),error != NULL?error->message:"Kullanıcı izinleri okunamadı.");return;
    }
    radios_admin_ui_people(ctx,page,json_node_get_object(result));
}

/* Formal programme approval of the Broadcast page's selected queue. The panel
 * itself is the Broadcast page's lifecycle component; this tab only hosts it. */
static gboolean
programme_target_tick(gpointer data)
{
    GtkWidget *label = data;
    RadioSViewContext *ctx = g_object_get_data(G_OBJECT(label), "technical-context");
    gchar *target = radios_broadcast_programme_label(ctx);
    gchar *text = g_strdup_printf("Onaya gönderilecek program: %s", target ? target : "Yayın sayfasında yayın sırası yok");
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), text) != 0) gtk_label_set_text(GTK_LABEL(label), text);
    g_free(text); g_free(target);
    return G_SOURCE_CONTINUE;
}

static void
programme_target_destroyed(GtkWidget *label, gpointer data)
{
    (void)data;
    guint timer = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(label), "technical-programme-timer"));
    if (timer) g_source_remove(timer);
    g_object_set_data(G_OBJECT(label), "technical-programme-timer", NULL);
}

static void
media_rights_clicked(GtkButton *button, gpointer data)
{
    RadioSViewContext *ctx = data;
    if (!radios_data_connected(ctx)) {
        if (ctx->notify != NULL) ctx->notify(ctx->notify_data, "Kullanım hakları için veri servisine bağlanın.");
        return;
    }
    radios_media_rights_open(ctx, GTK_WIDGET(button));
}

static GtkWidget *
programme_pane(RadioSViewContext *ctx)
{
    GtkWidget *pane = box(GTK_ORIENTATION_VERTICAL, 14, "technical-pane");
    gtk_widget_set_name(pane, "technical-programme-pane");
    GtkWidget *content, *heading;
    GtkWidget *approval = card("Program onayı ve dağıtım", &content, &heading);
    GtkWidget *rights = notice_button(heading, "Yayın ve medya kullanım hakları…", ctx, NULL, ctx->data != NULL);
    gtk_widget_set_name(rights, "technical-media-rights");
    gtk_widget_set_tooltip_text(rights, "Doğrulanmış kütüphane medyası için bölge, çıkış ve zaman aralığına göre izin / yasak kararı kaydeder.");
    g_signal_connect(rights, "clicked", G_CALLBACK(media_rights_clicked), ctx);
    GtkWidget *target = radios_ui_label("Onaya gönderilecek program: —", "technical-status");
    gtk_widget_set_name(target, "technical-programme-target");
    gtk_label_set_line_wrap(GTK_LABEL(target), TRUE);
    gtk_label_set_xalign(GTK_LABEL(target), 0.0f);
    pack(content, target, FALSE);
    GtkWidget *panel = ctx->data != NULL ? radios_broadcast_programme_lifecycle_new(ctx) : NULL;
    if (panel != NULL) {
        pack(content, panel, FALSE);
        g_object_set_data(G_OBJECT(target), "technical-context", ctx);
        programme_target_tick(target);
        g_object_set_data(G_OBJECT(target), "technical-programme-timer", GUINT_TO_POINTER(g_timeout_add_seconds(1, programme_target_tick, target)));
        g_signal_connect(target, "destroy", G_CALLBACK(programme_target_destroyed), NULL);
    } else {
        gtk_label_set_text(GTK_LABEL(target), ctx->data != NULL ? "Yayın sayfası bu pencerede yok; program onayı kullanılamıyor." :
            "Program onayı için veri servisi gerekir.");
    }
    pack(pane, approval, FALSE);
    return pane;
}

static void
technical_refresh(RadioSViewContext *ctx,GtkWidget *page)
{
    if(!radios_data_connected(ctx))return;
    JsonNode *body=radios_data_body(ctx);JsonObject *object=json_node_get_object(body);json_object_set_string_member(object,"kind","server");
    radios_data_request(ctx,page,"POST","/v1/workflows/desktop/list",body,servers_loaded,NULL,NULL);json_node_free(body);
    body=radios_data_body(ctx);radios_data_request(ctx,page,"POST","/v1/workflows/desktop/context",body,people_loaded,NULL,NULL);json_node_free(body);
}

GtkWidget *
radios_technical_new(RadioSViewContext *ctx)
{
    GtkWidget *page = box(GTK_ORIENTATION_VERTICAL, 20, "technical-management");
    gtk_widget_set_name(page, "technical-task-view");
    GtkWidget *stack = gtk_stack_new();
    gtk_widget_set_name(stack, "technical-settings-stack");
    gtk_stack_set_homogeneous(GTK_STACK(stack), FALSE);
    GtkWidget *tabs = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(tabs), GTK_STACK(stack));
    style(tabs, "technical-tabs");
    gtk_widget_set_halign(tabs, GTK_ALIGN_START);
    pack(page, tabs, FALSE);
    pack(page, stack, FALSE);
    gtk_stack_add_titled(GTK_STACK(stack), server_pane(ctx, page), "servers", "Sunucular");
    gtk_stack_add_titled(GTK_STACK(stack), people_pane(ctx,page), "people", "Yayın kişileri");
    gtk_stack_add_titled(GTK_STACK(stack), web_pane(ctx), "web", "Web erişimi");
    GtkWidget *devices = devices_pane(ctx);
    gtk_stack_add_titled(GTK_STACK(stack), devices, "devices", "Aygıtlar ve görünüm");
    gtk_stack_add_titled(GTK_STACK(stack), programme_pane(ctx), "programme", "Program onayı");
    gtk_stack_add_titled(GTK_STACK(stack), radios_workflow_hub_new(ctx),
                         "workflows", "İstasyon işlemleri");
    const gchar *hooks[] = {"task-preferences-save", "task-preferences-reset",
                            "task-text-size", "task-default-profile"};
    for (guint i = 0; i < G_N_ELEMENTS(hooks); ++i)
        g_object_set_data(G_OBJECT(page), hooks[i], get_field(devices, hooks[i]));
    GtkWidget *content, *heading;
    GtkWidget *audit = card("İşlem kaydı", &content, &heading);
    style(audit, "technical-audit");
    radios_admin_ui_attach(ctx,page,g_object_get_data(G_OBJECT(page),"admin-server-content"),content);
    pack(page, audit, FALSE);
    if(ctx->data != NULL)radios_data_register_refresh(ctx,page,technical_refresh);
    return page;
}
