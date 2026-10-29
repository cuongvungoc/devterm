#pragma once
#include <gtk/gtk.h>
#include "sessions.h"
typedef struct SessionManager SessionManager;
typedef void (*SessionConnect)(const SessionProfile *profile, const char *password, gpointer data);
typedef void (*SessionCurrentAction)(gboolean reconnect, gpointer data);
SessionManager *session_manager_new(GtkWindow *parent, SessionConnect connect, SessionCurrentAction current, gpointer data);
void session_manager_show(SessionManager *manager);
void session_manager_connect(SessionManager *manager, const SessionProfile *profile);
void session_manager_free(SessionManager *manager);
