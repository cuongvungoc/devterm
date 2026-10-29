#pragma once
#include <gio/gio.h>

/* Owns references to the immutable snapshot and destination until completion. */
void text_export_async(GFile *file, GBytes *text, GCancellable *cancellable,
                       GAsyncReadyCallback callback, gpointer user_data);
gboolean text_export_finish(GFile *file, GAsyncResult *result, GError **error);
