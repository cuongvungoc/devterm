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
static char *pane_text(TerminalPane *pane) {
    GBytes *snapshot = pane_snapshot_text(pane, NULL); g_assert_nonnull(snapshot);
    gsize length; const char *bytes = g_bytes_get_data(snapshot, &length);
    char *text = length ? g_strndup(bytes, length) : g_strdup(""); g_bytes_unref(snapshot); return text;
}
static void test_sequence_tab_target(void) {
    App *app = present(); TerminalPane *target = split_layout_active(app->active_tab->layout);
    gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!pane_is_running(target) && g_get_monotonic_time() < end) settle();
    g_assert_true(pane_is_running(target));
    QuickCommand *cmd = quick_sequence_new("tabs-sequence", "Tab target");
    g_ptr_array_add(cmd->steps, quick_command_step_new("printf 'SEQ-FIRST\\n'", 900));
    g_ptr_array_add(cmd->steps, quick_command_step_new("printf 'SEQ-LAST\\n'", 0));
    on_quick_command(cmd, app); g_assert_true(command_sequences_busy(app->sequences, target));
    on_new(NULL, app); settle(); TerminalTab *other = app->active_tab;
    draft_set_visible(other->draft, TRUE); draft_focus(other->draft); settle();
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(app->window));
    end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (command_sequences_busy(app->sequences, target) && g_get_monotonic_time() < end) settle();
    g_assert_false(command_sequences_busy(app->sequences, target)); settle();
    g_assert_true(app->active_tab == other); g_assert_true(gtk_window_get_focus(GTK_WINDOW(app->window)) == focus);
    char *text = pane_text(target); g_assert_nonnull(strstr(text, "SEQ-LAST")); g_free(text);
    text = pane_text(split_layout_active(other->layout)); g_assert_null(strstr(text, "SEQ-FIRST")); g_assert_null(strstr(text, "SEQ-LAST")); g_free(text);
    /* Closing the application also cancels a pending sequence before tab free. */
    on_quick_command(cmd, app); g_assert_true(command_sequences_busy(app->sequences, split_layout_active(other->layout)));
    quick_command_free(cmd); destroy_app(app);
}
static void wait_live(TerminalPane *pane) {
    gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!pane_is_running(pane) && g_get_monotonic_time() < end) settle();
    g_assert_true(pane_is_running(pane));
}
static void test_pane_confirmation_target(void) {
    App *app = present(); TerminalTab *tab = app->active_tab;
    TerminalPane *first = split_layout_active(tab->layout); wait_live(first);
    split_layout_split(tab->layout, GTK_ORIENTATION_HORIZONTAL); settle();
    TerminalPane *target = pane_ref(split_layout_active(tab->layout)); wait_live(target);
    on_close_pane(NULL, app); GtkWidget *alert = tab->pane_close_dialog; g_assert_nonnull(alert);
    g_assert_true(gtk_window_get_default_widget(GTK_WINDOW(alert)) == find_widget(alert, GTK_TYPE_BUTTON, "Cancel"));
    on_close_pane(NULL, app); g_assert_true(tab->pane_close_dialog == alert);
    gtk_window_close(GTK_WINDOW(app->window)); settle(); g_assert_null(app->window_close_dialog);
    g_assert_true(tab->pane_close_dialog == alert); g_assert_cmpuint(split_layout_pane_count(tab->layout), ==, 2);
    g_signal_emit_by_name(find_widget(alert, GTK_TYPE_BUTTON, "Cancel"), "clicked"); settle();
    g_assert_null(tab->pane_close_dialog); g_assert_true(pane_is_running(target));
    on_close_pane(NULL, app); alert = tab->pane_close_dialog; g_assert_nonnull(alert);
    /* A selection change must never redirect an already captured close request. */
    pane_focus(first); settle(); g_assert_true(split_layout_active(tab->layout) == first);
    g_signal_emit_by_name(find_widget(alert, GTK_TYPE_BUTTON, "Close Pane"), "clicked"); settle();
    g_assert_cmpuint(split_layout_pane_count(tab->layout), ==, 1);
    g_assert_true(split_layout_active(tab->layout) == first); g_assert_true(pane_is_running(first));
    g_assert_false(pane_has_live_session(target)); pane_unref(target); destroy_app(app);
}
static void test_window_confirmation_and_exited(void) {
    App *app = present(); wait_live(split_layout_active(app->active_tab->layout));
    on_new(NULL, app); settle(); wait_live(split_layout_active(app->active_tab->layout));
    gtk_window_close(GTK_WINDOW(app->window)); settle(); GtkWidget *alert = app->window_close_dialog;
    g_assert_nonnull(alert); g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(alert)), ==, "Close DevTerm?");
    g_assert_true(gtk_window_get_default_widget(GTK_WINDOW(alert)) == find_widget(alert, GTK_TYPE_BUTTON, "Cancel"));
    g_signal_emit_by_name(find_widget(alert, GTK_TYPE_BUTTON, "Cancel"), "clicked"); settle();
    g_assert_cmpuint(app->tabs->len, ==, 2); g_assert_null(app->window_close_dialog);
    TerminalTab *tab = app->active_tab; TerminalPane *pane = split_layout_active(tab->layout);
    pane_send(pane, "exit", TRUE); gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (pane_has_live_session(pane) && g_get_monotonic_time() < end) settle();
    g_assert_false(pane_has_live_session(pane)); guint id = tab->id;
    request_close_pane(tab); settle(); g_assert_null(find_tab(app, id));
    g_assert_cmpuint(app->tabs->len, ==, 1);
    GtkWindow *window = g_object_ref(GTK_WINDOW(app->window));
    gtk_window_close(window); settle(); alert = app->window_close_dialog; g_assert_nonnull(alert);
    g_signal_emit_by_name(find_widget(alert, GTK_TYPE_BUTTON, "Close DevTerm"), "clicked"); settle();
    g_assert_false(gtk_widget_get_visible(GTK_WIDGET(window))); g_object_unref(window);
    app = present(); pane = split_layout_active(app->active_tab->layout); wait_live(pane);
    pane_send(pane, "exit", TRUE); end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (pane_has_live_session(pane) && g_get_monotonic_time() < end) settle();
    g_assert_false(pane_has_live_session(pane)); window = g_object_ref(GTK_WINDOW(app->window));
    gtk_window_close(window); settle(); g_assert_false(gtk_widget_get_visible(GTK_WIDGET(window))); g_object_unref(window);
}
static void test_close_preference_and_draft(void) {
    App *app = present(); g_assert_true(app->preferences->confirm_close_sessions);
    action_confirm_close(NULL, NULL, app); g_assert_false(app->preferences->confirm_close_sessions);
    char *path = preferences_config_path(); Preferences *saved = preferences_load(path, NULL);
    g_assert_nonnull(saved); g_assert_false(saved->confirm_close_sessions); g_free(saved); g_free(path);
    activate(application, NULL); settle(); App *other = g_object_get_data(G_OBJECT(gtk_application_get_active_window(application)), "app");
    g_assert_true(app != other); g_assert_true(app->preferences == other->preferences);
    GVariant *state = g_action_get_state(g_action_map_lookup_action(G_ACTION_MAP(other->window), "confirm-close-sessions"));
    g_assert_false(g_variant_get_boolean(state)); g_variant_unref(state);
    GtkApplication *fresh = gtk_application_new("com.example.DevTerm.PreferenceReload", G_APPLICATION_NON_UNIQUE);
    g_assert_false(application_preferences(fresh)->confirm_close_sessions); g_object_unref(fresh);
    TerminalTab *tab = app->active_tab; wait_live(split_layout_active(tab->layout));
    split_layout_split(tab->layout, GTK_ORIENTATION_HORIZONTAL); settle(); wait_live(split_layout_active(tab->layout));
    on_close_pane(NULL, app); settle(); g_assert_null(tab->pane_close_dialog); g_assert_cmpuint(split_layout_pane_count(tab->layout), ==, 1);
    GtkWidget *view = find_widget(tab->page, GTK_TYPE_TEXT_VIEW, NULL);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), "hidden draft — 東京", -1);
    draft_set_visible(tab->draft, FALSE); request_close_tab(tab); g_assert_nonnull(tab->close_dialog);
    g_signal_emit_by_name(find_widget(tab->close_dialog, GTK_TYPE_BUTTON, "Cancel"), "clicked"); settle();
    gtk_window_close(GTK_WINDOW(app->window)); settle(); g_assert_nonnull(app->window_close_dialog);
    g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(app->window_close_dialog)), ==, "Close DevTerm and discard drafts?");
    g_signal_emit_by_name(find_widget(app->window_close_dialog, GTK_TYPE_BUTTON, "Cancel"), "clicked"); settle();
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), "", -1);
    request_close_tab(tab); settle(); g_assert_cmpuint(app->tabs->len, ==, 0);
    action_confirm_close(NULL, NULL, other); g_assert_true(other->preferences->confirm_close_sessions);
    state = g_action_get_state(g_action_map_lookup_action(G_ACTION_MAP(app->window), "confirm-close-sessions"));
    g_assert_true(g_variant_get_boolean(state)); g_variant_unref(state);
    action_confirm_close(NULL, NULL, other); g_assert_false(app->preferences->confirm_close_sessions);
    wait_live(split_layout_active(other->active_tab->layout));
    GtkWindow *other_window = g_object_ref(GTK_WINDOW(other->window));
    gtk_window_close(other_window); settle(); g_assert_false(gtk_widget_get_visible(GTK_WIDGET(other_window))); g_object_unref(other_window);
    action_confirm_close(NULL, NULL, app); g_assert_true(app->preferences->confirm_close_sessions);
    destroy_app(app);
}
static void test_pending_pane_close_lifetime(void) {
    App *app = present(); TerminalTab *tab = app->active_tab;
    split_layout_split(tab->layout, GTK_ORIENTATION_HORIZONTAL); settle();
    TerminalPane *target = pane_ref(split_layout_active(tab->layout)); wait_live(target);
    on_close_pane(NULL, app); GtkWidget *alert = tab->pane_close_dialog; g_assert_nonnull(alert);
    /* The target may already have been closed by its owning layout. */
    g_assert_true(split_layout_close_pane(tab->layout, target));
    g_signal_emit_by_name(find_widget(alert, GTK_TYPE_BUTTON, "Close Pane"), "clicked"); settle();
    g_assert_cmpuint(split_layout_pane_count(tab->layout), ==, 1); g_assert_null(tab->pane_close_dialog); pane_unref(target);
    TerminalPane *sibling = split_layout_active(tab->layout);
    split_layout_split(tab->layout, GTK_ORIENTATION_HORIZONTAL); settle(); wait_live(split_layout_active(tab->layout));
    GtkWidget *view = find_widget(tab->page, GTK_TYPE_TEXT_VIEW, NULL);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), "Draft must survive", -1);
    on_close_pane(NULL, app); alert = tab->pane_close_dialog; g_assert_nonnull(alert);
    g_assert_true(split_layout_close_pane(tab->layout, sibling));
    g_signal_emit_by_name(find_widget(alert, GTK_TYPE_BUTTON, "Close Pane"), "clicked"); settle();
    g_assert_cmpuint(split_layout_pane_count(tab->layout), ==, 1); g_assert_nonnull(tab->close_dialog);
    g_signal_emit_by_name(find_widget(tab->close_dialog, GTK_TYPE_BUTTON, "Cancel"), "clicked"); settle();
    g_assert_true(draft_has_text(tab->draft));
    split_layout_split(tab->layout, GTK_ORIENTATION_HORIZONTAL); settle(); wait_live(split_layout_active(tab->layout));
    on_close_pane(NULL, app); g_assert_nonnull(tab->pane_close_dialog);
    destroy_app(app); /* Pending requests must release references without touching freed tabs. */
}
static void test_pane_identity_and_sequence_status(void) {
    App *app = present(); TerminalTab *tab = app->active_tab;
    TerminalPane *first = split_layout_active(tab->layout); wait_live(first);
    char *identity = g_strdup(pane_title(first));
    split_layout_split(tab->layout, GTK_ORIENTATION_HORIZONTAL); settle();
    TerminalPane *other = split_layout_active(tab->layout); wait_live(other);
    g_assert_cmpstr(pane_title(first), !=, pane_title(other));
    QuickCommand *sequence = quick_sequence_new("identity-sequence", "Chuỗi — 東京");
    g_ptr_array_add(sequence->steps, quick_command_step_new("printf 'IDENTITY-FIRST\\n'", 900));
    g_ptr_array_add(sequence->steps, quick_command_step_new("printf 'IDENTITY-LAST\\n'", 0));
    pane_focus(first); settle(); on_quick_command(sequence, app);
    g_assert_true(command_sequences_busy(app->sequences, first));
    g_assert_nonnull(strstr(pane_status_message(first), "1/2"));
    pane_focus(other); settle(); GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(app->window));
    g_assert_true(split_layout_active(tab->layout) == other);
    gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (command_sequences_busy(app->sequences, first) && g_get_monotonic_time() < end) settle();
    g_assert_false(command_sequences_busy(app->sequences, first));
    g_assert_cmpstr(pane_title(first), ==, identity);
    g_assert_true(gtk_window_get_focus(GTK_WINDOW(app->window)) == focus);
    GtkWidget *header = gtk_widget_get_first_child(pane_widget(first)), *name = gtk_widget_get_first_child(header);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(name)), ==, identity);
    GtkWidget *notice = gtk_widget_get_next_sibling(name);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(notice)), ==, pane_status_message(first));
    g_assert_nonnull(strstr(gtk_widget_get_tooltip_text(notice), "all 2 commands sent"));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(app->status)), identity));
    char *text = pane_text(first); g_assert_nonnull(strstr(text, "IDENTITY-LAST")); g_free(text);
    text = pane_text(other); g_assert_null(strstr(text, "IDENTITY-LAST")); g_free(text);
    pane_report_status(first, "Connection warning — Đường 日本語");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(name)), ==, identity);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(app->status)), identity));
    pane_focus(first); settle(); pane_focus(other); settle();
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(app->status)), pane_title(other)));
    quick_command_free(sequence); g_free(identity); destroy_app(app);
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
    g_test_add_func("/tabs/sequence-tab-target", test_sequence_tab_target);
    g_test_add_func("/tabs/pane-close-captured-target", test_pane_confirmation_target);
    g_test_add_func("/tabs/window-close-live-and-exited", test_window_confirmation_and_exited);
    g_test_add_func("/tabs/close-preference-draft", test_close_preference_and_draft);
    g_test_add_func("/tabs/pending-pane-close-lifetime", test_pending_pane_close_lifetime);
    g_test_add_func("/tabs/pane-identity-sequence-status", test_pane_identity_and_sequence_status);
    int result = g_test_run(); g_object_unref(application);
    char *preferences = preferences_config_path(), *directory = g_path_get_dirname(preferences);
    g_remove(preferences); g_rmdir(directory); g_free(preferences); g_free(directory); g_rmdir(config); g_free(config); return result;
}
