#include "../src/command-sequence.h"
#include <glib/gstdio.h>
#include <string.h>

static GtkWindow *window;
static GtkWidget *host;
static char *root;
static guint ticks, sent;
static gint64 times[8];
static void iterate(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void wait_ms(guint ms) {
    gint64 end = g_get_monotonic_time() + ms * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < end) iterate();
}
static gboolean pulse(gpointer data) { (void)data; ticks++; return G_SOURCE_CONTINUE; }
static void status(TerminalPane *pane, const char *message, gpointer data) {
    (void)pane; (void)data;
    if (strstr(message, "commands sent") && sent < G_N_ELEMENTS(times)) times[sent++] = g_get_monotonic_time();
}
static TerminalPane *new_pane(void) {
    TerminalPane *pane = pane_new(root, NULL, status, NULL); gtk_box_append(GTK_BOX(host), pane_widget(pane));
    gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!pane_is_running(pane) && g_get_monotonic_time() < end) iterate();
    g_assert_true(pane_is_running(pane)); wait_ms(100); return pane;
}
static void close_pane(TerminalPane *pane) {
    gtk_window_set_focus(window, NULL); gtk_box_remove(GTK_BOX(host), pane_widget(pane)); pane_close(pane);
}
static QuickCommand *sequence(const char *first, guint delay, const char *second) {
    QuickCommand *cmd = quick_sequence_new("sequence", "Test Unicode 日本語");
    g_ptr_array_add(cmd->steps, quick_command_step_new(first, delay));
    g_ptr_array_add(cmd->steps, quick_command_step_new(second, 60000)); return cmd;
}
static char *read_file(const char *name) {
    char *path = g_build_filename(root, name, NULL), *text = NULL;
    g_file_get_contents(path, &text, NULL, NULL); g_free(path); return text;
}
static void wait_file(const char *name, const char *expected) {
    gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < end) {
        iterate(); char *text = read_file(name); gboolean done = g_strcmp0(text, expected) == 0;
        g_free(text); if (done) return;
    }
    g_error("Expected file %s to contain %s", name, expected);
}
static void wait_done(CommandSequences *runs, TerminalPane *pane) {
    gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (command_sequences_busy(runs, pane) && g_get_monotonic_time() < end) iterate();
    g_assert_false(command_sequences_busy(runs, pane));
}
static void test_order_delay_snapshot(void) {
    TerminalPane *pane = new_pane(); CommandSequences *runs = command_sequences_new();
    QuickCommand *cmd = sequence("printf '%s\\n' \"Đường ' | && >\" > order", 250, "printf '%s\\n' second >> order");
    ((QuickCommandStep *)cmd->steps->pdata[1])->delay_ms = 350;
    g_ptr_array_add(cmd->steps, quick_command_step_new("printf '%s\\n' third >> order", 60000));
    sent = ticks = 0; guint timer = g_timeout_add(10, pulse, NULL); gint64 begin = g_get_monotonic_time();
    g_assert_true(command_sequences_send(runs, pane, cmd, NULL));
    g_assert_cmpuint(sent, ==, 1); g_assert_true(command_sequences_busy(runs, pane));
    /* Editing/deleting the saved definition must not change an in-flight run. */
    g_free(((QuickCommandStep *)cmd->steps->pdata[1])->command);
    ((QuickCommandStep *)cmd->steps->pdata[1])->command = g_strdup("BAD MUTATED INPUT"); quick_command_free(cmd);
    wait_done(runs, pane); g_source_remove(timer);
    g_assert_cmpuint(sent, ==, 3); g_assert_cmpuint(ticks, >, 10);
    g_assert_cmpint(times[1] - times[0], >=, 245 * G_TIME_SPAN_MILLISECOND);
    g_assert_cmpint(times[2] - times[1], >=, 345 * G_TIME_SPAN_MILLISECOND);
    g_assert_cmpint(g_get_monotonic_time() - begin, <, 5 * G_TIME_SPAN_SECOND);
    wait_file("order", "Đường ' | && >\nsecond\nthird\n");
    command_sequences_free(runs); close_pane(pane);
}
static void test_target_focus_busy(void) {
    TerminalPane *a = new_pane(), *b = new_pane(); CommandSequences *runs = command_sequences_new();
    QuickCommand *cmd = sequence("printf 'first\\n' > target", 400, "printf 'last\\n' >> target");
    QuickCommand *single = quick_command_new("single", "Single", "printf 'other\\n' > other", TRUE);
    g_assert_true(command_sequences_send(runs, a, cmd, NULL)); GError *error = NULL;
    g_assert_false(command_sequences_send(runs, a, cmd, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error(&error);
    g_assert_false(command_sequences_send(runs, a, single, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error(&error);
    g_assert_true(command_sequences_send(runs, b, single, NULL));
    GtkWidget *entry = gtk_entry_new(); gtk_box_append(GTK_BOX(host), entry); gtk_widget_grab_focus(entry); wait_ms(30);
    GtkWidget *focus = gtk_window_get_focus(window); g_assert_true(focus == entry || gtk_widget_is_ancestor(focus, entry));
    wait_done(runs, a); g_assert_true(gtk_window_get_focus(window) == focus);
    wait_file("target", "first\nlast\n"); wait_file("other", "other\n");
    /* cd changes this same shell, with zero-delay rows sent via the main loop. */
    QuickCommand *cwd = sequence("cd /", 0, "pwd > cwd-result");
    char *out = g_build_filename(root, "cwd", NULL), *quoted = g_shell_quote(out);
    QuickCommandStep *step = cwd->steps->pdata[1]; g_free(step->command); step->command = g_strdup_printf("pwd > %s", quoted);
    g_assert_true(command_sequences_send(runs, a, cwd, NULL)); wait_done(runs, a); wait_file("cwd", "/\n");
    quick_command_free(cwd); g_free(out); g_free(quoted); quick_command_free(cmd); quick_command_free(single);
    command_sequences_free(runs); gtk_box_remove(GTK_BOX(host), entry); close_pane(a); close_pane(b);
}
static void test_exit_close_shutdown(void) {
    CommandSequences *runs = command_sequences_new(); TerminalPane *p = new_pane();
    QuickCommand *cmd = sequence("exit", 400, "printf forbidden > forbidden");
    g_assert_true(command_sequences_send(runs, p, cmd, NULL)); wait_done(runs, p);
    g_assert_false(pane_is_running(p)); wait_ms(450); g_assert_null(read_file("forbidden")); close_pane(p);
    p = new_pane(); g_free(((QuickCommandStep *)cmd->steps->pdata[0])->command);
    ((QuickCommandStep *)cmd->steps->pdata[0])->command = g_strdup(":");
    g_assert_true(command_sequences_send(runs, p, cmd, NULL));
    pane_ref(p); close_pane(p); g_assert_false(command_sequences_busy(runs, p)); pane_unref(p);
    wait_ms(450); g_assert_null(read_file("forbidden"));
    p = new_pane(); g_assert_true(command_sequences_send(runs, p, cmd, NULL)); command_sequences_free(runs);
    wait_ms(450); g_assert_null(read_file("forbidden")); close_pane(p); quick_command_free(cmd);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL); root = g_dir_make_tmp("devterm-sequence-XXXXXX", NULL);
    if (!gtk_init_check()) return 77;
    window = GTK_WINDOW(gtk_window_new()); gtk_window_set_default_size(window, 1000, 700);
    host = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4); gtk_window_set_child(window, host); gtk_window_present(window);
    g_test_add_func("/sequence/order-delay-snapshot", test_order_delay_snapshot);
    g_test_add_func("/sequence/target-focus-busy-cd", test_target_focus_busy);
    g_test_add_func("/sequence/exit-close-shutdown", test_exit_close_shutdown);
    int result = g_test_run(); gtk_window_destroy(window); wait_ms(200);
    GDir *dir = g_dir_open(root, 0, NULL); const char *name;
    while ((name = g_dir_read_name(dir))) { char *path = g_build_filename(root, name, NULL); g_remove(path); g_free(path); }
    g_dir_close(dir); g_rmdir(root); g_free(root); return result;
}
