#ifndef RADIOS_RUNTIME_H
#define RADIOS_RUNTIME_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct RadioSRuntime RadioSRuntime;
typedef struct RadioSCancelToken RadioSCancelToken;

typedef enum {
    RADIO_S_RUNTIME_ERROR_INVALID_ARGUMENT,
    RADIO_S_RUNTIME_ERROR_FULL,
    RADIO_S_RUNTIME_ERROR_STOPPED,
    RADIO_S_RUNTIME_ERROR_ID_EXHAUSTED
} RadioSRuntimeError;

#define RADIO_S_RUNTIME_ERROR (radios_runtime_error_quark())
GQuark radios_runtime_error_quark(void);

typedef enum {
    RADIO_S_EVENT_STARTED,
    RADIO_S_EVENT_COMPLETED,
    RADIO_S_EVENT_FAILED,
    RADIO_S_EVENT_CANCELLED
} RadioSEventKind;

typedef struct RadioSEvent {
    guint64 task_id;
    RadioSEventKind kind;
    gchar *message;
} RadioSEvent;

/* Runs on the worker thread. Return an owned UTF-8 completion message, or set
 * error on failure. NULL without an error is a successful empty message.
 * The token is valid only for the duration of this callback. Blocking work must
 * use bounded I/O and check cancellation; shutdown cannot interrupt arbitrary
 * third-party blocking calls. Never call shutdown/free from work or its data
 * destructor: shutdown joins this worker. */
typedef gchar *(*RadioSWorkFunc)(gpointer data,
                               const RadioSCancelToken *cancel,
                               GError **error);

gboolean radios_cancelled(const RadioSCancelToken *cancel);

/* Capacities must be positive. Task capacity counts waiting tasks; the current
 * task is separate. Event capacity counts all queued status events. */
RadioSRuntime *radios_runtime_new(guint task_capacity, guint event_capacity);

/* Safe to call from another thread while the runtime remains alive.
 * A nonzero ID transfers data to the runtime. On any failure the caller keeps
 * data and must destroy it. An accepted task's destructor runs exactly once,
 * on the worker for a running task or on the shutdown caller for a pending one. */
guint64 radios_runtime_submit(RadioSRuntime *runtime,
                             RadioSWorkFunc work,
                             gpointer data,
                             GDestroyNotify destroy,
                             GError **error);

/* Readiness work goes ahead of waiting background work, preserving FIFO
 * order within both classes. It shares the same bounded queue, ownership,
 * cancellation and shutdown rules; current work is never interrupted. */
guint64 radios_runtime_submit_priority(RadioSRuntime *runtime,
                                      RadioSWorkFunc work,
                                      gpointer data,
                                      GDestroyNotify destroy,
                                      GError **error);

/* Nonblocking; the caller owns the returned event. Normal operation never
 * drops events: a full event queue applies backpressure to the worker. */
RadioSEvent *radios_runtime_pop_event(RadioSRuntime *runtime);
void radios_event_free(RadioSEvent *event);

/* Call on the owning/UI thread, not concurrently with shutdown/free. Stops
 * admission, cancels current work, destroys pending tasks, wakes queue waits,
 * and joins the worker. Idempotent. Existing events remain available to pop;
 * shutdown deliberately stops further event delivery so closing an undrained
 * window cannot deadlock. Work must cooperate with the cancellation token. */
void radios_runtime_shutdown(RadioSRuntime *runtime);
void radios_runtime_free(RadioSRuntime *runtime);

G_END_DECLS

#endif
