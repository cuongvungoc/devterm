#include "text-export.h"
#include <errno.h>
#include <glib/gstdio.h>
#include <unistd.h>

static gboolean write_snapshot(const char *path, GBytes *text, GCancellable *cancel, GError **error) {
    if (g_cancellable_set_error_if_cancelled(cancel, error)) return FALSE;
    char *directory = g_path_get_dirname(path);
    char *temporary = g_build_filename(directory, ".devterm-export.XXXXXX", NULL);
    g_free(directory);
    int fd = g_mkstemp(temporary);
    gboolean ok = fd >= 0;
    int saved_errno = errno;
    if (ok) {
        gsize size, offset = 0;
        const char *data = g_bytes_get_data(text, &size);
        while (offset < size) {
            if (g_cancellable_set_error_if_cancelled(cancel, error)) { ok = FALSE; break; }
            ssize_t written = write(fd, data + offset, MIN(size - offset, (gsize)65536));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { saved_errno = written == 0 ? EIO : errno; ok = FALSE; break; }
            offset += (gsize)written;
        }
        if (ok && fsync(fd) != 0) { saved_errno = errno; ok = FALSE; }
        if (close(fd) != 0 && ok) { saved_errno = errno; ok = FALSE; }
        if (ok && g_cancellable_set_error_if_cancelled(cancel, error)) ok = FALSE;
        /* Never fall back to truncating the destination. Until this rename
         * succeeds, a failed/cancelled export leaves the old file untouched. */
        if (ok && g_rename(temporary, path) != 0) { saved_errno = errno; ok = FALSE; }
    }
    if (!ok) {
        if (!error || !*error)
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno), "Cannot save %s: %s", path, g_strerror(saved_errno));
        if (fd >= 0) g_unlink(temporary);
    }
    g_free(temporary);
    return ok;
}
static void export_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel) {
    char *path = g_file_get_path(G_FILE(source));
    if (!path) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Choose a local folder to export terminal text");
        return;
    }
    GError *error = NULL;
    gboolean ok = write_snapshot(path, task_data, cancel, &error);
    g_free(path);
    if (ok) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, error);
}
void text_export_async(GFile *file, GBytes *text, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
    GTask *task = g_task_new(file, cancel, callback, data);
    g_task_set_source_tag(task, text_export_async);
    /* Cancellation is checked before commit. A late cancellation must not
     * report failure after the destination has already been replaced. */
    g_task_set_check_cancellable(task, FALSE);
    g_task_set_task_data(task, g_bytes_ref(text), (GDestroyNotify)g_bytes_unref);
    g_task_run_in_thread(task, export_worker);
    g_object_unref(task);
}
gboolean text_export_finish(GFile *file, GAsyncResult *result, GError **error) {
    g_return_val_if_fail(g_task_is_valid(result, file), FALSE);
    g_return_val_if_fail(g_async_result_is_tagged(result, text_export_async), FALSE);
    return g_task_propagate_boolean(G_TASK(result), error);
}
