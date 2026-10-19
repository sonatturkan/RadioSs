#ifndef RADIOS_AUTOMATION_WORKSPACE_UI_H
#define RADIOS_AUTOMATION_WORKSPACE_UI_H

#include "radios/views.h"
#include "radios/automation_scope.h"

typedef void (*RadioSAutomationWorkspaceSelected)(const RadioSAutomationScope *scope, gpointer user);
GtkWidget *radios_automation_workspace_ui_new(RadioSViewContext *ctx,
    RadioSAutomationWorkspaceSelected selected, gpointer user);
/* Programmatic selection refreshes this panel without forwarding selected. */
gboolean radios_automation_workspace_ui_select(GtkWidget *widget, const RadioSAutomationScope *scope);
void radios_automation_workspace_ui_refresh(GtkWidget *widget);
void radios_automation_workspace_ui_set_edit_hook(GtkWidget *widget,
    RadioSAutomationWorkspaceSelected edit, gpointer user);

#endif
