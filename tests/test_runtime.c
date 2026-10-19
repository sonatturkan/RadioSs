#include "radios/runtime.h"

typedef enum {
    TEST_WORK_IMMEDIATE,
    TEST_WORK_BLOCKED,
    TEST_WORK_UNTIL_CANCELLED,
    TEST_WORK_FAILURE
} TestWorkKind;

typedef struct {
    GMutex mutex;
    GCond changed;
    guint calls;
    guint destroyed;
    gboolean release;
    gboolean observed_cancel;
} TestState;

typedef struct {
    TestState *state;
    TestWorkKind kind;
} TestTask;

static void
state_init(TestState *state)
{
    *state = (TestState) { 0 };
    g_mutex_init(&state->mutex);
    g_cond_init(&state->changed);
}

static void
state_clear(TestState *state)
{
    g_cond_clear(&state->changed);
    g_mutex_clear(&state->mutex);
}

static void
state_wait(TestState *state, gboolean destruction)
{
    const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    g_mutex_lock(&state->mutex);
    while ((destruction ? state->destroyed : state->calls) == 0) {
        gboolean signalled = g_cond_wait_until(&state->changed, &state->mutex,
                                              deadline);
        g_assert_true(signalled ||
                      (destruction ? state->destroyed : state->calls) > 0);
    }
    g_mutex_unlock(&state->mutex);
}

static TestTask *
task_new(TestState *state, TestWorkKind kind)
{
    TestTask *task = g_new0(TestTask, 1);
    task->state = state;
    task->kind = kind;
    return task;
}

static gchar *
test_work(gpointer data, const RadioSCancelToken *cancel, GError **error)
{
    TestTask *task = data;
    TestState *state = task->state;

    g_mutex_lock(&state->mutex);
    state->calls++;
    g_cond_broadcast(&state->changed);
    if (task->kind == TEST_WORK_BLOCKED) {
        while (!state->release)
            g_cond_wait(&state->changed, &state->mutex);
    }
    g_mutex_unlock(&state->mutex);

    if (task->kind == TEST_WORK_UNTIL_CANCELLED) {
        while (!radios_cancelled(cancel))
            g_thread_yield();
        g_mutex_lock(&state->mutex);
        state->observed_cancel = TRUE;
        g_mutex_unlock(&state->mutex);
    }

    if (task->kind == TEST_WORK_FAILURE) {
        g_set_error_literal(error, g_quark_from_static_string("test-work-error"),
                            3, "Readable worker error");
        return g_strdup("Discarded result on failure");
    }

    return g_strdup("Real work completed");
}

static void
test_destroy(gpointer data)
{
    TestTask *task = data;
    TestState *state = task->state;

    g_mutex_lock(&state->mutex);
    state->destroyed++;
    g_cond_broadcast(&state->changed);
    g_mutex_unlock(&state->mutex);
    g_free(task);
}

/* pop_event is intentionally nonblocking. Yield until its observable predicate
 * is satisfied; the deadline is a failure guard, not a scheduling assumption. */
static RadioSEvent *
wait_event(RadioSRuntime *runtime)
{
    const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    RadioSEvent *event;

    while ((event = radios_runtime_pop_event(runtime)) == NULL) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_thread_yield();
    }
    return event;
}

static void
expect_event(RadioSRuntime *runtime, guint64 id, RadioSEventKind kind,
             const gchar *message)
{
    RadioSEvent *event = wait_event(runtime);
    g_assert_cmpuint(event->task_id, ==, id);
    g_assert_cmpint(event->kind, ==, kind);
    if (message != NULL)
        g_assert_cmpstr(event->message, ==, message);
    radios_event_free(event);
}

static void
test_execution(void)
{
    TestState state;
    RadioSRuntime *runtime = radios_runtime_new(2, 4);
    guint64 id;
    GError *error = NULL;

    state_init(&state);
    id = radios_runtime_submit(runtime, test_work,
                               task_new(&state, TEST_WORK_IMMEDIATE),
                               test_destroy, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(id, >, 0);
    expect_event(runtime, id, RADIO_S_EVENT_STARTED, "");
    expect_event(runtime, id, RADIO_S_EVENT_COMPLETED, "Real work completed");
    radios_runtime_free(runtime);
    g_assert_cmpuint(state.calls, ==, 1);
    g_assert_cmpuint(state.destroyed, ==, 1);
    state_clear(&state);
}

static void
test_failure(void)
{
    TestState state;
    RadioSRuntime *runtime = radios_runtime_new(1, 2);
    guint64 id;
    GError *error = NULL;

    state_init(&state);
    id = radios_runtime_submit(runtime, test_work,
                               task_new(&state, TEST_WORK_FAILURE),
                               test_destroy, &error);
    g_assert_no_error(error);
    expect_event(runtime, id, RADIO_S_EVENT_STARTED, NULL);
    expect_event(runtime, id, RADIO_S_EVENT_FAILED, "Readable worker error");
    radios_runtime_free(runtime);
    g_assert_cmpuint(state.calls, ==, 1);
    g_assert_cmpuint(state.destroyed, ==, 1);
    state_clear(&state);
}

static void
test_capacity_and_ownership(void)
{
    TestState running, pending, rejected;
    RadioSRuntime *runtime = radios_runtime_new(1, 4);
    TestTask *rejected_task;
    guint64 first, second;
    GError *error = NULL;

    state_init(&running);
    state_init(&pending);
    state_init(&rejected);
    first = radios_runtime_submit(runtime, test_work,
                                  task_new(&running, TEST_WORK_BLOCKED),
                                  test_destroy, &error);
    g_assert_no_error(error);
    state_wait(&running, FALSE);

    second = radios_runtime_submit(runtime, test_work,
                                   task_new(&pending, TEST_WORK_IMMEDIATE),
                                   test_destroy, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(second, >, first);

    rejected_task = task_new(&rejected, TEST_WORK_IMMEDIATE);
    g_assert_cmpuint(radios_runtime_submit(runtime, test_work, rejected_task,
                                          test_destroy, &error), ==, 0);
    g_assert_error(error, RADIO_S_RUNTIME_ERROR, RADIO_S_RUNTIME_ERROR_FULL);
    g_clear_error(&error);
    g_assert_cmpuint(rejected.calls, ==, 0);
    g_assert_cmpuint(rejected.destroyed, ==, 0);
    test_destroy(rejected_task);

    g_mutex_lock(&running.mutex);
    running.release = TRUE;
    g_cond_broadcast(&running.changed);
    g_mutex_unlock(&running.mutex);
    expect_event(runtime, first, RADIO_S_EVENT_STARTED, NULL);
    expect_event(runtime, first, RADIO_S_EVENT_COMPLETED, NULL);
    expect_event(runtime, second, RADIO_S_EVENT_STARTED, NULL);
    expect_event(runtime, second, RADIO_S_EVENT_COMPLETED, NULL);
    radios_runtime_free(runtime);
    g_assert_cmpuint(running.destroyed, ==, 1);
    g_assert_cmpuint(pending.calls, ==, 1);
    g_assert_cmpuint(pending.destroyed, ==, 1);
    g_assert_cmpuint(rejected.destroyed, ==, 1);
    state_clear(&running);
    state_clear(&pending);
    state_clear(&rejected);
}

static void
test_event_backpressure(void)
{
    TestState state;
    RadioSRuntime *runtime = radios_runtime_new(1, 1);
    GError *error = NULL;
    guint64 id;

    state_init(&state);
    id = radios_runtime_submit(runtime, test_work,
                               task_new(&state, TEST_WORK_IMMEDIATE),
                               test_destroy, &error);
    g_assert_no_error(error);
    /* The worker has finished while the single status slot still holds STARTED.
     * Releasing that slot must deliver completion, rather than drop it. */
    state_wait(&state, TRUE);
    expect_event(runtime, id, RADIO_S_EVENT_STARTED, NULL);
    expect_event(runtime, id, RADIO_S_EVENT_COMPLETED, "Real work completed");
    g_assert_null(radios_runtime_pop_event(runtime));
    radios_runtime_free(runtime);
    state_clear(&state);
}

static void
test_cancel_and_pending_cleanup(void)
{
    TestState running, pending, rejected;
    RadioSRuntime *runtime = radios_runtime_new(2, 2);
    GError *error = NULL;
    TestTask *rejected_task;
    guint64 id;

    state_init(&running);
    state_init(&pending);
    state_init(&rejected);
    id = radios_runtime_submit(runtime, test_work,
                               task_new(&running, TEST_WORK_UNTIL_CANCELLED),
                               test_destroy, &error);
    g_assert_no_error(error);
    state_wait(&running, FALSE);
    g_assert_cmpuint(radios_runtime_submit(runtime, test_work,
                                          task_new(&pending, TEST_WORK_IMMEDIATE),
                                          test_destroy, &error), >, 0);
    g_assert_no_error(error);

    radios_runtime_shutdown(runtime);
    radios_runtime_shutdown(runtime);
    g_assert_true(running.observed_cancel);
    g_assert_cmpuint(running.calls, ==, 1);
    g_assert_cmpuint(running.destroyed, ==, 1);
    g_assert_cmpuint(pending.calls, ==, 0);
    g_assert_cmpuint(pending.destroyed, ==, 1);
    expect_event(runtime, id, RADIO_S_EVENT_STARTED, NULL);
    g_assert_null(radios_runtime_pop_event(runtime));

    rejected_task = task_new(&rejected, TEST_WORK_IMMEDIATE);
    g_assert_cmpuint(radios_runtime_submit(runtime, test_work, rejected_task,
                                          test_destroy, &error), ==, 0);
    g_assert_error(error, RADIO_S_RUNTIME_ERROR, RADIO_S_RUNTIME_ERROR_STOPPED);
    g_clear_error(&error);
    g_assert_cmpuint(rejected.destroyed, ==, 0);
    test_destroy(rejected_task);
    radios_runtime_free(runtime);
    g_assert_cmpuint(rejected.destroyed, ==, 1);
    state_clear(&running);
    state_clear(&pending);
    state_clear(&rejected);
}

static void
test_shutdown_full_event_queue(void)
{
    TestState running, pending;
    RadioSRuntime *runtime;
    GError *error = NULL;
    guint64 id;

    if (!g_test_subprocess()) {
        /* A deadlock is an observable test failure, bounded independently of
         * the worker implementation. No sleeps determine ordering. */
        g_test_trap_subprocess(NULL, 5 * G_TIME_SPAN_SECOND, 0);
        g_test_trap_assert_passed();
        return;
    }

    state_init(&running);
    state_init(&pending);
    runtime = radios_runtime_new(2, 1);
    id = radios_runtime_submit(runtime, test_work,
                               task_new(&running, TEST_WORK_IMMEDIATE),
                               test_destroy, &error);
    g_assert_no_error(error);
    state_wait(&running, TRUE);
    g_assert_cmpuint(radios_runtime_submit(runtime, test_work,
                                          task_new(&pending, TEST_WORK_IMMEDIATE),
                                          test_destroy, &error), >, 0);
    g_assert_no_error(error);
    radios_runtime_shutdown(runtime);
    radios_runtime_shutdown(runtime);
    expect_event(runtime, id, RADIO_S_EVENT_STARTED, NULL);
    g_assert_null(radios_runtime_pop_event(runtime));
    g_assert_cmpuint(running.destroyed, ==, 1);
    g_assert_cmpuint(pending.calls, ==, 0);
    g_assert_cmpuint(pending.destroyed, ==, 1);
    radios_runtime_free(runtime);
    state_clear(&running);
    state_clear(&pending);
}

static void
test_idle_shutdown_and_invalid_work(void)
{
    RadioSRuntime *runtime = radios_runtime_new(1, 1);
    GError *error = NULL;
    TestState state;
    TestTask *task;

    state_init(&state);
    task = task_new(&state, TEST_WORK_IMMEDIATE);
    g_assert_cmpuint(radios_runtime_submit(runtime, NULL, task,
                                          test_destroy, &error), ==, 0);
    g_assert_error(error, RADIO_S_RUNTIME_ERROR,
                   RADIO_S_RUNTIME_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_cmpuint(state.destroyed, ==, 0);
    test_destroy(task);
    radios_runtime_shutdown(runtime);
    radios_runtime_shutdown(runtime);
    radios_runtime_free(runtime);
    state_clear(&state);
    g_assert_null(radios_runtime_new(0, 1));
    g_assert_null(radios_runtime_new(1, 0));
    g_assert_false(radios_cancelled(NULL));
}

typedef enum {
    RACE_RUNNING,
    RACE_PENDING,
    RACE_REJECTED,
    RACE_TASK_COUNT
} RaceTaskKind;

typedef struct {
    GMutex mutex;
    GCond changed;
    RadioSRuntime *runtime;
    guint64 running_id;
    guint64 pending_id;
    guint calls[RACE_TASK_COUNT];
    guint destroyed[RACE_TASK_COUNT];
    gboolean worker_entered;
    gboolean producer_queued;
    gboolean pending_destroyed;
    gboolean producer_rejected;
    gboolean consumer_done;
    gboolean release_worker;
    gboolean observed_cancel;
} ShutdownRace;

typedef struct {
    ShutdownRace *race;
    RaceTaskKind kind;
} RaceTask;

/* All predicates are protected by race->mutex. The deadline only catches a
 * broken handshake; no timing or polling determines thread ordering. */
static void
race_wait_locked(ShutdownRace *race, const gboolean *predicate)
{
    const gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!*predicate) {
        gboolean signalled = g_cond_wait_until(&race->changed, &race->mutex,
                                              deadline);
        g_assert_true(signalled || *predicate);
    }
}

static RaceTask *
race_task_new(ShutdownRace *race, RaceTaskKind kind)
{
    RaceTask *task = g_new0(RaceTask, 1);
    task->race = race;
    task->kind = kind;
    return task;
}

static gchar *
race_work(gpointer data, const RadioSCancelToken *cancel, GError **error)
{
    RaceTask *task = data;
    ShutdownRace *race = task->race;
    (void) error;

    g_mutex_lock(&race->mutex);
    race->calls[task->kind]++;
    if (task->kind == RACE_RUNNING) {
        race->worker_entered = TRUE;
        g_cond_broadcast(&race->changed);
        race_wait_locked(race, &race->release_worker);
        race->observed_cancel = radios_cancelled(cancel);
    }
    g_mutex_unlock(&race->mutex);
    return g_strdup("Race work finished");
}

static void
race_destroy(gpointer data)
{
    RaceTask *task = data;
    ShutdownRace *race = task->race;

    g_mutex_lock(&race->mutex);
    race->destroyed[task->kind]++;
    if (task->kind == RACE_PENDING)
        race->pending_destroyed = TRUE;
    g_cond_broadcast(&race->changed);
    g_mutex_unlock(&race->mutex);
    g_free(task);
}

static gpointer
race_producer(gpointer data)
{
    ShutdownRace *race = data;
    GError *error = NULL;
    RaceTask *rejected;
    guint64 pending_id;

    pending_id = radios_runtime_submit(race->runtime, race_work,
                                       race_task_new(race, RACE_PENDING),
                                       race_destroy, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(pending_id, >, 0);

    g_mutex_lock(&race->mutex);
    race->pending_id = pending_id;
    race->producer_queued = TRUE;
    g_cond_broadcast(&race->changed);
    /* Shutdown destroys the accepted pending item after stopping admission.
     * That public ownership notification lets this producer make a new submit
     * while the owner is still joining the held running callback. */
    race_wait_locked(race, &race->pending_destroyed);
    g_mutex_unlock(&race->mutex);

    rejected = race_task_new(race, RACE_REJECTED);
    g_assert_cmpuint(radios_runtime_submit(race->runtime, race_work, rejected,
                                          race_destroy, &error), ==, 0);
    g_assert_error(error, RADIO_S_RUNTIME_ERROR, RADIO_S_RUNTIME_ERROR_STOPPED);
    g_clear_error(&error);
    g_mutex_lock(&race->mutex);
    g_assert_cmpuint(race->destroyed[RACE_REJECTED], ==, 0);
    g_mutex_unlock(&race->mutex);
    race_destroy(rejected); /* Rejected data remains owned by this producer. */

    g_mutex_lock(&race->mutex);
    race->producer_rejected = TRUE;
    g_cond_broadcast(&race->changed);
    race_wait_locked(race, &race->consumer_done);
    race->release_worker = TRUE;
    g_cond_broadcast(&race->changed);
    g_mutex_unlock(&race->mutex);
    return NULL;
}

static gpointer
race_consumer(gpointer data)
{
    ShutdownRace *race = data;
    RadioSEvent *event;

    g_mutex_lock(&race->mutex);
    race_wait_locked(race, &race->producer_rejected);
    g_mutex_unlock(&race->mutex);

    /* STARTED must already exist: the work callback entered before these
     * threads were created. No event polling is needed. */
    event = radios_runtime_pop_event(race->runtime);
    g_assert_nonnull(event);
    g_assert_cmpuint(event->task_id, ==, race->running_id);
    g_assert_cmpint(event->kind, ==, RADIO_S_EVENT_STARTED);
    radios_event_free(event);
    g_assert_null(radios_runtime_pop_event(race->runtime));

    g_mutex_lock(&race->mutex);
    race->consumer_done = TRUE;
    g_cond_broadcast(&race->changed);
    g_mutex_unlock(&race->mutex);
    return NULL;
}

static void
test_submit_pop_during_owner_shutdown(void)
{
    ShutdownRace race = { 0 };
    GError *error = NULL;
    GThread *producer;
    GThread *consumer;

    g_mutex_init(&race.mutex);
    g_cond_init(&race.changed);
    race.runtime = radios_runtime_new(2, 1);
    race.running_id = radios_runtime_submit(race.runtime, race_work,
                                            race_task_new(&race, RACE_RUNNING),
                                            race_destroy, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(race.running_id, >, 0);
    g_mutex_lock(&race.mutex);
    race_wait_locked(&race, &race.worker_entered);
    g_mutex_unlock(&race.mutex);

    producer = g_thread_new("test-producer", race_producer, &race);
    consumer = g_thread_new("test-consumer", race_consumer, &race);
    g_mutex_lock(&race.mutex);
    race_wait_locked(&race, &race.producer_queued);
    g_mutex_unlock(&race.mutex);

    radios_runtime_shutdown(race.runtime);
    /* Runtime lifetime is retained until every caller has stopped using it. */
    g_thread_join(producer);
    g_thread_join(consumer);
    g_assert_true(race.producer_rejected);
    g_assert_true(race.consumer_done);
    g_assert_true(race.observed_cancel);
    g_assert_cmpuint(race.pending_id, >, race.running_id);
    g_assert_cmpuint(race.calls[RACE_RUNNING], ==, 1);
    g_assert_cmpuint(race.calls[RACE_PENDING], ==, 0);
    g_assert_cmpuint(race.calls[RACE_REJECTED], ==, 0);
    for (guint i = 0; i < RACE_TASK_COUNT; i++)
        g_assert_cmpuint(race.destroyed[i], ==, 1);
    radios_runtime_free(race.runtime);
    g_cond_clear(&race.changed);
    g_mutex_clear(&race.mutex);
}

static void
test_priority_readiness_preserves_fifo_capacity(void)
{
    TestState blocked, queued; state_init(&blocked); state_init(&queued);
    RadioSRuntime *runtime = radios_runtime_new(4, 16); GError *error = NULL;
    guint64 running = radios_runtime_submit(runtime, test_work,
        task_new(&blocked, TEST_WORK_BLOCKED), test_destroy, &error);
    g_assert_no_error(error); state_wait(&blocked, FALSE);
    guint64 normal_a = radios_runtime_submit(runtime, test_work,
        task_new(&queued, TEST_WORK_IMMEDIATE), test_destroy, &error);
    guint64 ready_a = radios_runtime_submit_priority(runtime, test_work,
        task_new(&queued, TEST_WORK_IMMEDIATE), test_destroy, &error);
    guint64 normal_b = radios_runtime_submit(runtime, test_work,
        task_new(&queued, TEST_WORK_IMMEDIATE), test_destroy, &error);
    guint64 ready_b = radios_runtime_submit_priority(runtime, test_work,
        task_new(&queued, TEST_WORK_IMMEDIATE), test_destroy, &error);
    g_assert_no_error(error);
    TestTask *rejected = task_new(&queued, TEST_WORK_IMMEDIATE);
    g_assert_cmpuint(radios_runtime_submit_priority(runtime, test_work, rejected,
        test_destroy, &error), ==, 0);
    g_assert_error(error, RADIO_S_RUNTIME_ERROR, RADIO_S_RUNTIME_ERROR_FULL);
    g_clear_error(&error); test_destroy(rejected);
    g_mutex_lock(&blocked.mutex); blocked.release = TRUE;
    g_cond_broadcast(&blocked.changed); g_mutex_unlock(&blocked.mutex);
    guint64 order[] = {running, ready_a, ready_b, normal_a, normal_b};
    for (guint i = 0; i < G_N_ELEMENTS(order); ++i) {
        expect_event(runtime, order[i], RADIO_S_EVENT_STARTED, "");
        expect_event(runtime, order[i], RADIO_S_EVENT_COMPLETED, "Real work completed");
    }
    radios_runtime_free(runtime);
    g_assert_cmpuint(blocked.calls, ==, 1); g_assert_cmpuint(blocked.destroyed, ==, 1);
    g_assert_cmpuint(queued.calls, ==, 4); g_assert_cmpuint(queued.destroyed, ==, 5);
    state_clear(&queued); state_clear(&blocked);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/runtime/execution", test_execution);
    g_test_add_func("/runtime/readiness-priority-fifo-capacity", test_priority_readiness_preserves_fifo_capacity);
    g_test_add_func("/runtime/failure", test_failure);
    g_test_add_func("/runtime/capacity-and-ownership", test_capacity_and_ownership);
    g_test_add_func("/runtime/event-backpressure", test_event_backpressure);
    g_test_add_func("/runtime/cancel-and-pending-cleanup", test_cancel_and_pending_cleanup);
    g_test_add_func("/runtime/shutdown-full-event-queue", test_shutdown_full_event_queue);
    g_test_add_func("/runtime/idle-shutdown-and-invalid-work", test_idle_shutdown_and_invalid_work);
    g_test_add_func("/runtime/submit-pop-during-owner-shutdown", test_submit_pop_during_owner_shutdown);
    return g_test_run();
}
