#ifndef RADIOS_AUTOMATION_SCOPE_UI_H
#define RADIOS_AUTOMATION_SCOPE_UI_H

#include "radios/data.h"
#include "radios/automation_scope.h"

/* Exact saved schema lookup. The authenticated client and first read revision
 * are captured and checked again before delivery. Revision zero accepts the
 * latest active schema; positive revisions reject updates. callback receives
 * borrowed record data and runs only while owner is alive. This never saves,
 * changes a queue, or starts audio. destroy(user) runs exactly once, including
 * a synchronous FALSE result. Callers must recheck their captured topology
 * binding before applying the result. */
gboolean radios_automation_scope_fetch(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *document_id, gint64 expected_revision,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy);

#endif
