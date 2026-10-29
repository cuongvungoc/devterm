#include "../src/text-export.h"
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

typedef struct { gboolean done, ok; GError *error; } Result;
static void completed(GObject *source, GAsyncResult *result, gpointer data) {
    Result *out = data;
    out->ok = text_export_finish(G_FILE(source), result, &out->error);
    out->done = TRUE;
}
static gboolean save(const char *path, GBytes *text, GCancellable *cancel, GError **error) {
    GFile *file = g_file_new_for_commandline_arg(path);
    Result out = {0};
    text_export_async(file, text, cancel, completed, &out);
    g_object_unref(file);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!out.done && g_get_monotonic_time() < deadline) {
        g_main_context_iteration(NULL, FALSE); g_usleep(1000);
    }
    g_assert_true(out.done);
    if (out.error) g_propagate_error(error, out.error);
    return out.ok;
}
static char *new_directory(void) {
    GError *error = NULL;
    char *dir = g_dir_make_tmp("devterm-export-XXXXXX", &error);
    g_assert_no_error(error); return dir;
}
static void assert_file(const char *path, const char *expected) {
    char *actual = NULL; GError *error = NULL;
    g_assert_true(g_file_get_contents(path, &actual, NULL, &error));
    g_assert_no_error(error); g_assert_cmpstr(actual, ==, expected); g_free(actual);
}
static void remove_directory(char *dir) {
    GDir *entries = g_dir_open(dir, 0, NULL);
    g_assert_nonnull(entries);
    /* Every expected output has been removed: no temporary file may remain. */
    g_assert_null(g_dir_read_name(entries));
    g_dir_close(entries); g_assert_cmpint(g_rmdir(dir), ==, 0); g_free(dir);
}
static void test_atomic_unicode(void) {
    char *dir = new_directory();
    char *path = g_build_filename(dir, "Nhật ký terminal.txt", NULL);
    char *old_link = g_build_filename(dir, "previous", NULL);
    g_assert_true(g_file_set_contents(path, "old contents", -1, NULL));
    g_assert_cmpint(link(path, old_link), ==, 0);
    const char *content = "Đường dẫn /tmp/a b\n'quote' | && >\n日本語\n";
    GBytes *text = g_bytes_new(content, strlen(content));
    GError *error = NULL;
    g_assert_true(save(path, text, NULL, &error)); g_assert_no_error(error);
    assert_file(path, content);
    /* An in-place truncating write would also change this hard link. */
    assert_file(old_link, "old contents");
    GStatBuf statbuf; g_assert_cmpint(g_stat(path, &statbuf), ==, 0);
    g_assert_cmpint(statbuf.st_mode & 0777, ==, 0600);
    g_bytes_unref(text);
    text = g_bytes_new_static("", 0);
    g_assert_true(save(path, text, NULL, &error)); g_assert_no_error(error);
    assert_file(path, ""); assert_file(old_link, "old contents");
    g_bytes_unref(text); g_unlink(path); g_unlink(old_link);
    g_free(path); g_free(old_link); remove_directory(dir);
}
static void test_cancelled(void) {
    char *dir = new_directory(), *path = g_build_filename(dir, "existing.txt", NULL);
    GBytes *text = g_bytes_new_static("new", 3);
    GCancellable *cancel = g_cancellable_new(); g_cancellable_cancel(cancel);
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, "keep", -1, NULL));
    g_assert_false(save(path, text, cancel, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&error);
    assert_file(path, "keep"); g_unlink(path);
    g_assert_false(save(path, text, cancel, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&error);
    g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
    g_object_unref(cancel); g_bytes_unref(text); g_free(path); remove_directory(dir);
}
static void test_commit_failure(void) {
    char *dir = new_directory(), *path = g_build_filename(dir, "directory.txt", NULL);
    g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
    GBytes *text = g_bytes_new_static("text", 4); GError *error = NULL;
    g_assert_false(save(path, text, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_true(g_file_test(path, G_FILE_TEST_IS_DIR));
    g_rmdir(path); g_bytes_unref(text); g_free(path); remove_directory(dir);
}
static void test_unwritable_directory(void) {
    if (getuid() == 0) { g_test_skip("Permission checks require a non-root user"); return; }
    char *dir = new_directory(), *path = g_build_filename(dir, "existing.txt", NULL);
    g_assert_true(g_file_set_contents(path, "keep", -1, NULL));
    g_assert_cmpint(g_chmod(dir, 0500), ==, 0);
    GBytes *text = g_bytes_new_static("replace", 7); GError *error = NULL;
    g_assert_false(save(path, text, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED); g_clear_error(&error);
    assert_file(path, "keep");
    g_chmod(dir, 0700); g_unlink(path); g_bytes_unref(text); g_free(path); remove_directory(dir);
}
static void test_nonlocal_destination(void) {
    GBytes *text = g_bytes_new_static("text", 4); GError *error = NULL;
    g_assert_false(save("sftp://example.invalid/export.txt", text, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_clear_error(&error); g_bytes_unref(text);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/export/atomic-unicode", test_atomic_unicode);
    g_test_add_func("/export/cancelled", test_cancelled);
    g_test_add_func("/export/commit-failure", test_commit_failure);
    g_test_add_func("/export/unwritable-directory", test_unwritable_directory);
    g_test_add_func("/export/nonlocal-destination", test_nonlocal_destination);
    return g_test_run();
}
