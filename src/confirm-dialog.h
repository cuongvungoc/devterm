#pragma once
#include <gtk/gtk.h>

typedef void (*ConfirmResponse)(gboolean accepted, gpointer data);
/* The returned window is borrowed until response. Destroying it is Cancel;
 * response runs once, followed by destroy_data. Cancel is the default button. */
GtkWidget *confirm_dialog_show(GtkWindow *parent, const char *title, const char *detail,
    const char *accept_label, ConfirmResponse response, gpointer data, GDestroyNotify destroy_data);
