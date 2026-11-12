/* Exercise the real window integration, not a duplicate tab model. */
#define main devterm_application_main
#include "../src/main.c"
#undef main
#include <glib/gstdio.h>

static GtkApplication *application;
static void settle(void) {
    gint64 end = g_get_monotonic_time() + 180000;
    while (g_get_monotonic_time() < end) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
}
static GtkWidget *find_widget(GtkWidget *root, GType type, const char *text) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(root, type) && (!text ||
        (GTK_IS_LABEL(root) && g_strcmp0(gtk_label_get_text(GTK_LABEL(root)), text) == 0) ||
        (GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), text) == 0))) return root;
    for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_widget(child, type, text); if (found) return found;
    }
    return NULL;
}
static App *present(void) {
    activate(application, NULL); settle();
    GtkWindow *window = gtk_application_get_active_window(application);
    g_assert_nonnull(window);
    return g_object_get_data(G_OBJECT(window), "app");
}
static void destroy_app(App *app) { gtk_window_destroy(GTK_WINDOW(app->window)); settle(); }
static void action(App *app, guint id, const char *name) {
    g_assert_true(gtk_widget_activate_action(app->notebook, name, "u", id)); settle();
}
static GtkWidget *dialog(const char *title) {
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
        GtkWindow *w = g_list_model_get_item(windows, i);
        gboolean matches = g_strcmp0(gtk_window_get_title(w), title) == 0;
        g_object_unref(w); if (matches) return GTK_WIDGET(w);
    }
    return NULL;
}
static void rename_tab(App *app, guint id, const char *name) {
    action(app, id, "tab.rename"); GtkWidget *d = dialog("Rename Tab"); g_assert_nonnull(d);
    gtk_editable_set_text(GTK_EDITABLE(find_widget(d, GTK_TYPE_ENTRY, NULL)), name);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); settle();
}
static void test_rename(void) {
    App *app = present(); TerminalTab *first = app->active_tab;
    on_new(NULL, app); settle(); TerminalTab *second = app->active_tab;
    rename_tab(app, first->id, "  Máy chủ — 東京  ");
    g_assert_cmpstr(first->title, ==, "Máy chủ — 東京"); g_assert_true(app->active_tab == second);
    GtkWidget *header = gtk_notebook_get_tab_label(GTK_NOTEBOOK(app->notebook), first->page);
    g_assert_cmpstr(gtk_widget_get_tooltip_text(header), ==, first->title);
    g_assert_nonnull(find_widget(first->page, GTK_TYPE_LABEL, "Draft — Máy chủ — 東京"));
    g_assert_null(pane_session_profile(split_layout_active(first->layout)));
    rename_tab(app, second->id, first->title); /* Repeated names still have separate IDs. */
    action(app, first->id, "tab.right");
    rename_tab(app, first->id, "Only first"); g_assert_cmpstr(second->title, ==, "Máy chủ — 東京");
    destroy_app(app);
}
static void test_validation(void) {
    App *app = present(); TerminalTab *tab = app->active_tab;
    const char *invalid[] = {"", "　   ", "new\nname", "bad\001name", "new\u2028name", "new\u2029name"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
        rename_tab(app, tab->id, invalid[i]);
        GtkWidget *d = dialog("Rename Tab"); g_assert_nonnull(d);
        g_assert_cmpstr(tab->title, ==, "Terminal 1");
        gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); settle();
    }
    GString *long_name = g_string_new(NULL);
    for (int i = 0; i < 129; i++) g_string_append(long_name, "界");
    rename_tab(app, tab->id, long_name->str); g_assert_nonnull(dialog("Rename Tab"));
    gtk_dialog_response(GTK_DIALOG(dialog("Rename Tab")), GTK_RESPONSE_CANCEL); settle();
    g_string_truncate(long_name, long_name->len - 3); rename_tab(app, tab->id, long_name->str);
    g_assert_null(dialog("Rename Tab")); g_assert_cmpstr(tab->title, ==, long_name->str);
    g_string_free(long_name, TRUE); destroy_app(app);
}
static void test_order_and_state(void) {
    App *app = present(); TerminalTab *first = app->active_tab;
    split_layout_split(first->layout, GTK_ORIENTATION_HORIZONTAL); settle();
    TerminalPane *pane = split_layout_active(first->layout);
    draft_set_visible(first->draft, TRUE); settle();
    GtkWidget *view = find_widget(first->page, GTK_TYPE_TEXT_VIEW, NULL);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_begin_user_action(buffer); gtk_text_buffer_insert_at_cursor(buffer, "nháp\n東京", -1); gtk_text_buffer_end_user_action(buffer);
    draft_focus(first->draft);
    on_new(NULL, app); settle(); TerminalTab *second = app->active_tab;
    on_new(NULL, app); settle(); TerminalTab *third = app->active_tab;
    action(app, first->id, "tab.right"); action(app, first->id, "tab.right");
    g_assert_true(app->tabs->pdata[0] == second); g_assert_true(app->tabs->pdata[1] == third); g_assert_true(app->tabs->pdata[2] == first);
    action(app, first->id, "tab.right"); /* Boundary is a no-op. */
    action(app, first->id, "tab.left");
    g_assert_true(app->tabs->pdata[1] == first); g_assert_true(app->active_tab == third);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), 1); settle();
    g_assert_true(app->active_tab == first); g_assert_true(split_layout_active(first->layout) == pane);
    g_assert_cmpuint(split_layout_pane_count(first->layout), ==, 2);
    g_assert_true(draft_wants_focus(first->draft)); g_assert_true(gtk_text_buffer_get_can_undo(buffer));
    GtkTextIter a, b; gtk_text_buffer_get_bounds(buffer, &a, &b); char *text = gtk_text_buffer_get_text(buffer, &a, &b, TRUE);
    g_assert_cmpstr(text, ==, "nháp\n東京"); g_free(text);
    destroy_app(app);
}
static void test_close_confirmation(void) {
    App *app = present(); TerminalTab *first = app->active_tab;
    on_new(NULL, app); settle(); TerminalTab *second = app->active_tab;
    rename_tab(app, first->id, "Router A");
    action(app, first->id, "tab.close");
    g_assert_nonnull(first->close_dialog); g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(first->close_dialog)), ==, "Close Router A?");
    g_signal_emit_by_name(find_widget(first->close_dialog, GTK_TYPE_BUTTON, "Cancel"), "clicked"); settle();
    g_assert_cmpuint(app->tabs->len, ==, 2); g_assert_true(app->active_tab == second);
    action(app, first->id, "tab.close"); guint id = first->id;
    g_signal_emit_by_name(find_widget(first->close_dialog, GTK_TYPE_BUTTON, "Close Tab"), "clicked"); settle();
    g_assert_null(find_tab(app, id)); g_assert_cmpuint(app->tabs->len, ==, 1); g_assert_true(app->active_tab == second);
    destroy_app(app);
}
static void test_pending_rename(void) {
    App *app = present(); guint id = app->active_tab->id;
    action(app, id, "tab.rename"); g_assert_nonnull(dialog("Rename Tab"));
    close_tab_now(find_tab(app, id)); settle();
    g_assert_null(dialog("Rename Tab")); g_assert_cmpuint(app->tabs->len, ==, 0);
    on_new(NULL, app); settle(); action(app, app->active_tab->id, "tab.rename");
    destroy_app(app); g_assert_null(dialog("Rename Tab"));
}
int main(int argc, char **argv) {
    char *config = g_dir_make_tmp("devterm-tabs-XXXXXX", NULL); g_assert_nonnull(config);
    g_setenv("XDG_CONFIG_HOME", config, TRUE);
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check()) { g_rmdir(config); g_free(config); return 77; }
    application = gtk_application_new("com.example.DevTerm.TabTests", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, NULL));
    g_test_add_func("/tabs/rename-unicode-stable-target", test_rename);
    g_test_add_func("/tabs/validate-cancel", test_validation);
    g_test_add_func("/tabs/order-preserves-state", test_order_and_state);
    g_test_add_func("/tabs/close-confirmation", test_close_confirmation);
    g_test_add_func("/tabs/pending-rename-close", test_pending_rename);
    int result = g_test_run(); g_object_unref(application); g_rmdir(config); g_free(config); return result;
}
