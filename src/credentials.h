#pragma once
#include "sessions.h"
void credential_lookup(const SessionProfile *profile, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data);
char *credential_lookup_finish(GAsyncResult *result, GError **error);
void credential_store(const SessionProfile *profile, const char *password, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data);
void credential_clear(const SessionProfile *profile, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data);
gboolean credential_change_finish(GAsyncResult *result, GError **error);
