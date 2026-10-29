#pragma once
#include <gtk/gtk.h>
#include <vte/vte.h>
#include "sessions.h"

typedef struct TerminalPane TerminalPane;
typedef void (*PaneEvent)(TerminalPane *pane, const char *message, gpointer user_data);

TerminalPane *pane_new(const char *working_directory, PaneEvent selected, PaneEvent status, gpointer user_data);
TerminalPane *pane_new_session(const SessionProfile *profile, const char *password, PaneEvent selected, PaneEvent status, gpointer user_data);
const SessionProfile *pane_session_profile(TerminalPane *pane);
void pane_disconnect(TerminalPane *pane);
GtkWidget *pane_widget(TerminalPane *pane);
void pane_set_active(TerminalPane *pane, gboolean active);
void pane_focus(TerminalPane *pane);
void pane_search_open(TerminalPane *pane);
/* Snapshot the current screen buffer, including retained scrollback. GTK thread only. */
GBytes *pane_snapshot_text(TerminalPane *pane, GError **error);
gboolean pane_copy_all_text(TerminalPane *pane, GError **error);
gboolean pane_is_running(TerminalPane *pane);
gboolean pane_has_live_session(TerminalPane *pane);
char *pane_dup_working_directory(TerminalPane *pane);
void pane_send(TerminalPane *pane, const char *command, gboolean send_enter);
void pane_close(TerminalPane *pane);
