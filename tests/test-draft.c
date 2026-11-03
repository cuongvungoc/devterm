#include "../src/draft.h"
#include <string.h>

static void iterate(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void settle(void) {
    gint64 end = g_get_monotonic_time() + 200000;
    while (g_get_monotonic_time() < end) iterate();
}
static GtkWidget *find(GtkWidget *root, GType type, const char *label) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(root, type) && (!label ||
        (GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), label) == 0))) return root;
    for (GtkWidget *w = gtk_widget_get_first_child(root); w; w = gtk_widget_get_next_sibling(w)) {
        GtkWidget *found = find(w, type, label); if (found) return found;
    }
    return NULL;
}
static void click(Draft *d, const char *label) {
    GtkWidget *button = find(draft_widget(d), GTK_TYPE_BUTTON, label); g_assert_nonnull(button);
    g_signal_emit_by_name(button, "clicked");
}
static void respond(const char *label) {
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
        GtkWidget *w = g_list_model_get_item(windows, i), *button = find(w, GTK_TYPE_BUTTON, label);
        if (gtk_widget_get_visible(w) && button) {
            g_signal_emit_by_name(button, "clicked"); g_object_unref(w); settle(); return;
        }
        g_object_unref(w);
    }
    g_error("No dialog button %s", label);
}
static Draft *present(GtkWidget **window) {
    GtkWidget *terminal = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(terminal), gtk_entry_new()); gtk_widget_set_vexpand(terminal, TRUE);
    Draft *d = draft_new(terminal, "Test 日本語", NULL, NULL);
    *window = gtk_window_new(); gtk_window_set_default_size(GTK_WINDOW(*window), 1000, 700);
    gtk_window_set_child(GTK_WINDOW(*window), draft_widget(d)); gtk_window_present(GTK_WINDOW(*window)); settle(); return d;
}
static GtkTextBuffer *buffer(Draft *d) {
    return gtk_text_view_get_buffer(GTK_TEXT_VIEW(find(draft_widget(d), GTK_TYPE_TEXT_VIEW, NULL)));
}
static char *text(Draft *d) {
    GtkTextIter start, end; gtk_text_buffer_get_bounds(buffer(d), &start, &end);
    return gtk_text_buffer_get_text(buffer(d), &start, &end, TRUE);
}
static void expect(Draft *d, const char *expected) { char *actual = text(d); g_assert_cmpstr(actual, ==, expected); g_free(actual); }
static void destroy(Draft *d, GtkWidget *window) {
    gtk_window_set_focus(GTK_WINDOW(window), NULL); draft_free(d); gtk_window_destroy(GTK_WINDOW(window)); settle();
}
typedef struct { gboolean done; char *text; } Clipboard;
static void copied(GObject *source, GAsyncResult *result, gpointer data) {
    Clipboard *out = data; GError *error = NULL;
    out->text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
    g_assert_no_error(error); out->done = TRUE;
}
static void expect_clipboard(Draft *d, const char *expected) {
    Clipboard out = {0};
    gdk_clipboard_read_text_async(gtk_widget_get_clipboard(draft_widget(d)), NULL, copied, &out);
    gint64 end = g_get_monotonic_time() + 3000000;
    while (!out.done && g_get_monotonic_time() < end) iterate();
    g_assert_true(out.done); g_assert_cmpstr(out.text, ==, expected); g_free(out.text);
}
static void test_copy_clear_undo(void) {
    GtkWidget *window; Draft *d = present(&window); draft_set_visible(d, TRUE); settle();
    g_assert_false(gtk_widget_get_sensitive(find(draft_widget(d), GTK_TYPE_BUTTON, "Copy")));
    const char *notes = "  Nháp 日本語\nGhi chú: 'quotes' | &&\n\n";
    gtk_text_buffer_set_text(buffer(d), notes, -1);
    click(d, "Copy"); expect_clipboard(d, notes); g_assert_true(draft_has_text(d)); expect(d, notes);
    GtkTextIter start, end; gtk_text_buffer_get_iter_at_offset(buffer(d), &start, 2); gtk_text_buffer_get_iter_at_offset(buffer(d), &end, 6);
    gtk_text_buffer_select_range(buffer(d), &start, &end);
    click(d, "Copy"); expect_clipboard(d, "Nháp"); expect(d, notes);
    click(d, "Clear"); respond("Cancel"); expect(d, notes);
    click(d, "Clear"); respond("Clear Draft"); g_assert_false(draft_has_text(d)); expect(d, "");
    g_assert_true(gtk_text_buffer_get_can_undo(buffer(d))); gtk_text_buffer_undo(buffer(d)); expect(d, notes);
    gtk_text_buffer_redo(buffer(d)); expect(d, "");
    gtk_text_buffer_insert_at_cursor(buffer(d), " \n", -1); g_assert_true(draft_has_text(d));
    destroy(d, window);
}
static void test_layout_retention(void) {
    GtkWidget *window; Draft *d = present(&window);
    g_assert_false(draft_is_visible(d)); draft_set_visible(d, TRUE); settle();
    GtkPaned *page = GTK_PANED(draft_widget(d));
    int available; g_object_get(page, "max-position", &available, NULL);
    g_assert_cmpint(ABS(gtk_paned_get_position(page) - (int)(available * .7)), <=, 2);
    gtk_paned_set_position(page, available / 2); settle();
    GString *notes = g_string_new(NULL);
    for (int i = 0; i < 200; i++) g_string_append_printf(notes, "Note %d 日本語\n", i);
    gtk_text_buffer_insert_at_cursor(buffer(d), notes->str, -1); settle();
    GtkWidget *view = find(draft_widget(d), GTK_TYPE_TEXT_VIEW, NULL);
    GtkScrolledWindow *scroll = GTK_SCROLLED_WINDOW(gtk_widget_get_parent(view));
    GtkAdjustment *adjust = gtk_scrolled_window_get_vadjustment(scroll);
    gtk_adjustment_set_value(adjust, 0); settle();
    draft_set_visible(d, FALSE); settle(); g_assert_true(draft_has_text(d));
    draft_set_visible(d, TRUE); settle(); expect(d, notes->str);
    g_assert_cmpint(ABS(gtk_paned_get_position(page) - available / 2), <=, 2);
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adjust), 0, 1);
    g_assert_true(gtk_text_buffer_get_can_undo(buffer(d)));
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 850); settle();
    g_object_get(page, "max-position", &available, NULL);
    g_assert_cmpint(ABS(gtk_paned_get_position(page) - available / 2), <=, 2);
    g_string_free(notes, TRUE); destroy(d, window);
}
static void test_pending_clear_dispose(void) {
    GtkWidget *window; Draft *d = present(&window); draft_set_visible(d, TRUE); settle();
    gtk_text_buffer_set_text(buffer(d), "pending draft", -1); click(d, "Clear");
    gpointer page = draft_widget(d); g_object_add_weak_pointer(G_OBJECT(page), &page);
    draft_free(d); gtk_window_destroy(GTK_WINDOW(window));
    gint64 deadline = g_get_monotonic_time() + 3000000;
    while (page && g_get_monotonic_time() < deadline) iterate();
    g_assert_null(page);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL); if (!gtk_init_check()) return 77;
    g_test_add_func("/draft/copy-clear-undo", test_copy_clear_undo);
    g_test_add_func("/draft/layout-retention", test_layout_retention);
    g_test_add_func("/draft/pending-clear-dispose", test_pending_clear_dispose);
    return g_test_run();
}
