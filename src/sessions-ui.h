#pragma once
#include <gtk/gtk.h>
#include "sessions.h"
typedef struct SessionManager SessionManager;
typedef void (*SessionConnect)(const SessionProfile *profile, const char *password, gpointer data);
typedef void (*SessionCurrentAction)(gboolean reconnect, gpointer data);
SessionManager *session_manager_new(GtkWindow *parent, SessionConnect connect, SessionCurrentAction current, gpointer data);
void session_manager_show(SessionManager *manager);
void session_manager_connect(SessionManager *manager, const SessionProfile *profile);
/* Resolve credentials for a specific destination. destroy runs on success,
 * cancellation or shutdown; callback receives the password only on success.
 * Called/cancelled on the GTK thread. Does not open a tab on its own. */
void session_manager_connect_to(SessionManager *manager, const SessionProfile *profile,
    GCancellable *cancel, SessionConnect callback, gpointer data, GDestroyNotify destroy);
void session_manager_free(SessionManager *manager);
