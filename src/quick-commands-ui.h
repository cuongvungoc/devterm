#pragma once
#include <gtk/gtk.h>
#include "commands.h"

typedef struct QuickCommandsUI QuickCommandsUI;
typedef void (*QuickCommandsRun)(const QuickCommand *command, gpointer data);
typedef void (*QuickCommandsStatus)(gpointer data, const char *message);
QuickCommandsUI *quick_commands_ui_new(GtkWindow *parent, GtkFlowBox *bar, GtkScrolledWindow *scroll,
    GtkWidget *add, GtkWidget *manage, QuickCommandsRun run, QuickCommandsStatus status, gpointer data);
const char *quick_commands_ui_load_error(QuickCommandsUI *ui);
/* Close dialogs/cancel drags before freeing the parent application state. */
void quick_commands_ui_free(QuickCommandsUI *ui);
