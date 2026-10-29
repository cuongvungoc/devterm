#pragma once
#include <gtk/gtk.h>
#include <vte/vte.h>

typedef struct TerminalPane TerminalPane;
typedef void (*PaneEvent)(TerminalPane *pane, const char *message, gpointer user_data);

TerminalPane *pane_new(PaneEvent selected, PaneEvent status, gpointer user_data);
GtkWidget *pane_widget(TerminalPane *pane);
void pane_set_active(TerminalPane *pane, gboolean active);
void pane_focus(TerminalPane *pane);
gboolean pane_is_running(TerminalPane *pane);
void pane_send(TerminalPane *pane, const char *command, gboolean send_enter);
void pane_close(TerminalPane *pane);
