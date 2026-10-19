#define main radios_existing_dj_test_main
#include "test_dj_ui.c"
#undef main

static void assert_effect_choices(GtkWidget *root, const gchar *name)
{
    GtkWidget *widget = find(root, name); g_assert_true(GTK_IS_COMBO_BOX(widget));
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
    g_assert_cmpint(gtk_tree_model_iter_n_children(model, NULL), ==, 5);
    GtkTreeIter iter; gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    const gchar *expected[] = {"Yok", "Effector 1", "Effector 2", "Effector 3", "Effector 4"};
    for (guint i = 0; i < G_N_ELEMENTS(expected); ++i) {
        g_assert_true(valid); gchar *text = NULL;
        gtk_tree_model_get(model, &iter, 0, &text, -1);
        g_assert_cmpstr(text, ==, expected[i]); g_free(text);
        valid = gtk_tree_model_iter_next(model, &iter);
    }
    g_assert_false(valid);
    g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), "7"));
    g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), "4"));
}
static void return_ratio(Capture measured, Capture reference, gdouble expected)
{
    for (guint c = 0; c < 2; ++c)
        g_assert_cmpfloat_with_epsilon(measured.rms[c] / reference.rms[c], expected, .06);
}
static gboolean effect_returns_saved(JsonObject *configuration)
{
    JsonObject *receiver = channel_config(configuration, 7);
    JsonObject *master = json_object_get_object_member(configuration, "master");
    return radios_json_int(receiver, "return_effect", -99) == 0 &&
        radios_json_int(receiver, "return_channel", -99) == -1 &&
        radios_json_int(master, "return_effect", -99) == 0 &&
        radios_json_int(master, "return_channel", -99) == -1;
}
static void test_independent_effect_return(void)
{
    Window window = window_open(); assert_gates_off(&window); reset_mixer(&window);
    g_assert_true(radios_window_select_profile(window.window, "dj")); run_for(250);
    for (guint attempt = 0; !find(window.root, "dj-channel-8") && attempt < 32; ++attempt) {
        click(window.root, "dj-add-channel"); run_for(100);
    }
    assert_effect_choices(window.root, "dj-channel-8-return-source");
    assert_effect_choices(window.root, "dj-channel-0-return-source");
    JsonNode *detail = media_import(&window, "Efektör dönüş PCM", 997, .2);
    native_load(&window, detail, 1);
    combo(window.root, "dj-channel-8-source", "-1");
    click(window.root, "dj-channel-2-play"); deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 100);
    run_for(250); Capture dry = measure("MAIN"); g_assert_cmpfloat(dry.rms[0], >, .04);
    RadioSAudioMixerConfig mixer; GError *error = NULL;
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    mixer.effects[0].type = RADIOS_FX_DELAY; mixer.effects[0].depth = 1;
    mixer.effects[0].tempo_bpm = 299.1; /* 200 complete periods at 997 Hz. */
    mixer.effects[0].enabled = TRUE; mixer.effects[0].return_db = 0;
    mixer.effects[1] = mixer.effects[0];
    require_call(radios_audio_ui_set_mixer(window.ctx, &mixer, &error), &error);
    combo(window.root, "dj-channel-2-effector", "0"); run_for(250);
    return_ratio(measure("MAIN"), dry, 1); /* SEND alone never returns dry or wet. */
    knob(window.root, "dj-channel-8-route-return", -6.0206);
    combo(window.root, "dj-channel-8-return-source", "0"); run_for(250);
    return_ratio(measure("MAIN"), dry, 1.5);
    combo(window.root, "dj-channel-8-effector", "0"); run_for(250);
    return_ratio(measure("MAIN"), dry, 1.5);
    GtkWidget *incoming = find(window.root, "dj-fx-incoming-1"); g_assert_true(GTK_IS_LABEL(incoming));
    GtkWidget *channel_name = find(window.root, "dj-channel-2-name"); g_assert_true(GTK_IS_ENTRY(channel_name));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(incoming)), ==, gtk_entry_get_text(GTK_ENTRY(channel_name)));
    combo(window.root, "dj-channel-8-return-source", "1"); run_for(250);
    return_ratio(measure("MAIN"), dry, 1); /* Different, un-fed effector. */
    combo(window.root, "dj-channel-8-return-source", "-1"); run_for(250);
    return_ratio(measure("MAIN"), dry, 1);
    combo(window.root, "dj-channel-8-return-source", "0");
    toggle(window.root, "dj-channel-8-mute", TRUE); run_for(250);
    return_ratio(measure("MAIN"), dry, 1);
    toggle(window.root, "dj-channel-8-mute", FALSE);
    knob(window.root, "dj-channel-0-route-return", 0);
    combo(window.root, "dj-channel-0-return-source", "0"); run_for(250);
    return_ratio(measure("MAIN"), dry, 2.5);
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    g_assert_cmpint(mixer.channels[1].fx_slot, ==, 0);
    g_assert_cmpint(mixer.channels[1].return_effect, ==, -1);
    g_assert_cmpint(mixer.channels[7].return_effect, ==, 0);
    g_assert_cmpint(mixer.master.return_effect, ==, 0);
    capture_dj(&window, "effect-return-selected.png");
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", effect_returns_saved);
    document_ack_wait(&window, record); json_node_free(record);
    click(window.root, "dj-channel-2-stop"); deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0);
    json_node_free(detail); window_close(&window);
    window = window_open(); g_assert_true(radios_window_select_profile(window.window, "dj")); run_for(250);
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    g_assert_cmpint(mixer.channels[7].return_effect, ==, 0);
    g_assert_cmpint(mixer.master.return_effect, ==, 0);
    g_assert_cmpfloat_with_epsilon(mixer.channels[7].return_db, -6.0206, .001);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "dj-channel-8-return-source"))), ==, "0");
    click(window.root, "dj-channel-8-remove"); run_for(250);
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    g_assert_cmpint(mixer.master.return_effect, ==, 0);
    g_assert_cmpint(mixer.channels[7].return_effect, ==, -1);
    assert_effect_choices(window.root, "dj-channel-0-return-source");
    reset_mixer(&window); window_close(&window);
}
static void test_legacy_return_migration(void)
{
    Window window = window_open(); reset_mixer(&window);
    JsonNode *record = document_read("dj_mixer", "DJ mixer"); g_assert_nonnull(record);
    JsonNode *legacy = deep_copy(json_object_get_member(json_node_get_object(record), "config"));
    JsonObject *old = json_node_get_object(legacy);
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i)
        json_object_remove_member(channel_config(old, i), "return_effect");
    JsonObject *master = json_object_get_object_member(old, "master");
    json_object_remove_member(master, "return_effect");
    json_object_set_int_member(channel_config(old, 0), "fx_slot", 1);
    json_object_set_int_member(channel_config(old, 0), "return_channel", 7);
    json_object_set_int_member(master, "fx_slot", 2);
    json_object_set_int_member(master, "return_channel", 7);
    window_close(&window);
    JsonNode *saved = document_remote_save(record, old);
    json_node_free(saved); json_node_free(legacy); json_node_free(record);
    window = window_open(); g_assert_true(radios_window_select_profile(window.window, "dj")); run_for(250);
    RadioSAudioMixerConfig mixer; g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    g_assert_cmpint(mixer.channels[0].return_effect, ==, 1);
    g_assert_cmpfloat_with_epsilon(mixer.channels[0].return_db, mixer.channels[0].fx_return_db, .001);
    g_assert_cmpint(mixer.channels[0].return_channel, ==, -1);
    g_assert_cmpint(mixer.master.return_effect, ==, 2);
    g_assert_cmpfloat_with_epsilon(mixer.master.return_db, mixer.master.fx_return_db, .001);
    g_assert_cmpint(mixer.master.return_channel, ==, -1);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "dj-channel-1-return-source"))), ==, "1");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "dj-channel-0-return-source"))), ==, "2");
    record = document_read("dj_mixer", "DJ mixer");
    g_assert_false(json_object_has_member(channel_config(json_object_get_object_member(json_node_get_object(record), "config"), 0), "return_effect"));
    json_node_free(record); /* Loading migrates memory without rewriting storage. */
    reset_mixer(&window); window_close(&window);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"); cue_sink = g_getenv("RADIOS_TEST_CUE_SINK"); monitor_sink = g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink); g_assert_nonnull(cue_sink); g_assert_nonnull(monitor_sink);
    g_assert_true(g_str_has_prefix(main_sink, "radios_gui_")); g_assert_true(g_str_has_prefix(cue_sink, "radios_gui_"));
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.NativeEffectReturnAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-effect-return/selector-PCM-independent-reopen-cleanup", test_independent_effect_return);
    g_test_add_func("/native-effect-return/legacy-channel-return-migration", test_legacy_return_migration);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
