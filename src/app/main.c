#include "radios/config.h"
#include "radios/runtime.h"
#include "radios/window.h"
#include "radios/ui_preferences.h"
#include <glib-unix.h>
#include <locale.h>
#include <signal.h>

#ifndef RADIOS_VERSION
#define RADIOS_VERSION "development"
#endif

typedef struct {
    RadioSConfig config;
    RadioSRuntime *runtime;
    RadioSWindow *window;
    gboolean smoke_test;
    gboolean smoke_passed;
    gboolean startup_failed;
    const gchar *capture_path;
    const gchar *profile_override;
    guint termination_source, interrupt_source;
} RadioSApplication;

static gboolean
termination_requested(gpointer data)
{
    /* Run normal window/audio/data teardown on the GTK thread, including WAV
     * finalization and durable pending-write preservation. */
    g_application_quit(G_APPLICATION(data));
    return G_SOURCE_CONTINUE;
}

static void
quit_action(GSimpleAction *action, GVariant *parameter, gpointer data)
{
    (void)action;
    (void)parameter;
    g_application_quit(G_APPLICATION(data));
}

static void
startup(GApplication *application, gpointer data)
{
    RadioSApplication *app = data;
    GSimpleAction *quit = g_simple_action_new("quit", NULL);
    const gchar *accelerators[] = { "<Primary>q", NULL };
    /* Initial profile documents and audio preferences share this bounded queue. */
    app->runtime = radios_runtime_new(64, 128);
    g_signal_connect(quit, "activate", G_CALLBACK(quit_action), application);
    g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(quit));
    g_object_unref(quit);
    gtk_application_set_accels_for_action(GTK_APPLICATION(application),
                                          "app.quit", accelerators);
    app->termination_source = g_unix_signal_add(SIGTERM, termination_requested, application);
    app->interrupt_source = g_unix_signal_add(SIGINT, termination_requested, application);
}

static void
activate(GApplication *application, gpointer data)
{
    RadioSApplication *app = data;
    if (app->window == NULL) {
        GError *error = NULL;
        app->window = radios_window_new(GTK_APPLICATION(application), &app->config,
                                       app->runtime, app->smoke_test, app->capture_path, &error);
        if (app->window == NULL) {
            g_printerr("RadioS: %s\n", error != NULL ? error->message : "Pencere oluşturulamadı.");
            g_clear_error(&error);
            app->startup_failed = TRUE;
            g_application_quit(application);
            return;
        }
    }
    if (app->profile_override)
        radios_window_select_profile(app->window, app->profile_override);
    radios_window_present(app->window);
}

static void
shutdown_application(GApplication *application, gpointer data)
{
    RadioSApplication *app = data;
    (void)application;
    if (app->termination_source) g_source_remove(app->termination_source);
    if (app->interrupt_source) g_source_remove(app->interrupt_source);
    app->termination_source = app->interrupt_source = 0;
    app->smoke_passed = radios_window_smoke_passed(app->window);
    radios_window_free(app->window);
    app->window = NULL;
    radios_runtime_shutdown(app->runtime);
    /* Deliver cancellation completions while application/config storage still exists. */
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
}

int
main(int argc, char **argv)
{
    gchar *root_option = NULL, *config_option = NULL, *capture_option = NULL, *profile_option = NULL;
    gboolean version = FALSE, check_config = FALSE, smoke_test = FALSE;
    GError *error = NULL;
    GOptionEntry options[] = {
        { "project-root", 0, 0, G_OPTION_ARG_FILENAME, &root_option, "RadioS proje kökü", "DİZİN" },
        { "config", 0, 0, G_OPTION_ARG_FILENAME, &config_option, "Proje içindeki INI dosyası", "DOSYA" },
        { "profile", 0, 0, G_OPTION_ARG_STRING, &profile_option, "Bu açılışın çalışma sekmesi (routing: Ses yönlendirme)", "PROFİL" },
        { "check-config", 0, 0, G_OPTION_ARG_NONE, &check_config, "Pencere açmadan ayarları doğrula", NULL },
        { "smoke-test", 0, 0, G_OPTION_ARG_NONE, &smoke_test, "Yerel pencere kabulünü çalıştır ve kapat", NULL },
        { "capture", 0, 0, G_OPTION_ARG_FILENAME, &capture_option, "Kabul testinde kendi pencere görüntüsünü kaydet", "PNG" },
        { "version", 0, 0, G_OPTION_ARG_NONE, &version, "Sürümü yazdır", NULL },
        { NULL, 0, 0, 0, NULL, NULL, NULL }
    };
    RadioSApplication app = {0};
    int result = 1;
    GtkApplication *application = NULL;
    gchar *executable = NULL, *root = NULL;
    GOptionContext *context;
    setlocale(LC_ALL, "");
    context = g_option_context_new("— RadioS yerel masaüstü uygulaması");
    g_option_context_add_main_entries(context, options, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error))
        goto error;
    if (argc != 1 || (capture_option != NULL && !smoke_test) ||
        (check_config && smoke_test)) {
        g_set_error_literal(&error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                            "Geçersiz seçenek birleşimi; --help ile kullanımı görüntüleyin.");
        goto error;
    }
    if (version) {
        g_print("RadioS %s — C / GTK 3\n", RADIOS_VERSION);
        result = 0;
        goto done;
    }
    if (profile_option && !radios_config_profile_is_valid(profile_option)) {
        g_set_error_literal(&error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Geçersiz çalışma profili.");
        goto error;
    }
    executable = g_file_read_link("/proc/self/exe", NULL);
    if (executable == NULL)
        executable = g_find_program_in_path(argv[0]);
    root = radios_project_root_find(root_option, executable, &error);
    if (root == NULL || !radios_config_load(root, config_option, &app.config, &error))
        goto error;
    if (profile_option) {
        g_free(app.config.initial_profile);
        app.config.initial_profile = g_strdup(profile_option);
    }
    if (check_config) {
        RadioSUIPreferences preferences;
        if (!radios_ui_preferences_load(root, &preferences, &error))
            goto error;
        g_print("Yapılandırma geçerli.\nProje: %s\nAyar: %s\nProfil: %s\nVeritabanı yolu: %s\n",
                app.config.project_root, app.config.config_file,
                app.config.initial_profile, app.config.database_file);
        result = 0;
        goto done;
    }
    if (!gtk_init_check(NULL, NULL)) {
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Yerel ekran açılamadı. Wayland/X11 masaüstü oturumu gerekli.");
        goto error;
    }
    app.smoke_test = smoke_test;
    app.capture_path = capture_option;
    app.profile_override = profile_option;
    application = gtk_application_new("org.radios.desktop", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(application, "startup", G_CALLBACK(startup), &app);
    g_signal_connect(application, "activate", G_CALLBACK(activate), &app);
    g_signal_connect(application, "shutdown", G_CALLBACK(shutdown_application), &app);
    result = g_application_run(G_APPLICATION(application), 1, argv);
    if (app.startup_failed || (smoke_test && !app.smoke_passed))
        result = 1;
    goto done;

error:
    g_printerr("RadioS: %s\n", error != NULL ? error->message : "Başlatma başarısız.");
done:
    g_clear_error(&error);
    g_clear_object(&application);
    radios_runtime_free(app.runtime);
    radios_config_clear(&app.config);
    g_option_context_free(context);
    g_free(root_option);
    g_free(config_option);
    g_free(capture_option);
    g_free(profile_option);
    g_free(executable);
    g_free(root);
    return result;
}
