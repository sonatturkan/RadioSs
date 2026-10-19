#include "radios/runtime.h"

typedef struct {
    guint64 id;
    gboolean priority;
    RadioSWorkFunc work;
    gpointer data;
    GDestroyNotify destroy;
} RadioSTask;

struct RadioSCancelToken {
    gint requested;
};

struct RadioSRuntime {
    GMutex mutex;
    GCond work_ready;
    GCond event_space;
    GQueue tasks;
    GQueue events;
    GThread *worker;
    RadioSCancelToken cancel;
    guint task_capacity;
    guint event_capacity;
    guint64 next_id;
    gboolean stopping;
};

GQuark
radios_runtime_error_quark(void)
{
    return g_quark_from_static_string("radios-runtime-error");
}

gboolean
radios_cancelled(const RadioSCancelToken *cancel)
{
    return cancel != NULL && g_atomic_int_get(&cancel->requested) != 0;
}

static void
task_free(RadioSTask *task)
{
    if (task->destroy != NULL)
        task->destroy(task->data);
    g_free(task);
}

void
radios_event_free(RadioSEvent *event)
{
    if (event == NULL)
        return;
    g_free(event->message);
    g_free(event);
}

/* Takes ownership even when shutdown prevents delivery. */
static gboolean
publish_event(RadioSRuntime *runtime, RadioSEvent *event)
{
    g_mutex_lock(&runtime->mutex);
    while (!runtime->stopping &&
           g_queue_get_length(&runtime->events) >= runtime->event_capacity)
        g_cond_wait(&runtime->event_space, &runtime->mutex);

    if (runtime->stopping) {
        g_mutex_unlock(&runtime->mutex);
        radios_event_free(event);
        return FALSE;
    }

    g_queue_push_tail(&runtime->events, event);
    g_mutex_unlock(&runtime->mutex);
    return TRUE;
}

static RadioSEvent *
event_new(guint64 id, RadioSEventKind kind, gchar *message)
{
    RadioSEvent *event = g_new0(RadioSEvent, 1);
    event->task_id = id;
    event->kind = kind;
    event->message = message != NULL ? message : g_strdup("");
    return event;
}

static gpointer
worker_main(gpointer data)
{
    RadioSRuntime *runtime = data;

    for (;;) {
        RadioSTask *task;
        RadioSEvent *event;
        gchar *message = NULL;
        GError *error = NULL;
        guint64 task_id;

        g_mutex_lock(&runtime->mutex);
        while (!runtime->stopping && g_queue_is_empty(&runtime->tasks))
            g_cond_wait(&runtime->work_ready, &runtime->mutex);
        if (runtime->stopping) {
            g_mutex_unlock(&runtime->mutex);
            return NULL;
        }
        task = g_queue_pop_head(&runtime->tasks);
        g_mutex_unlock(&runtime->mutex);

        task_id = task->id;
        if (!publish_event(runtime,
                           event_new(task_id, RADIO_S_EVENT_STARTED, NULL))) {
            task_free(task);
            return NULL;
        }

        if (!radios_cancelled(&runtime->cancel))
            message = task->work(task->data, &runtime->cancel, &error);

        if (radios_cancelled(&runtime->cancel)) {
            g_free(message);
            message = NULL;
            event = event_new(task_id, RADIO_S_EVENT_CANCELLED, NULL);
        } else if (error != NULL) {
            g_free(message);
            message = NULL;
            event = event_new(task_id, RADIO_S_EVENT_FAILED,
                              g_strdup(error->message));
        } else {
            event = event_new(task_id, RADIO_S_EVENT_COMPLETED, message);
        }
        g_clear_error(&error);
        task_free(task);

        if (!publish_event(runtime, event))
            return NULL;
    }
}

RadioSRuntime *
radios_runtime_new(guint task_capacity, guint event_capacity)
{
    RadioSRuntime *runtime;

    if (task_capacity == 0 || event_capacity == 0)
        return NULL;

    runtime = g_new0(RadioSRuntime, 1);
    g_mutex_init(&runtime->mutex);
    g_cond_init(&runtime->work_ready);
    g_cond_init(&runtime->event_space);
    g_queue_init(&runtime->tasks);
    g_queue_init(&runtime->events);
    runtime->task_capacity = task_capacity;
    runtime->event_capacity = event_capacity;
    runtime->next_id = 1;
    runtime->worker = g_thread_new("radios-worker", worker_main, runtime);
    return runtime;
}

static guint64
runtime_submit(RadioSRuntime *runtime, RadioSWorkFunc work, gpointer data,
               GDestroyNotify destroy, gboolean priority, GError **error)
{
    RadioSTask *task;
    guint64 id;

    if (runtime == NULL || work == NULL) {
        g_set_error_literal(error, RADIO_S_RUNTIME_ERROR,
                            RADIO_S_RUNTIME_ERROR_INVALID_ARGUMENT,
                            "A runtime and work callback are required.");
        return 0;
    }

    g_mutex_lock(&runtime->mutex);
    if (runtime->stopping) {
        g_mutex_unlock(&runtime->mutex);
        g_set_error_literal(error, RADIO_S_RUNTIME_ERROR,
                            RADIO_S_RUNTIME_ERROR_STOPPED,
                            "The runtime has stopped accepting tasks.");
        return 0;
    }
    if (g_queue_get_length(&runtime->tasks) >= runtime->task_capacity) {
        g_mutex_unlock(&runtime->mutex);
        g_set_error_literal(error, RADIO_S_RUNTIME_ERROR,
                            RADIO_S_RUNTIME_ERROR_FULL,
                            "The task queue is full.");
        return 0;
    }
    if (runtime->next_id == 0) {
        g_mutex_unlock(&runtime->mutex);
        g_set_error_literal(error, RADIO_S_RUNTIME_ERROR,
                            RADIO_S_RUNTIME_ERROR_ID_EXHAUSTED,
                            "The task identifier range is exhausted.");
        return 0;
    }

    task = g_new0(RadioSTask, 1);
    id = runtime->next_id++;
    task->id = id;
    task->work = work;
    task->data = data;
    task->destroy = destroy;
    task->priority = priority;
    GList *before = NULL;
    if (priority) {
        for (GList *item = runtime->tasks.head; item; item = item->next)
            if (!((RadioSTask *)item->data)->priority) { before = item; break; }
    }
    if (before) g_queue_insert_before(&runtime->tasks, before, task);
    else g_queue_push_tail(&runtime->tasks, task);
    g_cond_signal(&runtime->work_ready);
    g_mutex_unlock(&runtime->mutex);
    return id;
}

guint64 radios_runtime_submit(RadioSRuntime *runtime, RadioSWorkFunc work,
    gpointer data, GDestroyNotify destroy, GError **error)
{ return runtime_submit(runtime, work, data, destroy, FALSE, error); }

guint64 radios_runtime_submit_priority(RadioSRuntime *runtime, RadioSWorkFunc work,
    gpointer data, GDestroyNotify destroy, GError **error)
{ return runtime_submit(runtime, work, data, destroy, TRUE, error); }

RadioSEvent *
radios_runtime_pop_event(RadioSRuntime *runtime)
{
    RadioSEvent *event;

    if (runtime == NULL)
        return NULL;

    g_mutex_lock(&runtime->mutex);
    event = g_queue_pop_head(&runtime->events);
    if (event != NULL)
        g_cond_signal(&runtime->event_space);
    g_mutex_unlock(&runtime->mutex);
    return event;
}

void
radios_runtime_shutdown(RadioSRuntime *runtime)
{
    GQueue pending = G_QUEUE_INIT;
    GThread *worker;
    RadioSTask *task;

    if (runtime == NULL)
        return;

    g_mutex_lock(&runtime->mutex);
    worker = runtime->worker;
    if (worker == NULL) {
        g_mutex_unlock(&runtime->mutex);
        return;
    }
    runtime->stopping = TRUE;
    g_atomic_int_set(&runtime->cancel.requested, 1);
    pending = runtime->tasks;
    g_queue_init(&runtime->tasks);
    g_cond_broadcast(&runtime->work_ready);
    g_cond_broadcast(&runtime->event_space);
    g_mutex_unlock(&runtime->mutex);

    while ((task = g_queue_pop_head(&pending)) != NULL)
        task_free(task);
    g_thread_join(worker);

    g_mutex_lock(&runtime->mutex);
    runtime->worker = NULL;
    g_mutex_unlock(&runtime->mutex);
}

void
radios_runtime_free(RadioSRuntime *runtime)
{
    RadioSEvent *event;

    if (runtime == NULL)
        return;
    radios_runtime_shutdown(runtime);
    while ((event = radios_runtime_pop_event(runtime)) != NULL)
        radios_event_free(event);
    g_cond_clear(&runtime->work_ready);
    g_cond_clear(&runtime->event_space);
    g_mutex_clear(&runtime->mutex);
    g_free(runtime);
}
