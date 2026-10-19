/* The MIDI module on the real Windows MIDI system: mapping keys, the channel
 * message decoder, the WinMM listing, an open that finds no device and, only
 * where a loopback port is configured, a round trip through WinMM. A real
 * controller is never opened: WinMM input is exclusive per device, and a test
 * must not take a studio controller away from a running RadioS. */
#include "radios/midi.h"
#include <windows.h>
#include <mmsystem.h>
#include <limits.h>
#include <math.h>
#include <string.h>

typedef struct { GPtrArray *keys; GArray *values; } Received;
static void received(const RadioSMidiEvent *event, gpointer user)
{
    Received *got = user; g_ptr_array_add(got->keys, radios_midi_event_key(event)); g_array_append_val(got->values, event->value);
}
static gchar *utf8_pname(const WCHAR *pname)
{
    glong units = 0; while (units < MAXPNAMELEN && pname[units]) ++units;
    gchar *name = g_utf16_to_utf8((const gunichar2 *)pname, units, NULL, NULL, NULL);
    return name ? name : g_strdup("");
}
static gboolean port_id_shape(const gchar *id)
{ return g_regex_match_simple("^[0-9]{1,4}:[0-9a-f]{8}$", id, 0, 0); }

static void test_keys(void)
{
    g_assert_true(radios_midi_key_valid("note:1:0")); g_assert_true(radios_midi_key_valid("cc:16:127")); g_assert_true(radios_midi_key_valid("pitch:2"));
    const gchar *invalid[] = {"note:0:1", "cc:17:1", "cc:1:128", "pitch:1:2", "note:1", "x:1:1", "", NULL};
    for (guint index = 0; invalid[index]; ++index) g_assert_false(radios_midi_key_valid(invalid[index]));
    g_assert_false(radios_midi_key_valid(NULL));
    gchar *label = radios_midi_key_label("note:1:36"); g_assert_cmpstr(label, ==, "Nota 36 · Kanal 1"); g_free(label);
    label = radios_midi_key_label("cc:3:7"); g_assert_cmpstr(label, ==, "Kontrol (CC) 7 · Kanal 3"); g_free(label);
    label = radios_midi_key_label("pitch:2"); g_assert_cmpstr(label, ==, "Pitch bend · Kanal 2"); g_free(label);
    g_assert_null(radios_midi_input_open("bad", received, NULL, NULL));
    /* Not a port id of this system, refused before WinMM is asked; "20:0" is
     * an ALSA client:port address from a Linux installation. */
    const gchar *ids[] = {"bad", "20:0", "0:1234567", "0:123456789", "0:ABCDEF01", "12345:00000000", ":00000000", "a:00000000",
                          "0:0000000g", "0:00000000 ", NULL};
    for (guint index = 0; ids[index]; ++index) {
        GError *error = NULL;
        g_assert_null(radios_midi_input_open(ids[index], received, NULL, &error));
        g_assert_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_PORT);
        g_assert_cmpstr(error->message, ==, "Geçerli bir MIDI giriş portu seçin."); g_clear_error(&error);
    }
    GError *error = NULL;
    g_assert_null(radios_midi_input_open_full("0:00000000", NULL, NULL, NULL, NULL, &error));
    g_assert_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_PORT); g_clear_error(&error);
    g_assert_null(radios_midi_input_open(NULL, received, NULL, &error));
    g_assert_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_PORT); g_clear_error(&error);
    g_assert_cmpstr(g_quark_to_string(RADIOS_MIDI_ERROR), ==, "radios-midi");
    g_assert_null(radios_midi_input_port(NULL)); g_assert_false(radios_midi_input_alive(NULL));
    radios_midi_input_close(NULL);
}

/* The same events as the Linux sequencer delivered for the same bytes. */
static void test_decode(void)
{
    const struct { guint8 bytes[3]; const gchar *key; gdouble value; gboolean pressed; } valid[] = {
        {{0x90, 0x24, 0x7F}, "note:1:36", 1.0, TRUE},
        {{0x90, 0x24, 0x00}, "note:1:36", 0.0, FALSE},     /* note on, velocity 0: released */
        {{0x80, 0x24, 0x40}, "note:1:36", 0.0, FALSE},     /* note off, whatever its velocity */
        {{0xB2, 0x07, 0x40}, "cc:3:7", 64 / 127.0, TRUE},
        {{0xB2, 0x07, 0x3F}, "cc:3:7", 63 / 127.0, FALSE},
        {{0xBF, 0x7B, 0x00}, "cc:16:123", 0.0, FALSE},     /* a channel mode message is a controller too */
        {{0xEF, 0x7F, 0x7F}, "pitch:16", 1.0, TRUE},
        {{0xE0, 0x00, 0x40}, "pitch:1", 8192 / 16383.0, TRUE},
        {{0xE0, 0x7F, 0x3F}, "pitch:1", 8191 / 16383.0, FALSE},
        {{0xE5, 0x00, 0x00}, "pitch:6", 0.0, FALSE},
        {{0x9F, 0xA4, 0xFF}, "note:16:36", 1.0, TRUE},     /* data bytes masked to 7 bits */
    };
    for (guint i = 0; i < G_N_ELEMENTS(valid); ++i) {
        RadioSMidiEvent event;
        g_assert_true(radios_midi_decode_message(valid[i].bytes, 3, &event));
        gchar *key = radios_midi_event_key(&event); g_assert_cmpstr(key, ==, valid[i].key); g_free(key);
        g_assert_cmpfloat(fabs(event.value - valid[i].value), <, 1e-12);
        g_assert_cmpint(event.pressed, ==, valid[i].pressed);
        if (event.kind == RADIOS_MIDI_PITCH) g_assert_cmpuint(event.number, ==, 0);
    }
    const struct { guint8 bytes[3]; gsize length; } ignored[] = {
        {{0xC0, 0x05, 0x00}, 2}, {{0xC0, 0x05, 0x00}, 3},   /* program change */
        {{0xD0, 0x09, 0x00}, 2}, {{0xA0, 0x01, 0x02}, 3},   /* channel and key pressure */
        {{0xF8, 0x00, 0x00}, 1}, {{0xFE, 0x00, 0x00}, 1},   /* clock, active sensing */
        {{0xF0, 0x7E, 0x7F}, 3}, {{0xF2, 0x00, 0x10}, 3},   /* SysEx, song position */
        {{0x90, 0x24, 0x00}, 2}, {{0x24, 0x7F, 0x00}, 2},   /* incomplete; a data byte as status */
        {{0x24, 0x7F, 0x40}, 3}, {{0x90, 0x24, 0x7F}, 0},
    };
    for (guint i = 0; i < G_N_ELEMENTS(ignored); ++i) {
        RadioSMidiEvent event; memset(&event, 0xA5, sizeof event);
        g_assert_false(radios_midi_decode_message(ignored[i].bytes, ignored[i].length, &event));
        g_assert_cmpuint(event.channel, ==, 0); g_assert_cmpfloat(event.value, ==, 0); g_assert_false(event.pressed);
    }
    RadioSMidiEvent event;
    g_assert_false(radios_midi_decode_message(NULL, 3, &event));
    g_assert_false(radios_midi_decode_message(valid[0].bytes, 3, NULL));
}

/* The real listing: valid UTF-8, unique names, port ids of the documented
 * shape. Nothing is opened. */
static void test_system_list(void)
{
    GError *error = NULL; GPtrArray *ports = radios_midi_list_inputs(&error);
    g_assert_no_error(error); g_assert_nonnull(ports);
    g_assert_cmpuint(ports->len, <=, midiInGetNumDevs());
    GHashTable *names = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < ports->len; ++i) {
        const RadioSMidiPort *port = g_ptr_array_index(ports, i);
        g_test_message("MIDI input %u: %s (%s)", i, port->name, port->id);
        g_assert_true(port_id_shape(port->id));
        g_assert_true(g_utf8_validate(port->name, -1, NULL)); g_assert_cmpstr(port->name, !=, "");
        g_assert_false(g_hash_table_contains(names, port->name)); g_hash_table_add(names, (gpointer)port->name);
    }
    g_test_message("%u MIDI inputs, %u WinMM input devices", ports->len, midiInGetNumDevs());
    g_hash_table_unref(names); g_ptr_array_unref(ports);
}

/* A well-formed id that no device has: device notifications are registered
 * and released on the real system, the devices are listed, nothing opens. */
static void test_system_open_missing(void)
{
    Received got = {g_ptr_array_new_with_free_func(g_free), g_array_new(FALSE, FALSE, sizeof(gdouble))};
    for (guint round = 0; round < 3; ++round) {
        GError *error = NULL;
        RadioSMidiInput *input = radios_midi_input_open("9999:00000000", received, &got, &error);
        g_assert_null(input); g_assert_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_CONNECT);
        g_assert_nonnull(strstr(error->message, "cihaz bulunamadı")); g_clear_error(&error);
    }
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_assert_cmpuint(got.keys->len, ==, 0);
    g_ptr_array_unref(got.keys); g_array_unref(got.values);
}

static UINT loopback_output(const gchar *name)
{
    const UINT count = midiOutGetNumDevs();
    for (UINT index = 0; index < count; ++index) {
        MIDIOUTCAPSW caps; memset(&caps, 0, sizeof caps);
        if (midiOutGetDevCapsW(index, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        gchar *found = utf8_pname(caps.szPname); const gboolean same = g_str_equal(found, name); g_free(found);
        if (same) return index;
    }
    return UINT_MAX;
}
static void send_short(HMIDIOUT out, DWORD message) { g_assert_cmpuint(midiOutShortMsg(out, message), ==, MMSYSERR_NOERROR); }
/* A loopback port (loopMIDI, a Windows MIDI Services app loopback...) named
 * by RADIOS_TEST_MIDI_LOOPBACK_OUT (output) and RADIOS_TEST_MIDI_LOOPBACK_IN
 * (its input, by default the same name): what is sent to it arrives on the
 * main context with the stable mapping keys, and nothing after close. */
static void test_virtual_controller(void)
{
    const gchar *out_name = g_getenv("RADIOS_TEST_MIDI_LOOPBACK_OUT"), *in_name = g_getenv("RADIOS_TEST_MIDI_LOOPBACK_IN");
    if (!out_name || !*out_name) { g_test_skip("MIDI loopback yok (RADIOS_TEST_MIDI_LOOPBACK_OUT)"); return; }
    if (!in_name || !*in_name) in_name = out_name;
    const UINT out_index = loopback_output(out_name);
    if (out_index == UINT_MAX) { g_test_skip("MIDI loopback çıkışı bulunamadı (RADIOS_TEST_MIDI_LOOPBACK_OUT)"); return; }
    GPtrArray *ports = radios_midi_list_inputs(NULL); gchar *id = NULL;
    for (guint index = 0; index < ports->len && !id; ++index) {
        const RadioSMidiPort *entry = g_ptr_array_index(ports, index);
        if (g_str_equal(entry->name, in_name)) id = g_strdup(entry->id);
    }
    g_ptr_array_unref(ports);
    g_assert_nonnull(id);
    Received got = {g_ptr_array_new_with_free_func(g_free), g_array_new(FALSE, FALSE, sizeof(gdouble))};
    GError *error = NULL; RadioSMidiInput *input = radios_midi_input_open(id, received, &got, &error);
    g_assert_no_error(error); g_assert_nonnull(input); g_assert_cmpstr(radios_midi_input_port(input), ==, id);
    HMIDIOUT out = NULL;
    g_assert_cmpuint(midiOutOpen(&out, out_index, 0, 0, CALLBACK_NULL), ==, MMSYSERR_NOERROR);
    send_short(out, 0x7F2490); /* note on, channel 1, note 36, velocity 127 */
    send_short(out, 0x002480); /* note off */
    send_short(out, 0x4007B2); /* channel 3, controller 7 = 64 */
    send_short(out, 0x7F7FEF); /* channel 16, pitch bend at its top */
    gint64 until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    while (got.keys->len < 4 && g_get_monotonic_time() < until) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
    for (guint i = 0; i < got.keys->len; ++i) g_test_message("event %u: %s %.3f", i, (gchar *)g_ptr_array_index(got.keys, i), g_array_index(got.values, gdouble, i));
    g_assert_cmpuint(got.keys->len, ==, 4);
    g_assert_cmpstr(g_ptr_array_index(got.keys, 0), ==, "note:1:36"); g_assert_cmpfloat(g_array_index(got.values, gdouble, 0), ==, 1);
    g_assert_cmpstr(g_ptr_array_index(got.keys, 1), ==, "note:1:36"); g_assert_cmpfloat(g_array_index(got.values, gdouble, 1), ==, 0);
    g_assert_cmpstr(g_ptr_array_index(got.keys, 2), ==, "cc:3:7"); g_assert_cmpfloat(fabs(g_array_index(got.values, gdouble, 2) - 64 / 127.0), <, 1e-9);
    g_assert_cmpstr(g_ptr_array_index(got.keys, 3), ==, "pitch:16"); g_assert_cmpfloat(g_array_index(got.values, gdouble, 3), ==, 1);
    /* Nothing is delivered after close, even for queued input. */
    send_short(out, 0x642890);
    radios_midi_input_close(input);
    until = g_get_monotonic_time() + G_USEC_PER_SEC / 5;
    while (g_get_monotonic_time() < until) { g_main_context_iteration(NULL, FALSE); g_usleep(1000); }
    g_assert_cmpuint(got.keys->len, ==, 4);
    g_assert_cmpuint(midiOutClose(out), ==, MMSYSERR_NOERROR);
    g_ptr_array_unref(got.keys); g_array_unref(got.values); g_free(id);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/midi/keys", test_keys);
    g_test_add_func("/midi/decode", test_decode);
    g_test_add_func("/midi/system-list", test_system_list);
    g_test_add_func("/midi/system-open-missing", test_system_open_missing);
    g_test_add_func("/midi/virtual-controller", test_virtual_controller);
    return g_test_run();
}
