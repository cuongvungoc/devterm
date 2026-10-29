#include "../src/pane.h"

static GtkWidget *host;

static void iterate(void) {
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_usleep(1000);
}
static GtkWidget *find_widget(GtkWidget *root, GType type, const char *tooltip) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(root, type) &&
        (!tooltip || g_strcmp0(gtk_widget_get_tooltip_text(root), tooltip) == 0)) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_widget(child, type, tooltip);
        if (found) return found;
    }
    return NULL;
}
static GtkCheckButton *find_option(GtkWidget *root, const char *label) {
    if (GTK_IS_CHECK_BUTTON(root) &&
        g_strcmp0(gtk_check_button_get_label(GTK_CHECK_BUTTON(root)), label) == 0)
        return GTK_CHECK_BUTTON(root);
    for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
        GtkCheckButton *found = find_option(child, label);
        if (found) return found;
    }
    return NULL;
}
static TerminalPane *new_pane(void) {
    TerminalPane *pane = pane_new(NULL, NULL, NULL, NULL);
    gtk_box_append(GTK_BOX(host), pane_widget(pane));
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!pane_is_running(pane) && g_get_monotonic_time() < deadline) iterate();
    g_assert_true(pane_is_running(pane));
    return pane;
}
static void close_pane(TerminalPane *pane) {
    GtkRoot *root = gtk_widget_get_root(pane_widget(pane));
    if (root) gtk_root_set_focus(root, NULL);
    gtk_box_remove(GTK_BOX(host), pane_widget(pane));
    pane_close(pane);
}
static VteTerminal *terminal(TerminalPane *pane) {
    return VTE_TERMINAL(find_widget(pane_widget(pane), VTE_TYPE_TERMINAL, NULL));
}
static void feed(TerminalPane *pane, const char *text, glong minimum_row) {
    vte_terminal_feed(terminal(pane), text, -1);
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    glong row = 0;
    do {
        iterate();
        vte_terminal_get_cursor_position(terminal(pane), NULL, &row);
    } while (row < minimum_row && g_get_monotonic_time() < deadline);
    g_assert_cmpint(row, >=, minimum_row);
}
static void query(TerminalPane *pane, const char *text) {
    pane_search_open(pane);
    GtkWidget *entry = find_widget(pane_widget(pane), GTK_TYPE_ENTRY, NULL);
    gtk_editable_set_text(GTK_EDITABLE(entry), text);
}
static char *selection(TerminalPane *pane) { return vte_terminal_get_text_selected(terminal(pane), VTE_FORMAT_TEXT); }
static char *wait_selection(TerminalPane *pane, const char *different_from) {
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    char *selected = NULL;
    do {
        g_free(selected); iterate(); selected = selection(pane);
    } while ((!selected || !*selected || g_strcmp0(selected, different_from) == 0) && g_get_monotonic_time() < deadline);
    g_assert_nonnull(selected); g_assert_cmpstr(selected, !=, "");
    if (different_from) g_assert_cmpstr(selected, !=, different_from);
    return selected;
}
static void expect_selection(TerminalPane *pane, const char *expected) {
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    char *selected = NULL;
    do {
        g_free(selected);
        iterate();
        selected = selection(pane);
    } while (g_strcmp0(selected, expected) != 0 && g_get_monotonic_time() < deadline);
    g_assert_cmpstr(selected, ==, expected);
    g_free(selected);
}
static void navigate(TerminalPane *pane, gboolean previous) {
    GtkWidget *button = find_widget(pane_widget(pane), GTK_TYPE_BUTTON,
                                  previous ? "Previous match (Shift+Enter)" : "Next match (Enter)");
    g_assert_nonnull(button);
    g_signal_emit_by_name(button, "clicked");
}
static void expect_not_found(TerminalPane *pane) {
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    GtkWidget *label = NULL;
    do {
        iterate();
        label = find_widget(pane_widget(pane), GTK_TYPE_LABEL, "Not found");
    } while (!label && g_get_monotonic_time() < deadline);
    g_assert_nonnull(label);
    g_assert_false(vte_terminal_get_has_selection(terminal(pane)));
}
static void test_literal_unicode_scrollback(void) {
    TerminalPane *pane = new_pane();
    GString *output = g_string_new("\r\nprice.[x]*$\\path\r\nĐƯỜNG DẪN\r\nscrollback-only-marker\r\n");
    for (guint i = 0; i < 400; i++) g_string_append_printf(output, "output line %u\r\n", i);
    feed(pane, output->str, 400);
    query(pane, "price.[x]*$\\path");
    expect_selection(pane, "price.[x]*$\\path");
    query(pane, "đường dẫn");
    expect_selection(pane, "ĐƯỜNG DẪN");
    query(pane, "scrollback-only-marker");
    expect_selection(pane, "scrollback-only-marker");
    query(pane, "absent-text"); expect_not_found(pane);
    query(pane, "");
    g_assert_false(vte_terminal_get_has_selection(terminal(pane)));
    close_pane(pane); g_string_free(output, TRUE);
}
static void test_navigation_case_isolation(void) {
    TerminalPane *first = new_pane(), *second = new_pane();
    feed(first, "\r\nNeedle\r\nneedle\r\nNEEDLE\r\n", 3);
    feed(second, "\r\nonly in second pane\r\n", 1);
    query(first, "needle");
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    while (!vte_terminal_get_has_selection(terminal(first)) && g_get_monotonic_time() < deadline) iterate();
    char *a = wait_selection(first, NULL);
    navigate(first, FALSE); char *b = wait_selection(first, a);
    navigate(first, FALSE); char *c = wait_selection(first, b);
    g_assert_cmpstr(a, !=, b); g_assert_cmpstr(a, !=, c); g_assert_cmpstr(b, !=, c);
    navigate(first, FALSE); expect_selection(first, a);
    navigate(first, TRUE); expect_selection(first, c);
    GtkWidget *match_case = find_widget(pane_widget(first), GTK_TYPE_CHECK_BUTTON, NULL);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(match_case), TRUE);
    expect_selection(first, "needle");
    navigate(first, FALSE); expect_selection(first, "needle");
    query(second, "only in second pane"); expect_selection(second, "only in second pane");
    GtkWidget *first_entry = find_widget(pane_widget(first), GTK_TYPE_ENTRY, NULL);
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(first_entry)), ==, "needle");
    g_assert_true(gtk_check_button_get_active(GTK_CHECK_BUTTON(match_case)));
    query(first, "ONLY IN SECOND PANE"); expect_not_found(first);
    GtkWidget *second_entry = find_widget(pane_widget(second), GTK_TYPE_ENTRY, NULL);
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(second_entry)), ==, "only in second pane");
    gtk_widget_set_visible(pane_widget(first), FALSE);
    query(first, "Needle");
    gint64 hidden_deadline = g_get_monotonic_time() + 250 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < hidden_deadline) iterate();
    gtk_widget_set_visible(pane_widget(first), TRUE);
    expect_selection(first, "Needle");
    /* Search remains useful after the shell exits. */
    pane_send(second, "exit", TRUE);
    deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    while (pane_is_running(second) && g_get_monotonic_time() < deadline) iterate();
    g_assert_false(pane_is_running(second));
    query(second, "only in second pane"); expect_selection(second, "only in second pane");
    close_pane(first); close_pane(second);
    g_free(a); g_free(b); g_free(c);
}
static void test_close_pending_search(void) {
    TerminalPane *pane = new_pane();
    query(pane, "pending search");
    close_pane(pane);
    /* Give a mistakenly retained debounce callback time to run. */
    gint64 deadline = g_get_monotonic_time() + 250 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < deadline) iterate();
}
static void test_whole_words(void) {
    TerminalPane *pane = new_pane();
    feed(pane, "\r\nbobcat\r\nđườngdẫn\r\n_owl owl2 2owl owl_\r\n"
               "DOG\r\n(fox).\r\n(a.b) aXb\r\ncafe\u0301\r\nVIỆT\r\n", 8);
    GtkCheckButton *whole_words = find_option(pane_widget(pane), "Match whole word");
    GtkCheckButton *match_case = find_option(pane_widget(pane), "Match case");
    g_assert_nonnull(whole_words); g_assert_nonnull(match_case);
    g_assert_false(gtk_check_button_get_active(whole_words));
    query(pane, "cat"); expect_selection(pane, "cat");
    gtk_check_button_set_active(whole_words, TRUE); expect_not_found(pane);
    gtk_check_button_set_active(whole_words, FALSE); expect_selection(pane, "cat");
    gtk_check_button_set_active(whole_words, TRUE); expect_not_found(pane);
    query(pane, "đường"); expect_not_found(pane);
    query(pane, "owl"); expect_not_found(pane);
    query(pane, "dog"); expect_selection(pane, "DOG");
    gtk_check_button_set_active(match_case, TRUE); expect_not_found(pane);
    gtk_check_button_set_active(match_case, FALSE); expect_selection(pane, "DOG");
    query(pane, "fox"); expect_selection(pane, "fox");
    navigate(pane, FALSE); expect_selection(pane, "fox");
    navigate(pane, TRUE); expect_selection(pane, "fox");
    query(pane, "a.b"); expect_selection(pane, "a.b");
    query(pane, "cafe"); expect_not_found(pane);
    query(pane, "cafe\u0301"); expect_selection(pane, "cafe\u0301");
    query(pane, "việt"); expect_selection(pane, "VIỆT");
    /* Opening another pane must not share or reset either option. */
    TerminalPane *other = new_pane();
    query(other, "cat");
    GtkCheckButton *other_whole_words = find_option(pane_widget(other), "Match whole word");
    g_assert_false(gtk_check_button_get_active(other_whole_words));
    g_assert_true(gtk_check_button_get_active(whole_words));
    GtkWidget *close = find_widget(pane_widget(pane), GTK_TYPE_BUTTON, "Close search (Esc)");
    g_signal_emit_by_name(close, "clicked");
    pane_search_open(pane); expect_selection(pane, "VIỆT");
    g_assert_true(gtk_check_button_get_active(whole_words));
    query(pane, "");
    gtk_check_button_set_active(whole_words, FALSE);
    g_assert_false(vte_terminal_get_has_selection(terminal(pane)));
    close_pane(other); close_pane(pane);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check()) return 77;
    GtkWidget *window = gtk_window_new();
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 600);
    host = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_window_set_child(GTK_WINDOW(window), host);
    gtk_window_present(GTK_WINDOW(window));
    g_test_add_func("/search/literal-unicode-scrollback", test_literal_unicode_scrollback);
    g_test_add_func("/search/navigation-case-isolation", test_navigation_case_isolation);
    g_test_add_func("/search/close-pending-search", test_close_pending_search);
    g_test_add_func("/search/whole-words", test_whole_words);
    int result = g_test_run();
    gtk_window_destroy(GTK_WINDOW(window));
    return result;
}
