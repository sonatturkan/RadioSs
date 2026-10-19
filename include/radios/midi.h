#ifndef RADIOS_MIDI_H
#define RADIOS_MIDI_H
#include <glib.h>

/* MIDI 1 input from the WinMM MIDI input devices of Windows (USB
 * controllers, virtual and loopback ports). */
typedef struct {
    gchar id[24];    /* opaque and transient ("index:fingerprint"): valid only to open a port of this
                      * listing; never persist it */
    gchar name[192]; /* the device's WinMM name (at most 31 characters); exact duplicates get " #2",
                      * " #3"... in device path order, so a name stays with its USB socket. Persist this. */
} RadioSMidiPort;
typedef enum { RADIOS_MIDI_NOTE, RADIOS_MIDI_CONTROL, RADIOS_MIDI_PITCH } RadioSMidiKind;
typedef struct {
    RadioSMidiKind kind;
    guint channel; /* 1..16 */
    guint number;  /* note or controller 0..127; 0 for pitch bend */
    gdouble value; /* 0..1: velocity, controller value or bend */
    gboolean pressed; /* note on with velocity, or controller at least half */
} RadioSMidiEvent;

/* Errors of radios_midi_input_open*. */
#define RADIOS_MIDI_ERROR (radios_midi_error_quark())
GQuark radios_midi_error_quark(void);
typedef enum {
    RADIOS_MIDI_ERROR_SYSTEM = 1,  /* the MIDI system could not be used */
    RADIOS_MIDI_ERROR_PORT = 2,    /* not a port id of radios_midi_list_inputs, or no callback */
    RADIOS_MIDI_ERROR_CONNECT = 3, /* the device is gone or could not be opened */
    RADIOS_MIDI_ERROR_BUSY = 4     /* another application holds the device (WinMM input is exclusive per device) */
} RadioSMidiError;

/* A control's stable mapping key: "note:CH:N", "cc:CH:N" or "pitch:CH". */
gchar *radios_midi_event_key(const RadioSMidiEvent *event);
gboolean radios_midi_key_valid(const gchar *key);
/* Operator wording, e.g. "Nota 36 · Kanal 1". */
gchar *radios_midi_key_label(const gchar *key);
/* Decodes one complete MIDI 1.0 channel voice message (status byte first, no
 * running status). FALSE for anything RadioS does not map: system and
 * real-time messages, SysEx, program change, (poly) aftertouch, incomplete
 * input. */
gboolean radios_midi_decode_message(const guint8 *bytes, gsize length, RadioSMidiEvent *event);

/* The MIDI input devices. Returns an array of RadioSMidiPort* (free with
 * g_ptr_array_unref); never NULL. Enumerating WinMM devices can block (USB
 * enumeration, the Windows MIDI service starting): call it off the GTK
 * thread. */
GPtrArray *radios_midi_list_inputs(GError **error);

typedef void (*RadioSMidiCallback)(const RadioSMidiEvent *event, gpointer user);
/* What an open input hears about the devices (Windows device notifications
 * and the WinMM device list). */
typedef enum {
    RADIOS_MIDI_NOTICE_LOST, /* the device was removed (USB unplug), its driver closed the input, or it stopped answering */
    RADIOS_MIDI_NOTICE_PORT  /* a new MIDI input device became available, e.g. a controller was plugged in */
} RadioSMidiNotice;
typedef void (*RadioSMidiNoticeCallback)(RadioSMidiNotice notice, gpointer user);
typedef struct RadioSMidiInput RadioSMidiInput;
/* Opens `port_id` and reads it on a worker thread. Events are delivered on
 * the main context that was the thread default at open time. */
RadioSMidiInput *radios_midi_input_open(const gchar *port_id, RadioSMidiCallback callback, gpointer user, GError **error);
/* As radios_midi_input_open, delivering events and notices on `context`
 * (NULL: the thread default at open time), so it can be opened from a worker
 * thread. A lost input sends LOST once and releases the device at once (so
 * the device can be opened again); it stays open and keeps reporting PORT
 * notices until closed. Opening enumerates and opens WinMM devices (it can
 * take a while during USB enumeration): call it off the GTK thread. */
RadioSMidiInput *radios_midi_input_open_full(const gchar *port_id, GMainContext *context, RadioSMidiCallback callback,
    RadioSMidiNoticeCallback notice, gpointer user, GError **error);
const gchar *radios_midi_input_port(RadioSMidiInput *input);
/* FALSE once the device is gone or no longer feeds this input: seen at once
 * from the Windows device notification or the driver closing the input, else
 * within about one to two seconds. */
gboolean radios_midi_input_alive(RadioSMidiInput *input);
/* Stops the worker and releases the device; no callback runs after this
 * returns. Call it on the thread of the delivery context. */
void radios_midi_input_close(RadioSMidiInput *input);
#endif
