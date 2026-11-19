/* Real application actions, real GTK key delivery, and the actual settings UI. */
#define main devterm_application_main
#include "../src/main.c"
#undef main
#include <gdk/x11/gdkx.h>
#include <glib/gstdio.h>

static GtkApplication *application;
static char *root;
static guint fixture_id;
static void settle(void) {
    gint64 end = g_get_monotonic_time() + 180000;
    while (g_get_monotonic_time() < end) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
}
static GtkWidget *find_widget(GtkWidget *root_widget, GType type, const char *text) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(root_widget, type) && (!text ||
        (GTK_IS_LABEL(root_widget) && g_strcmp0(gtk_label_get_text(GTK_LABEL(root_widget)), text) == 0) ||
        (GTK_IS_BUTTON(root_widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root_widget)), text) == 0))) return root_widget;
    for (GtkWidget *child = gtk_widget_get_first_child(root_widget); child; child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_widget(child, type, text); if (found) return found;
    }
    return NULL;
}
static GtkWidget *dialog(const char *title) {
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
        GtkWindow *w = g_list_model_get_item(windows, i);
        gboolean same = g_strcmp0(gtk_window_get_title(w), title) == 0; g_object_unref(w); if (same) return GTK_WIDGET(w);
    }
    return NULL;
}
static void click(GtkWidget *button) { g_assert_nonnull(button); g_assert_true(gtk_widget_is_sensitive(button)); g_signal_emit_by_name(button, "clicked"); settle(); }
static App *present_application(void) {
    char *app_id = g_strdup_printf("com.example.DevTerm.ShortcutTests%u", fixture_id);
    application = gtk_application_new(app_id, G_APPLICATION_NON_UNIQUE); g_free(app_id);
    GError *error = NULL; g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    activate(application, NULL); settle();
    return g_object_get_data(G_OBJECT(gtk_application_get_active_window(application)), "app");
}
static App *present_with(const char *configuration) {
    char *dir = g_strdup_printf("%s/%u", root, ++fixture_id), *subdir = g_build_filename(dir, "devterm", NULL);
    g_mkdir_with_parents(subdir, 0700); g_setenv("XDG_CONFIG_HOME", dir, TRUE);
    if (configuration) { char *path = g_build_filename(subdir, "shortcuts.json", NULL); g_file_set_contents(path, configuration, -1, NULL); g_free(path); }
    g_free(dir); g_free(subdir); return present_application();
}
static App *present(void) { return present_with(NULL); }
static void cleanup(App *app) { gtk_window_destroy(GTK_WINDOW(app->window)); settle(); g_object_unref(application); application = NULL; }
static void keys(GtkWidget *window, const char *verb, const char *combination) {
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window)); g_assert_true(GDK_IS_X11_SURFACE(surface));
    char *id = g_strdup_printf("%lu", gdk_x11_surface_get_xid(surface));
    const char *args[] = {"xdotool", "windowraise", id, "windowfocus", "--sync", id, verb, "--clearmodifiers", combination, NULL};
    GError *error = NULL; int status;
    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, &error));
    g_assert_no_error(error); g_assert_cmpint(status, ==, 0); g_free(id); settle();
}
static GtkWidget *row_button(GtkWidget *d, ShortcutId id, const char *label) {
    GtkWidget *name = find_widget(d, GTK_TYPE_LABEL, shortcut_definition(id)->label); g_assert_nonnull(name);
    GtkWidget *row = gtk_widget_get_parent(gtk_widget_get_parent(name));
    return find_widget(row, GTK_TYPE_BUTTON, label);
}
static void test_keys_layout(void) {
    App *app = present(); TerminalTab *tab = app->active_tab; TerminalPane *first = split_layout_active(tab->layout);
    keys(app->window, "key", "ctrl+shift+e"); g_assert_cmpuint(split_layout_pane_count(tab->layout), ==, 2);
    TerminalPane *second = split_layout_active(tab->layout);
    keys(app->window, "key", "ctrl+shift+o"); g_assert_cmpuint(split_layout_pane_count(tab->layout), ==, 3);
    TerminalPane *third = split_layout_active(tab->layout);
    keys(app->window, "key", "ctrl+shift+Prior"); g_assert_true(split_layout_active(tab->layout) == second);
    keys(app->window, "key", "ctrl+shift+Next"); g_assert_true(split_layout_active(tab->layout) == third);
    keys(app->window, "key", "ctrl+shift+Next"); g_assert_true(split_layout_active(tab->layout) == first);
    split_layout_select_direction(tab->layout, GTK_DIR_RIGHT); g_assert_true(split_layout_active(tab->layout) != first);
    keys(app->window, "keydown", "ctrl+shift+x"); g_assert_cmpuint(split_layout_pane_count(tab->layout), ==, 2);
    keys(app->window, "keydown", "ctrl+shift+x"); g_assert_cmpuint(split_layout_pane_count(tab->layout), ==, 2);
    keys(app->window, "keyup", "ctrl+shift+x");
    keys(app->window, "key", "ctrl+shift+t"); g_assert_cmpuint(app->tabs->len, ==, 2);
    keys(app->window, "key", "ctrl+Prior"); g_assert_true(app->active_tab == tab);
    keys(app->window, "key", "ctrl+shift+F2"); g_assert_nonnull(dialog("Rename Tab"));
    keys(dialog("Rename Tab"), "key", "Escape");
    keys(app->window, "key", "ctrl+shift+m"); g_assert_true(draft_is_visible(tab->draft));
    cleanup(app);
}
static void test_editor_live_rebind(void) {
    App *app = present(); keys(app->window, "key", "ctrl+shift+comma");
    GtkWidget *d = dialog("Keyboard Shortcuts"); g_assert_nonnull(d);
    click(row_button(d, SC_SPLIT_RIGHT, NULL)); keys(d, "key", "ctrl+shift+y");
    g_assert_cmpuint(split_layout_pane_count(app->active_tab->layout), ==, 1);
    click(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_APPLY));
    g_assert_nonnull(strstr(gtk_widget_get_tooltip_text(app->shortcut_widgets[SC_SPLIT_RIGHT]), "Y"));
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); settle(); split_layout_focus(app->active_tab->layout);
    keys(app->window, "key", "ctrl+shift+e"); g_assert_cmpuint(split_layout_pane_count(app->active_tab->layout), ==, 1);
    keys(app->window, "key", "ctrl+shift+y"); g_assert_cmpuint(split_layout_pane_count(app->active_tab->layout), ==, 2);
    keys(app->window, "key", "ctrl+shift+y"); g_assert_cmpuint(split_layout_pane_count(app->active_tab->layout), ==, 3);
    /* All existing windows share settings, and newly created ones inherit them. */
    activate(application, NULL); settle(); GtkWindow *second_window = gtk_application_get_active_window(application);
    App *other = g_object_get_data(G_OBJECT(second_window), "app"); g_assert_true(other != app);
    keys(other->window, "key", "ctrl+shift+y"); g_assert_cmpuint(split_layout_pane_count(other->active_tab->layout), ==, 2);
    char *path = shortcuts_config_path(); ShortcutBindings *saved = shortcuts_load(path, NULL);
    g_assert_cmpstr(saved->keys[SC_SPLIT_RIGHT], ==, shortcut_settings_bindings(app->shortcuts)->keys[SC_SPLIT_RIGHT]);
    shortcut_bindings_free(saved); g_free(path); gtk_window_destroy(second_window); settle(); cleanup(app);
    fixture_id++; app = present_application(); /* New application object reads the saved file. */
    keys(app->window, "key", "ctrl+shift+y"); g_assert_cmpuint(split_layout_pane_count(app->active_tab->layout), ==, 2); cleanup(app);
}
static void test_conflict_cancel_reset(void) {
    App *app = present(); show_shortcuts(app); settle(); GtkWidget *d = dialog("Keyboard Shortcuts");
    click(row_button(d, SC_SPLIT_RIGHT, NULL)); keys(d, "key", "ctrl+shift+r");
    GtkWidget *reassign = find_widget(d, GTK_TYPE_BUTTON, "Reassign"); g_assert_true(gtk_widget_get_visible(reassign));
    g_assert_false(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_APPLY)));
    click(reassign); click(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_APPLY));
    g_assert_null(shortcut_settings_bindings(app->shortcuts)->keys[SC_RECONNECT]);
    click(row_button(d, SC_SPLIT_RIGHT, "Remove"));
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); settle();
    g_assert_nonnull(shortcut_settings_bindings(app->shortcuts)->keys[SC_SPLIT_RIGHT]);
    show_shortcuts(app); settle(); d = dialog("Keyboard Shortcuts");
    click(row_button(d, SC_RECONNECT, "Reset"));
    g_assert_true(gtk_widget_get_visible(find_widget(d, GTK_TYPE_BUTTON, "Reassign")));
    keys(d, "key", "Escape");
    click(row_button(d, SC_SPLIT_RIGHT, "Reset")); click(row_button(d, SC_RECONNECT, "Reset"));
    click(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_APPLY));
    g_assert_nonnull(shortcut_settings_bindings(app->shortcuts)->keys[SC_RECONNECT]);
    click(find_widget(d, GTK_TYPE_BUTTON, "Reset All")); click(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_APPLY));
    ShortcutBindings *defaults = shortcut_bindings_new();
    for (int i = 0; i < SC_COUNT; i++) g_assert_cmpstr(defaults->keys[i], ==, shortcut_settings_bindings(app->shortcuts)->keys[i]);
    shortcut_bindings_free(defaults);
    gtk_editable_set_text(GTK_EDITABLE(find_widget(d, GTK_TYPE_SEARCH_ENTRY, NULL)), "reconnect"); settle();
    GtkWidget *name = find_widget(d, GTK_TYPE_LABEL, shortcut_definition(SC_SPLIT_RIGHT)->label);
    g_assert_false(gtk_widget_get_visible(gtk_widget_get_parent(gtk_widget_get_parent(gtk_widget_get_parent(name)))));
    cleanup(app); g_assert_null(dialog("Keyboard Shortcuts"));
}
typedef struct { gboolean done; char *text; } ClipboardRead;
static void clipboard_read(GObject *source, GAsyncResult *result, gpointer data) {
    ClipboardRead *read = data; GError *error = NULL;
    read->text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error); g_assert_no_error(error); read->done = TRUE;
}
static char *clipboard_text(GtkWidget *widget) {
    ClipboardRead read = {0}; gdk_clipboard_read_text_async(gtk_widget_get_clipboard(widget), NULL, clipboard_read, &read);
    gint64 end = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    while (!read.done && g_get_monotonic_time() < end) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
    g_assert_true(read.done); return read.text;
}
static void test_copy_focus_and_paste(void) {
    App *app = present(); TerminalPane *pane = split_layout_active(app->active_tab->layout);
    VteTerminal *terminal = VTE_TERMINAL(find_widget(pane_widget(pane), VTE_TYPE_TERMINAL, NULL));
    vte_terminal_feed(terminal, "UNIQUE-COPY-TEXT\r\n", -1); settle();
    keys(app->window, "key", "ctrl+shift+a"); /* Copy All does not change selection. */
    g_assert_false(vte_terminal_get_has_selection(terminal));
    char *copied = clipboard_text(app->window); g_assert_nonnull(strstr(copied, "UNIQUE-COPY-TEXT")); g_free(copied);
    keys(app->window, "key", "ctrl+shift+e");
    TerminalPane *second = split_layout_active(app->active_tab->layout);
    VteTerminal *second_terminal = VTE_TERMINAL(find_widget(pane_widget(second), VTE_TYPE_TERMINAL, NULL));
    vte_terminal_feed(second_terminal, "SECOND-PANE-ONLY\r\n", -1); settle();
    gtk_widget_grab_focus(app->shortcut_widgets[SC_FIND]); keys(app->window, "key", "ctrl+shift+a");
    copied = clipboard_text(app->window); g_assert_nonnull(strstr(copied, "SECOND-PANE-ONLY"));
    g_assert_null(strstr(copied, "UNIQUE-COPY-TEXT")); g_free(copied);
    split_layout_cycle(app->active_tab->layout, TRUE); g_assert_true(split_layout_active(app->active_tab->layout) == pane);
    draft_set_visible(app->active_tab->draft, TRUE); draft_focus(app->active_tab->draft); settle();
    GtkTextView *view = GTK_TEXT_VIEW(find_widget(app->active_tab->page, GTK_TYPE_TEXT_VIEW, NULL));
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(view);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(app->window), "DRAFT-ONLY");
    keys(app->window, "key", "ctrl+v");
    GtkTextIter a, b; gtk_text_buffer_get_bounds(buffer, &a, &b); char *text = gtk_text_buffer_get_text(buffer, &a, &b, TRUE);
    g_assert_cmpstr(text, ==, "DRAFT-ONLY"); g_free(text);
    /* Custom terminal Copy key must leave standard Draft copy untouched. */
    ShortcutBindings *bindings = shortcut_bindings_copy(shortcut_settings_bindings(app->shortcuts));
    g_assert_true(shortcut_bindings_set(bindings, SC_COPY, "<Control><Shift>b", NULL));
    g_assert_true(shortcut_settings_apply(app->shortcuts, bindings, FALSE, NULL)); shortcut_bindings_free(bindings);
    split_layout_focus(app->active_tab->layout); vte_terminal_select_all(terminal);
    keys(app->window, "key", "ctrl+shift+b");
    copied = clipboard_text(app->window); g_assert_nonnull(strstr(copied, "UNIQUE-COPY-TEXT")); g_free(copied);
    /* Old copy chord no longer maps to an application action. */
    char **accels = gtk_application_get_accels_for_action(application, "win.copy"); g_assert_null(accels[0]); g_strfreev(accels);
    cleanup(app);
}
static void test_recovery_and_write_failure(void) {
    App *app = present_with("{invalid"); g_assert_nonnull(shortcut_settings_error(app->shortcuts));
    ShortcutBindings *b = shortcut_bindings_copy(shortcut_settings_bindings(app->shortcuts)); GError *error = NULL;
    g_assert_false(shortcut_settings_apply(app->shortcuts, b, FALSE, &error)); g_assert_nonnull(error); g_clear_error(&error);
    show_shortcuts(app); settle(); GtkWidget *d = dialog("Keyboard Shortcuts");
    g_assert_false(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_APPLY)));
    click(find_widget(d, GTK_TYPE_BUTTON, "Restore Defaults & Back Up Invalid File"));
    click(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_APPLY));
    g_assert_null(shortcut_settings_error(app->shortcuts));
    char *path = shortcuts_config_path(), *dir = g_path_get_dirname(path); GDir *directory = g_dir_open(dir, 0, NULL);
    gboolean found = FALSE; const char *name;
    while ((name = g_dir_read_name(directory))) if (g_str_has_prefix(name, "shortcuts.json.invalid-")) {
        char *backup = g_build_filename(dir, name, NULL), *text; g_file_get_contents(backup, &text, NULL, NULL);
        g_assert_cmpstr(text, ==, "{invalid"); found = TRUE; g_free(text); g_free(backup);
    }
    g_assert_true(found); g_dir_close(directory);
    /* A broken config directory must leave the active bindings unchanged. */
    click(row_button(d, SC_COPY_ALL, "Remove"));
    char *saved_dir = g_strconcat(dir, "-saved", NULL);
    g_assert_cmpint(g_rename(dir, saved_dir), ==, 0); g_assert_true(g_file_set_contents(dir, "blocked", -1, NULL));
    click(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_APPLY));
    g_assert_nonnull(shortcut_settings_bindings(app->shortcuts)->keys[SC_COPY_ALL]);
    g_assert_cmpint(g_remove(dir), ==, 0); g_assert_cmpint(g_rename(saved_dir, dir), ==, 0); g_free(saved_dir);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); settle();
    /* External modifications are preserved, rather than overwritten on Apply. */
    g_file_set_contents(path, "external changes", -1, NULL);
    g_assert_false(shortcut_settings_apply(app->shortcuts, b, FALSE, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error(&error);
    g_free(path); g_free(dir); shortcut_bindings_free(b); cleanup(app);
}
static char *pane_text(TerminalPane *pane) {
    GBytes *bytes = pane_snapshot_text(pane, NULL); g_assert_nonnull(bytes); gsize length;
    const char *data = g_bytes_get_data(bytes, &length); char *text = g_strndup(data, length); g_bytes_unref(bytes); return text;
}
static void wait_file(const char *path) {
    gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!g_file_test(path, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < end) settle();
    g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
}
static void wait_ready(TerminalPane *pane, guint count) {
    gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < end) {
        char *text = pane_text(pane); guint seen = 0; const char *p = text;
        while ((p = strstr(p, "FIXTURE-READY"))) { seen++; p++; } g_free(text);
        if (seen >= count && pane_is_running(pane)) return;
        settle();
    }
    g_error("Session fixture did not become ready");
}
static void test_session_shortcuts(void) {
    char *clients = g_build_filename(root, "clients", NULL); g_mkdir(clients, 0700);
    char *ssh = g_build_filename(clients, "ssh", NULL);
    const char *fixture = "#!/usr/bin/python3\nimport os,signal,sys,time\nprint('FIXTURE-READY',os.getpid(),flush=True)\nsignal.signal(signal.SIGHUP,lambda *a:sys.exit(0))\nwhile True: time.sleep(.05)\n";
    g_file_set_contents(ssh, fixture, -1, NULL); g_chmod(ssh, 0700);
    char *original_path = g_strdup(g_getenv("PATH")), *path = g_strconcat(clients, ":", original_path, NULL); g_setenv("PATH", path, TRUE); g_free(path);
    App *app = present(); TerminalTab *local = app->active_tab;
    SessionProfile *profile = session_profile_new(); profile->type = SESSION_SSH; profile->password_auth = FALSE;
    g_free(profile->name); profile->name = g_strdup("Shortcut fixture");
    g_free(profile->host); profile->host = g_strdup("fixture.example");
    g_free(profile->username); profile->username = g_strdup("tester");
    on_session_connect(profile, NULL, app); settle(); TerminalTab *session = app->active_tab;
    TerminalPane *remote = split_layout_active(session->layout); wait_ready(remote, 1);
    on_split_right(NULL, app); settle(); TerminalPane *other = split_layout_active(session->layout);
    split_layout_cycle(session->layout, TRUE); g_assert_true(split_layout_active(session->layout) == remote);
    /* Toolbar focus must preserve the session destination. */
    gtk_widget_grab_focus(app->shortcut_widgets[SC_SESSIONS]);
    keys(app->window, "key", "ctrl+shift+r"); wait_ready(remote, 2);
    g_assert_cmpuint(app->tabs->len, ==, 2); g_assert_cmpuint(split_layout_pane_count(session->layout), ==, 2);
    g_assert_true(split_layout_active(session->layout) == remote); g_assert_true(pane_is_running(other));
    keys(app->window, "key", "ctrl+shift+d"); settle(); g_assert_false(pane_has_live_session(remote));
    keys(app->window, "key", "ctrl+shift+r"); wait_ready(remote, 3);
    /* A local shell is not silently killed by the connection shortcut. */
    keys(app->window, "key", "ctrl+Prior"); g_assert_true(app->active_tab == local);
    keys(app->window, "key", "ctrl+shift+d"); g_assert_true(pane_is_running(split_layout_active(local->layout)));
    /* Password cancellation and target identity use the existing reconnect flow. */
    profile->password_auth = TRUE; on_session_connect(profile, "fixture-password", app); settle();
    TerminalTab *password_tab = app->active_tab; TerminalPane *password_pane = split_layout_active(password_tab->layout); wait_ready(password_pane, 1);
    keys(app->window, "key", "ctrl+shift+r"); GtkWidget *password = dialog("Session Password"); g_assert_nonnull(password);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), 0); settle();
    gtk_dialog_response(GTK_DIALOG(password), GTK_RESPONSE_CANCEL); settle(); g_assert_true(pane_is_running(password_pane));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), 2); settle();
    keys(app->window, "key", "ctrl+shift+r"); g_assert_nonnull(dialog("Session Password"));
    close_tab_now(password_tab); settle(); g_assert_null(dialog("Session Password"));
    cleanup(app); session_profile_free(profile); g_setenv("PATH", original_path, TRUE); g_free(original_path); g_free(ssh); g_free(clients);
}
static void test_raw_input_routing(void) {
    App *app = present(); TerminalPane *pane = split_layout_active(app->active_tab->layout);
    char *log = g_build_filename(root, "raw-input", NULL), *ready = g_build_filename(root, "raw-ready", NULL);
    char *script = g_strdup_printf("import os,tty; tty.setraw(0); open(%c%s%c,'w').close(); f=open(%c%s%c,'ab',buffering=0)\nwhile True: f.write(os.read(0,4096))", '\'', ready, '\'', '\'', log, '\'');
    char *quoted = g_shell_quote(script), *command = g_strconcat("python3 -u -c ", quoted, NULL);
    pane_send(pane, command, TRUE); wait_file(ready); settle();
    TerminalTab *original_tab = app->active_tab;
    on_new(NULL, app); settle(); gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), 0); settle();
    ShortcutBindings *b = shortcut_bindings_copy(shortcut_settings_bindings(app->shortcuts));
    g_assert_true(shortcut_bindings_set(b, SC_COPY, "<Control><Shift>b", NULL));
    g_assert_true(shortcut_bindings_set(b, SC_NEXT_TAB, NULL, NULL));
    g_assert_true(shortcut_bindings_set(b, SC_PASTE, "<Control><Shift>j", NULL));
    g_assert_true(shortcut_settings_apply(app->shortcuts, b, FALSE, NULL)); shortcut_bindings_free(b);
    keys(app->window, "key", "ctrl+shift+b"); keys(app->window, "key", "ctrl+shift+a"); keys(app->window, "key", "ctrl+shift+f");
    pane_focus(pane); keys(app->window, "key", "ctrl+Next"); g_assert_true(app->active_tab == original_tab);
    keys(app->window, "key", "ctrl+shift+c");
    gdk_clipboard_set_text(gtk_widget_get_clipboard(app->window), "PASTED-Đường-日本語");
    keys(app->window, "key", "ctrl+shift+j");
    char *text; gsize length; g_assert_true(g_file_get_contents(log, &text, &length, NULL));
    const char *expected = "\033[6;5~\003PASTED-Đường-日本語";
    g_assert_cmpuint(length, ==, strlen(expected)); g_assert_cmpmem(text, length, expected, strlen(expected));
    g_free(text); g_free(script); g_free(quoted); g_free(command); g_free(log); g_free(ready); cleanup(app);
}
static void remove_tree(const char *path) {
    GDir *dir = g_dir_open(path, 0, NULL);
    if (dir) { const char *name; while ((name = g_dir_read_name(dir))) { char *child = g_build_filename(path, name, NULL); remove_tree(child); g_free(child); } g_dir_close(dir); g_rmdir(path); }
    else g_remove(path);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check() || !GDK_IS_X11_DISPLAY(gdk_display_get_default())) return 77;
    char *xdotool = g_find_program_in_path("xdotool"); if (!xdotool) return 77; g_free(xdotool);
    root = g_dir_make_tmp("devterm-shortcut-ui-XXXXXX", NULL);
    g_test_add_func("/shortcut-ui/keys-layout-repeat", test_keys_layout);
    g_test_add_func("/shortcut-ui/live-rebind-windows", test_editor_live_rebind);
    g_test_add_func("/shortcut-ui/conflict-cancel-reset-search", test_conflict_cancel_reset);
    g_test_add_func("/shortcut-ui/copy-draft-focus", test_copy_focus_and_paste);
    g_test_add_func("/shortcut-ui/recovery-external-change", test_recovery_and_write_failure);
    g_test_add_func("/shortcut-ui/session-target-credentials", test_session_shortcuts);
    g_test_add_func("/shortcut-ui/raw-pty-input-routing", test_raw_input_routing);
    int result = g_test_run(); remove_tree(root); g_free(root); return result;
}
