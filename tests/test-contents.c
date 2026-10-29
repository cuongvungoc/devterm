#include "../src/pane.h"
#include "../src/text-export.h"
#include <glib/gstdio.h>
#include <string.h>

static GtkWidget *window, *host;
static void iterate(void) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
}
static void settle(void) {
    gint64 deadline = g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < deadline) iterate();
}
static VteTerminal *terminal(TerminalPane *pane) {
    for (GtkWidget *child = gtk_widget_get_first_child(pane_widget(pane)); child; child = gtk_widget_get_next_sibling(child))
        if (VTE_IS_TERMINAL(child)) return VTE_TERMINAL(child);
    g_assert_not_reached();
}
static TerminalPane *new_pane(void) {
    TerminalPane *pane = pane_new(NULL, NULL, NULL, NULL);
    gtk_box_append(GTK_BOX(host), pane_widget(pane));
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!pane_is_running(pane) && g_get_monotonic_time() < deadline) iterate();
    g_assert_true(pane_is_running(pane)); settle();
    vte_terminal_reset(terminal(pane), TRUE, TRUE);
    return pane;
}
static void close_pane(TerminalPane *pane) {
    gtk_window_set_focus(GTK_WINDOW(window), NULL);
    gtk_box_remove(GTK_BOX(host), pane_widget(pane)); pane_close(pane);
}
static char *snapshot(TerminalPane *pane) {
    GError *error = NULL;
    GBytes *text = pane_snapshot_text(pane, &error);
    g_assert_no_error(error); g_assert_nonnull(text);
    gsize size;
    const char *data = g_bytes_get_data(text, &size);
    char *string = size ? g_strndup(data, size) : g_strdup("");
    g_assert_true(g_utf8_validate(string, -1, NULL));
    g_bytes_unref(text); return string;
}
static void feed(TerminalPane *pane, const char *output, const char *marker) {
    vte_terminal_feed(terminal(pane), output, -1);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    char *text;
    do {
        iterate(); text = snapshot(pane);
        gboolean ready = strstr(text, marker) != NULL;
        g_free(text); if (ready) return;
    } while (g_get_monotonic_time() < deadline);
    g_error("VTE did not process marker: %s", marker);
}
typedef struct { gboolean done; char *text; } ClipboardResult;
static void clipboard_ready(GObject *source, GAsyncResult *result, gpointer data) {
    ClipboardResult *out = data; GError *error = NULL;
    out->text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
    g_assert_no_error(error); out->done = TRUE;
}
static void test_scrollback_clipboard(void) {
    TerminalPane *pane = new_pane();
    GString *output = g_string_new("first-retained-marker\r\n\033[31mĐƯỜNG DẪN 日本語\033[0m\r\n");
    for (int i = 0; i < 500; i++) g_string_append_printf(output, "line %d\r\n", i);
    g_string_append(output, "last-retained-marker");
    feed(pane, output->str, "last-retained-marker");
    GtkAdjustment *scroll = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(terminal(pane)));
    gtk_adjustment_set_value(scroll, gtk_adjustment_get_lower(scroll)); settle();
    double scroll_position = gtk_adjustment_get_value(scroll);
    vte_terminal_select_all(terminal(pane));
    char *selected = vte_terminal_get_text_selected(terminal(pane), VTE_FORMAT_TEXT);
    char *all = snapshot(pane);
    g_assert_nonnull(strstr(all, "first-retained-marker\nĐƯỜNG DẪN 日本語\n"));
    g_assert_nonnull(strstr(all, "last-retained-marker"));
    g_assert_null(strchr(all, '\033')); g_assert_null(strchr(all, '\r'));
    GError *error = NULL;
    g_assert_true(pane_copy_all_text(pane, &error)); g_assert_no_error(error);
    char *still_selected = vte_terminal_get_text_selected(terminal(pane), VTE_FORMAT_TEXT);
    g_assert_cmpstr(selected, ==, still_selected);
    g_assert_cmpfloat(gtk_adjustment_get_value(scroll), ==, scroll_position);
    ClipboardResult clipboard = {0};
    gdk_clipboard_read_text_async(gtk_widget_get_clipboard(pane_widget(pane)), NULL, clipboard_ready, &clipboard);
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    while (!clipboard.done && g_get_monotonic_time() < deadline) iterate();
    g_assert_true(clipboard.done); g_assert_cmpstr(clipboard.text, ==, all);
    g_free(clipboard.text); g_free(still_selected); g_free(selected); g_free(all); g_string_free(output, TRUE);
    /* Content evicted by the scrollback limit must not reappear in exports. */
    output = g_string_new(NULL);
    for (int i = 0; i < 11000; i++) g_string_append_printf(output, "new output %d %064d\r\n", i, i);
    g_string_append(output, "end-of-limit-test");
    feed(pane, output->str, "end-of-limit-test");
    gint64 start = g_get_monotonic_time();
    all = snapshot(pane);
    g_test_message("Snapshot of %zu bytes at the scrollback limit: %.3f ms", strlen(all),
                   (g_get_monotonic_time() - start) / 1000.0);
    g_assert_null(strstr(all, "first-retained-marker"));
    g_assert_nonnull(strstr(all, "new output 10999 "));
    g_assert_nonnull(strstr(all, "\nend-of-limit-test"));
    g_free(all); g_string_free(output, TRUE); close_pane(pane);
}
static void test_wrapping_resize(void) {
    TerminalPane *pane = new_pane();
    GString *line = g_string_new("long-line:");
    for (int i = 0; i < 160; i++) g_string_append(line, "đường/文件 ");
    GString *output = g_string_new(line->str);
    g_string_append(output, "\r\nexplicit-newline\r\nlast-line");
    feed(pane, output->str, "last-line");
    char *before = snapshot(pane);
    g_assert_nonnull(strstr(before, line->str));
    g_assert_nonnull(strstr(before, "\nexplicit-newline\nlast-line"));
    gtk_window_set_default_size(GTK_WINDOW(window), 600, 500); settle();
    char *after = snapshot(pane);
    g_assert_cmpstr(before, ==, after);
    g_free(before); g_free(after); g_string_free(line, TRUE); g_string_free(output, TRUE);
    close_pane(pane);
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 600); settle();
}
typedef struct { gboolean done; GError *error; } ExportResult;
static void exported(GObject *source, GAsyncResult *result, gpointer data) {
    ExportResult *out = data;
    g_assert_true(text_export_finish(G_FILE(source), result, &out->error));
    out->done = TRUE;
}
static void test_snapshot_lifetime_and_alternate(void) {
    TerminalPane *first = new_pane(), *second = new_pane();
    feed(first, "first-pane-only\r\n", "first-pane-only");
    feed(second, "second-pane-only\r\n", "second-pane-only");
    GError *error = NULL;
    GBytes *frozen = pane_snapshot_text(first, &error); g_assert_no_error(error);
    char *text = snapshot(second);
    g_assert_null(strstr(text, "first-pane-only")); g_free(text);
    feed(first, "later-output\r\n", "later-output");
    close_pane(first);
    char *dir = g_dir_make_tmp("devterm-snapshot-XXXXXX", &error); g_assert_no_error(error);
    char *path = g_build_filename(dir, "closed-pane.txt", NULL);
    GFile *file = g_file_new_for_path(path);
    ExportResult result = {0};
    text_export_async(file, frozen, NULL, exported, &result);
    g_bytes_unref(frozen); g_object_unref(file);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!result.done && g_get_monotonic_time() < deadline) iterate();
    g_assert_true(result.done); g_assert_no_error(result.error);
    g_assert_true(g_file_get_contents(path, &text, NULL, &error)); g_assert_no_error(error);
    g_assert_nonnull(strstr(text, "first-pane-only"));
    g_assert_null(strstr(text, "second-pane-only")); g_assert_null(strstr(text, "later-output"));
    g_free(text); g_unlink(path); g_rmdir(dir); g_free(path); g_free(dir);
    feed(second, "\033[?1049h\033[2J\033[HALTERNATE-SCREEN", "ALTERNATE-SCREEN");
    text = snapshot(second);
    g_assert_null(strstr(text, "second-pane-only")); g_free(text);
    feed(second, "\033[?1049l", "second-pane-only");
    text = snapshot(second); g_assert_null(strstr(text, "ALTERNATE-SCREEN")); g_free(text);
    pane_send(second, "exit", TRUE);
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (pane_is_running(second) && g_get_monotonic_time() < deadline) iterate();
    g_assert_false(pane_is_running(second));
    text = snapshot(second); g_assert_nonnull(strstr(text, "second-pane-only")); g_free(text);
    close_pane(second);
    g_assert_null(pane_snapshot_text(NULL, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED); g_clear_error(&error);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check()) return 77;
    window = gtk_window_new(); gtk_window_set_default_size(GTK_WINDOW(window), 1000, 600);
    host = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_window_set_child(GTK_WINDOW(window), host); gtk_window_present(GTK_WINDOW(window));
    g_test_add_func("/contents/scrollback-clipboard", test_scrollback_clipboard);
    g_test_add_func("/contents/wrapping-resize", test_wrapping_resize);
    g_test_add_func("/contents/snapshot-lifetime-alternate", test_snapshot_lifetime_and_alternate);
    int result = g_test_run(); gtk_window_destroy(GTK_WINDOW(window)); return result;
}
