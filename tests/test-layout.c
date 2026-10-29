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
static void settle_layout(void) {
    /* Allow several frame-clock layout passes, including deferred centering. */
    gint64 deadline = g_get_monotonic_time() + 150 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < deadline) iterate();
}
static GtkWidget *present_layout(SplitLayout *layout) {
    GtkWidget *window = gtk_window_new();
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 800);
    gtk_window_set_child(GTK_WINDOW(window), split_layout_widget(layout));
    gtk_window_present(GTK_WINDOW(window));
    settle_layout();
    split_layout_focus(layout);
    return window;
}
static void destroy_layout(GtkWidget *window, SplitLayout *layout) {
    gtk_window_set_focus(GTK_WINDOW(window), NULL);
    split_layout_free(layout);
    gtk_window_destroy(GTK_WINDOW(window));
}
static GtkPaned *root_paned(SplitLayout *layout) {
    GtkWidget *root = gtk_widget_get_first_child(split_layout_widget(layout));
    g_assert_true(GTK_IS_PANED(root));
    return GTK_PANED(root);
}
static int extent(GtkWidget *widget, GtkOrientation orientation) {
    return orientation == GTK_ORIENTATION_HORIZONTAL ? gtk_widget_get_width(widget) : gtk_widget_get_height(widget);
}
static void assert_near(int actual, int expected) { g_assert_cmpint(ABS(actual - expected), <=, 2); }
static void assert_ratio(GtkPaned *paned, double ratio) {
    GtkOrientation orientation = gtk_orientable_get_orientation(GTK_ORIENTABLE(paned));
    int first = extent(gtk_paned_get_start_child(paned), orientation);
    int second = extent(gtk_paned_get_end_child(paned), orientation);
    g_assert_cmpint(first, >, 30); g_assert_cmpint(second, >, 30);
    assert_near(first, (int)((first + second) * ratio + 0.5));
}
static void select_pane(SplitLayout *layout, TerminalPane *pane) {
    pane_focus(pane); settle_layout();
    g_assert_true(split_layout_active(layout) == pane);
}
static void test_repeated_split_geometry(gconstpointer data) {
    GtkOrientation orientation = GPOINTER_TO_INT(data);
    SplitLayout *layout = split_layout_new(NULL, NULL, NULL);
    TerminalPane *first = split_layout_active(layout);
    GtkWidget *window = present_layout(layout);
    split_layout_split(layout, orientation); settle_layout();
    GtkPaned *root = root_paned(layout);
    assert_ratio(root, 0.5);
    int initial_extent = extent(pane_widget(first), orientation);
    TerminalPane *second = split_layout_active(layout);
    split_layout_split(layout, orientation); settle_layout();
    TerminalPane *third = split_layout_active(layout);
    assert_near(extent(pane_widget(first), orientation), initial_extent);
    assert_ratio(root, 0.5);
    GtkPaned *nested = GTK_PANED(gtk_paned_get_end_child(root));
    assert_ratio(nested, 0.5);

    /* Simulate divider positions chosen by the user, then split another leaf. */
    gtk_paned_set_position(root, initial_extent * 2 / 3); settle_layout();
    int nested_extent = extent(GTK_WIDGET(nested), orientation);
    gtk_paned_set_position(nested, nested_extent * 2 / 3); settle_layout();
    int root_position = gtk_paned_get_position(root);
    int second_extent = extent(pane_widget(second), orientation);
    int third_extent = extent(pane_widget(third), orientation);
    select_pane(layout, first);
    split_layout_split(layout, orientation); settle_layout();
    assert_near(gtk_paned_get_position(root), root_position);
    assert_near(extent(pane_widget(second), orientation), second_extent);
    assert_near(extent(pane_widget(third), orientation), third_extent);
    assert_ratio(GTK_PANED(gtk_paned_get_start_child(root)), 0.5);

    double root_ratio = (double)root_position / (root_position + nested_extent);
    double nested_ratio = (double)second_extent / (second_extent + third_extent);
    gtk_window_set_default_size(GTK_WINDOW(window), 1440, 960); settle_layout();
    assert_ratio(root, root_ratio); assert_ratio(nested, nested_ratio);
    assert_ratio(GTK_PANED(gtk_paned_get_start_child(root)), 0.5);
    root_position = gtk_paned_get_position(root);
    second_extent = extent(pane_widget(second), orientation);
    third_extent = extent(pane_widget(third), orientation);
    split_layout_close_active(layout); settle_layout();
    g_assert_true(gtk_paned_get_start_child(root) == pane_widget(first));
    assert_near(gtk_paned_get_position(root), root_position);
    assert_near(extent(pane_widget(second), orientation), second_extent);
    assert_near(extent(pane_widget(third), orientation), third_extent);
    select_pane(layout, second);
    split_layout_close_active(layout); settle_layout();
    g_assert_true(gtk_paned_get_end_child(root) == pane_widget(third));
    assert_near(gtk_paned_get_position(root), root_position);
    destroy_layout(window, layout);
}
static void test_mixed_split_geometry(void) {
    SplitLayout *layout = split_layout_new(NULL, NULL, NULL);
    TerminalPane *first = split_layout_active(layout);
    GtkWidget *window = present_layout(layout);
    split_layout_split(layout, GTK_ORIENTATION_HORIZONTAL); settle_layout();
    TerminalPane *second = split_layout_active(layout);
    split_layout_split(layout, GTK_ORIENTATION_VERTICAL); settle_layout();
    GtkPaned *root = root_paned(layout);
    GtkPaned *right = GTK_PANED(gtk_paned_get_end_child(root));
    assert_ratio(root, 0.5); assert_ratio(right, 0.5);
    select_pane(layout, second);
    split_layout_split(layout, GTK_ORIENTATION_HORIZONTAL); settle_layout();
    assert_ratio(root, 0.5); assert_ratio(right, 0.5);
    assert_ratio(GTK_PANED(gtk_paned_get_start_child(right)), 0.5);
    split_layout_close_active(layout); settle_layout();
    assert_ratio(root, 0.5); assert_ratio(right, 0.5);
    select_pane(layout, first);
    split_layout_split(layout, GTK_ORIENTATION_VERTICAL); settle_layout();
    assert_ratio(root, 0.5); assert_ratio(right, 0.5);
    assert_ratio(GTK_PANED(gtk_paned_get_start_child(root)), 0.5);
    split_layout_close_active(layout); settle_layout();
    select_pane(layout, first);
    /* Promote a surviving split subtree, retaining its internal ratio. */
    split_layout_close_active(layout); settle_layout();
    g_assert_true(root_paned(layout) == right);
    assert_ratio(right, 0.5);
    destroy_layout(window, layout);
}
static void test_deferred_split_geometry(void) {
    SplitLayout *layout = split_layout_new(NULL, NULL, NULL);
    /* Repeated clicks can arrive before the next allocation or before mapping. */
    split_layout_split(layout, GTK_ORIENTATION_HORIZONTAL);
    split_layout_split(layout, GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *window = present_layout(layout);
    GtkPaned *root = root_paned(layout);
    GtkPaned *nested = GTK_PANED(gtk_paned_get_end_child(root));
    assert_ratio(root, 0.5); assert_ratio(nested, 0.5);
    gtk_widget_set_visible(window, FALSE);
    split_layout_split(layout, GTK_ORIENTATION_VERTICAL);
    /* Remove an uninitialized split while hidden, then initialize another. */
    split_layout_close_active(layout);
    split_layout_split(layout, GTK_ORIENTATION_VERTICAL);
    gtk_window_present(GTK_WINDOW(window)); settle_layout();
    assert_ratio(root, 0.5); assert_ratio(nested, 0.5);
    assert_ratio(GTK_PANED(gtk_paned_get_end_child(nested)), 0.5);
    split_layout_split(layout, GTK_ORIENTATION_HORIZONTAL);
    destroy_layout(window, layout);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check()) return 77;
    g_test_add_func("/layout/directory-and-isolation", test_directory_and_isolation);
    g_test_add_func("/layout/close-during-spawn", test_close_during_spawn);
    g_test_add_data_func("/layout/repeated-split-right", GINT_TO_POINTER(GTK_ORIENTATION_HORIZONTAL), test_repeated_split_geometry);
    g_test_add_data_func("/layout/repeated-split-down", GINT_TO_POINTER(GTK_ORIENTATION_VERTICAL), test_repeated_split_geometry);
    g_test_add_func("/layout/mixed-split-geometry", test_mixed_split_geometry);
    g_test_add_func("/layout/deferred-split-geometry", test_deferred_split_geometry);
    return g_test_run();
}
