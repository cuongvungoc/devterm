#include "../src/split-layout.h"
#include <glib/gstdio.h>

static void iterate(void) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
}
static void wait_running(TerminalPane *pane) {
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!pane_is_running(pane) && g_get_monotonic_time() < deadline) iterate();
    g_assert_true(pane_is_running(pane));
}
static void test_directory_and_isolation(void) {
    GError *error = NULL;
    char *directory = g_dir_make_tmp("devterm-layout-XXXXXX", &error);
    g_assert_no_error(error);
    char *subdirectory = g_build_filename(directory, "local directory", NULL);
    char *marker = g_build_filename(directory, "ready", NULL);
    g_assert_cmpint(g_mkdir(subdirectory, 0700), ==, 0);
    SplitLayout *first = split_layout_new(directory, NULL, NULL);
    SplitLayout *second = split_layout_new(g_get_home_dir(), NULL, NULL);
    TerminalPane *first_pane = split_layout_active(first);
    wait_running(first_pane);
    wait_running(split_layout_active(second));
    char *quoted_dir = g_shell_quote(subdirectory), *quoted_marker = g_shell_quote(marker);
    char *command = g_strdup_printf("printf '\\033]7;file://remote.example/etc\\007'; cd %s; printf ready > %s", quoted_dir, quoted_marker);
    pane_send(first_pane, command, TRUE);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!g_file_test(marker, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < deadline) iterate();
    g_assert_true(g_file_test(marker, G_FILE_TEST_EXISTS));
    char *cwd = pane_dup_working_directory(first_pane);
    g_assert_cmpstr(cwd, ==, subdirectory);
    SplitLayout *inherited = split_layout_new(cwd, NULL, NULL);
    wait_running(split_layout_active(inherited));
    char *inherited_cwd = pane_dup_working_directory(split_layout_active(inherited));
    g_assert_cmpstr(inherited_cwd, ==, subdirectory);
    split_layout_split(first, GTK_ORIENTATION_HORIZONTAL);
    wait_running(split_layout_active(first));
    g_assert_cmpuint(split_layout_pane_count(first), ==, 2);
    g_assert_cmpuint(split_layout_pane_count(second), ==, 1);
    char *other_cwd = pane_dup_working_directory(split_layout_active(second));
    g_assert_cmpstr(other_cwd, ==, g_get_home_dir());
    split_layout_close_active(first);
    g_assert_true(split_layout_active(first) == first_pane);
    g_assert_cmpuint(split_layout_live_sessions(second), ==, 1);
    TerminalPane *exiting = split_layout_active(second);
    pane_send(exiting, "exit", TRUE);
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (pane_is_running(exiting) && g_get_monotonic_time() < deadline) iterate();
    g_assert_false(pane_has_live_session(exiting));
    char *fallback = pane_dup_working_directory(exiting);
    g_assert_cmpstr(fallback, ==, g_get_home_dir());
    g_free(fallback);
    split_layout_free(first); split_layout_free(second); split_layout_free(inherited);
    g_free(cwd); g_free(inherited_cwd); g_free(other_cwd);
    g_free(quoted_dir); g_free(quoted_marker); g_free(command);
    g_remove(marker); g_rmdir(subdirectory); g_rmdir(directory);
    g_free(marker); g_free(subdirectory); g_free(directory);
}
static void test_close_during_spawn(void) {
    SplitLayout *layout = split_layout_new(NULL, NULL, NULL);
    gpointer first_widget = pane_widget(split_layout_active(layout));
    g_object_add_weak_pointer(G_OBJECT(first_widget), &first_widget);
    split_layout_split(layout, GTK_ORIENTATION_VERTICAL);
    gpointer second_widget = pane_widget(split_layout_active(layout));
    g_object_add_weak_pointer(G_OBJECT(second_widget), &second_widget);
    g_assert_cmpuint(split_layout_live_sessions(layout), ==, 2);
    split_layout_free(layout);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while ((first_widget || second_widget) && g_get_monotonic_time() < deadline) iterate();
    g_assert_null(first_widget);
    g_assert_null(second_widget);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check()) return 77;
    g_test_add_func("/layout/directory-and-isolation", test_directory_and_isolation);
    g_test_add_func("/layout/close-during-spawn", test_close_during_spawn);
    return g_test_run();
}
