/* Compile the genuine facade with a controllable deadline clock only in
 * this acceptance target. No production test hooks or audio clock changes. */
#include <glib.h>
gint64 radios_test_execution_clock_now(void);
#define g_get_monotonic_time radios_test_execution_clock_now
#include "../src/ui/player_scope_execution.c"
