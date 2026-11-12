#include "../src/split-layout.h"
#include "../src/sessions-ui.h"
#include "../src/draft.h"
#include <glib/gstdio.h>
#include <string.h>

/* Exercise real VTE/PTY/helper lifetimes with deterministic client fixtures.
 * Protocol authentication itself is covered by test-connections.py. */
static GtkWindow *window;
static char *root, *trace_path;
static guint prepared, released, unexpected_tabs;

static void iterate(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void settle(void) {
    gint64 end = g_get_monotonic_time() + 200 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < end) iterate();
}
static GtkWidget *find(GtkWidget *w, GType type) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(w, type)) return w;
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *found = find(c, type); if (found) return found;
    }
    return NULL;
}
static GtkWidget *password_dialog(void) {
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
        GtkWidget *w = g_list_model_get_item(windows, i);
        gboolean match = gtk_widget_get_visible(w) && g_strcmp0(gtk_window_get_title(GTK_WINDOW(w)), "Session Password") == 0;
        g_object_unref(w); if (match) return w;
    }
    return NULL;
}
static char *snapshot(TerminalPane *p) {
    GError *error = NULL; GBytes *bytes = pane_snapshot_text(p, &error);
    g_assert_no_error(error); g_assert_nonnull(bytes);
    gsize size; const char *data = g_bytes_get_data(bytes, &size);
    char *text = size ? g_strndup(data, size) : g_strdup(""); g_bytes_unref(bytes); return text;
}
static guint count(const char *text, const char *needle) {
    guint result = 0;
    while ((text = strstr(text, needle))) { result++; text += strlen(needle); }
    return result;
}
static char *trace(void) {
    char *text = NULL; g_file_get_contents(trace_path, &text, NULL, NULL); return text ? text : g_strdup("");
}
static guint starts(void) { char *text = trace(); guint n = count(text, "START "); g_free(text); return n; }
static void wait_ready(TerminalPane *p, guint expected) {
    gint64 end = g_get_monotonic_time() + 7 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < end) {
        iterate(); char *text = snapshot(p);
        gboolean ready = pane_is_running(p) && !pane_is_reconnecting(p) && count(text, "READY pid=") >= expected;
        g_free(text); if (ready) return;
    }
    char *text = snapshot(p); g_error("Pane did not become ready: %s", text);
}
static void wait_stopped(TerminalPane *p) {
    gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (pane_has_live_session(p) && g_get_monotonic_time() < end) iterate();
    g_assert_false(pane_has_live_session(p));
}
static void wait_clients_closed(void) {
    gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < end) {
        iterate(); char *text = trace();
        g_assert_null(strstr(text, "OVERLAP"));
        gboolean done = count(text, "START ") == count(text, "STOP "); g_free(text);
        if (done) { settle(); return; }
    }
    g_error("A fixture client survived pane closure");
}
static SessionProfile *profile(SessionType type, gboolean password) {
    SessionProfile *p = session_profile_new(); p->type = type; p->password_auth = password; p->auto_login = FALSE;
    g_free(p->name); p->name = g_strdup("Reconnect fixture");
    g_free(p->host); p->host = g_strdup("snapshot.example");
    g_free(p->username); p->username = g_strdup("tester");
    g_free(p->device); p->device = g_strdup("/dev/test-serial");
    return p;
}
static TerminalPane *new_pane(SessionProfile *p) {
    TerminalPane *pane = pane_new_session(p, "fixture-password", NULL, NULL, NULL);
    gtk_window_set_child(window, pane_widget(pane)); gtk_window_present(window); wait_ready(pane, 1); return pane;
}
static void close_pane(TerminalPane *p) {
    gtk_window_set_focus(window, NULL); gtk_window_set_child(window, NULL); pane_close(p); wait_clients_closed();
}
static void ready(const SessionProfile *p, const char *password, gpointer data) {
    prepared++; pane_reconnect_ready(data, p, password);
}
static void request_done(gpointer data) { released++; pane_finish_reconnect_request(data); pane_unref(data); }
static void new_tab(const SessionProfile *p, const char *password, gpointer data) {
    (void)p; (void)password; (void)data; unexpected_tabs++;
}
static void current(gboolean reconnect, gpointer data) { (void)reconnect; (void)data; }
static SessionManager *manager(void) { return session_manager_new(window, new_tab, current, NULL); }
static void request(SessionManager *m, TerminalPane *p) {
    GCancellable *cancel = pane_prepare_reconnect(p); g_assert_nonnull(cancel);
    session_manager_connect_to(m, pane_session_profile(p), cancel, ready, pane_ref(p), request_done);
}
static void reconnect(TerminalPane *p) {
    g_assert_nonnull(pane_prepare_reconnect(p));
    pane_reconnect_ready(p, pane_session_profile(p), "fixture-password"); pane_finish_reconnect_request(p);
}

static void test_layout_history(void) {
    SessionProfile *p = profile(SESSION_SSH, FALSE);
    SplitLayout *layout = split_layout_new_session(p, NULL, NULL, NULL);
    TerminalPane *original = split_layout_active(layout);
    Draft *draft = draft_new(split_layout_widget(layout), "Test", NULL, NULL);
    GtkWidget *notebook = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), draft_widget(draft), gtk_label_new("Session"));
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), gtk_label_new("Other tab"), gtk_label_new("Other"));
    gtk_window_set_child(window, notebook); gtk_window_present(window); wait_ready(original, 1);
    draft_set_visible(draft, TRUE);
    GtkTextView *editor = GTK_TEXT_VIEW(find(draft_widget(draft), GTK_TYPE_TEXT_VIEW));
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(editor), "Nháp còn nguyên 日本語", -1);
    split_layout_split(layout, GTK_ORIENTATION_HORIZONTAL); settle();
    GtkWidget *pane_root = pane_widget(original), *vte = find(pane_root, VTE_TYPE_TERMINAL);
    GtkPaned *paned = GTK_PANED(gtk_widget_get_first_child(split_layout_widget(layout)));
    gtk_paned_set_position(paned, 350); settle(); int divider = gtk_paned_get_position(paned);
    int width = gtk_widget_get_width(pane_root);
    GString *history = g_string_new("\r\nSCROLLBACK-KEEP Đường 日本語\r\n");
    for (guint i = 0; i < 250; i++) g_string_append_printf(history, "history line %u\r\n", i);
    vte_terminal_feed(VTE_TERMINAL(vte), history->str, history->len); g_string_free(history, TRUE); settle();
    /* Editing/deleting the source profile must not change the live snapshot. */
    g_free(p->host); p->host = g_strdup("changed.example"); session_profile_free(p);
    for (guint i = 2; i <= 4; i++) {
        reconnect(original); g_assert_true(pane_is_reconnecting(original));
        g_assert_null(pane_prepare_reconnect(original));
        wait_ready(original, i);
        g_assert_true(pane_widget(original) == pane_root); g_assert_true(find(pane_root, VTE_TYPE_TERMINAL) == vte);
        g_assert_cmpuint(split_layout_pane_count(layout), ==, 2);
        g_assert_cmpint(gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebook)), ==, 2);
        g_assert_cmpint(gtk_paned_get_position(paned), ==, divider);
        g_assert_cmpint(gtk_widget_get_width(pane_root), ==, width);
    }
    char *text = snapshot(original); g_assert_cmpuint(count(text, "READY pid="), ==, 4);
    g_assert_nonnull(strstr(text, "SCROLLBACK-KEEP Đường 日本語"));
    g_assert_nonnull(strstr(text, "snapshot.example")); g_assert_null(strstr(text, "changed.example")); g_free(text);
    GtkTextIter first, last; GtkTextBuffer *buffer = gtk_text_view_get_buffer(editor);
    gtk_text_buffer_get_bounds(buffer, &first, &last); text = gtk_text_buffer_get_text(buffer, &first, &last, TRUE);
    g_assert_cmpstr(text, ==, "Nháp còn nguyên 日本語"); g_free(text);
    g_assert_true(pane_copy_all_text(original, NULL));
    /* Search old output after reconnect. */
    pane_search_open(original); GtkWidget *entry = find(pane_root, GTK_TYPE_ENTRY);
    gtk_editable_set_text(GTK_EDITABLE(entry), "snapshot.example"); settle();
    text = vte_terminal_get_text_selected(VTE_TERMINAL(vte), VTE_FORMAT_TEXT);
    g_assert_cmpstr(text, ==, "snapshot.example"); g_free(text);
    gtk_window_set_focus(window, NULL); gtk_window_set_child(window, NULL);
    draft_free(draft); split_layout_free(layout); wait_clients_closed();
}
static void test_protocol(gconstpointer data) {
    SessionProfile *p = profile(GPOINTER_TO_INT(data), FALSE); TerminalPane *pane = new_pane(p);
    /* Reconnect must recover the normal screen and input modes after a TUI. */
    VteTerminal *vte = VTE_TERMINAL(find(pane_widget(pane), VTE_TYPE_TERMINAL));
    vte_terminal_feed(vte, "\033[?1049h\033[?1000h\033[?2004h\033[?25lold alternate screen", -1); settle();
    reconnect(pane); wait_ready(pane, 2);
    pane_disconnect(pane); wait_stopped(pane);
    reconnect(pane); wait_ready(pane, 3);
    close_pane(pane); session_profile_free(p);
}
static void test_cancel_password(void) {
    SessionProfile *p = profile(SESSION_SSH, TRUE); TerminalPane *pane = new_pane(p); SessionManager *m = manager();
    guint before = starts(), callbacks = prepared; request(m, pane);
    g_assert_true(pane_has_live_session(pane)); g_assert_false(pane_is_running(pane));
    g_assert_false(vte_terminal_get_input_enabled(VTE_TERMINAL(find(pane_widget(pane), VTE_TYPE_TERMINAL))));
    pane_send(pane, "SHOULD-NOT-ARRIVE", TRUE);
    GtkWidget *dialog = password_dialog(); g_assert_nonnull(dialog);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); settle();
    g_assert_null(password_dialog()); g_assert_false(pane_is_reconnecting(pane)); g_assert_true(pane_is_running(pane));
    g_assert_cmpuint(starts(), ==, before); g_assert_cmpuint(prepared, ==, callbacks);
    pane_send(pane, "after-cancel", TRUE); settle(); char *text = snapshot(pane);
    g_assert_nonnull(strstr(text, "RECEIVED:after-cancel")); g_assert_null(strstr(text, "SHOULD-NOT-ARRIVE")); g_free(text);
    session_manager_free(m); close_pane(pane); session_profile_free(p);
}
static void test_password_target(void) {
    SessionProfile *p = profile(SESSION_SSH, TRUE);
    SplitLayout *layout = split_layout_new_session(p, "fixture-password", NULL, NULL);
    TerminalPane *target = split_layout_active(layout); gtk_window_set_child(window, split_layout_widget(layout));
    gtk_window_present(window); wait_ready(target, 1); SessionManager *m = manager();
    request(m, target); GtkWidget *dialog = password_dialog(); g_assert_nonnull(dialog);
    split_layout_split(layout, GTK_ORIENTATION_VERTICAL); TerminalPane *other = split_layout_active(layout);
    GtkWidget *password = find(dialog, GTK_TYPE_PASSWORD_ENTRY); gtk_editable_set_text(GTK_EDITABLE(password), "new-fixture-password");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); wait_ready(target, 2);
    g_assert_true(split_layout_active(layout) == other); g_assert_cmpuint(split_layout_pane_count(layout), ==, 2);
    char *text = snapshot(other); g_assert_null(strstr(text, "READY pid=")); g_free(text);
    g_assert_cmpuint(unexpected_tabs, ==, 0); session_manager_free(m);
    gtk_window_set_focus(window, NULL); gtk_window_set_child(window, NULL); split_layout_free(layout);
    wait_clients_closed(); session_profile_free(p);
}
static void test_close_pending(void) {
    SessionProfile *p = profile(SESSION_SSH, TRUE); TerminalPane *pane = new_pane(p); SessionManager *m = manager();
    guint before = prepared, done = released; request(m, pane);
    g_assert_nonnull(password_dialog()); close_pane(pane);
    g_assert_null(password_dialog()); g_assert_cmpuint(prepared, ==, before); g_assert_cmpuint(released, ==, done + 1);
    /* A cancelled keyring lookup must not later launch or display a dialog. */
    p->remember = TRUE; pane = new_pane(p); request(m, pane); close_pane(pane); settle();
    g_assert_null(password_dialog()); g_assert_cmpuint(prepared, ==, before);
    /* Manager shutdown while credentials are pending restores the old child. */
    p->remember = FALSE; pane = new_pane(p); request(m, pane); session_manager_free(m); settle();
    g_assert_null(password_dialog()); g_assert_false(pane_is_reconnecting(pane)); g_assert_true(pane_is_running(pane));
    close_pane(pane); session_profile_free(p);
}
static void test_fail_and_retry(void) {
    SessionProfile *p = profile(SESSION_SSH, FALSE); TerminalPane *pane = new_pane(p);
    SessionProfile *bad = session_profile_copy(p); bad->port = 0;
    g_assert_nonnull(pane_prepare_reconnect(pane)); pane_reconnect_ready(pane, bad, NULL);
    pane_finish_reconnect_request(pane); g_assert_true(pane_is_running(pane));
    /* Kill the fixture via normal input to simulate network/session failure. */
    pane_send(pane, "exit-fixture", TRUE); wait_stopped(pane);
    reconnect(pane); wait_ready(pane, 2); close_pane(pane);
    session_profile_free(bad); session_profile_free(p);
}
static void test_early_reconnect_close(void) {
    SessionProfile *p = profile(SESSION_SSH, FALSE);
    for (guint i = 0; i < 12; i++) {
        TerminalPane *pane = pane_new_session(p, NULL, NULL, NULL, NULL);
        reconnect(pane);
        if (i % 3 == 0) {
            gtk_window_set_child(window, pane_widget(pane)); gtk_window_present(window); wait_ready(pane, 1);
            reconnect(pane);
        }
        if (i % 3 == 1) g_main_context_iteration(NULL, FALSE);
        if (gtk_window_get_child(window)) gtk_window_set_child(window, NULL);
        pane_close(pane); settle();
    }
    wait_clients_closed(); session_profile_free(p);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL); root = g_dir_make_tmp("devterm-reconnect-XXXXXX", NULL);
    trace_path = g_build_filename(root, "trace", NULL);
    const char *fixture =
        "#!/usr/bin/python3\nimport fcntl, os, signal, sys, time\n"
        "root=os.path.dirname(__file__)\nlock=open(root+'/lock','w')\n"
        "def record(s):\n with open(root+'/trace','a') as f: f.write(s+' '+str(os.getpid())+'\\n')\n"
        "try: fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)\n"
        "except BlockingIOError: record('OVERLAP');sys.exit(2)\n"
        "def stop(*args):\n signal.signal(signal.SIGTERM,signal.SIG_IGN)\n signal.signal(signal.SIGHUP,signal.SIG_IGN)\n time.sleep(.15)\n record('STOP')\n sys.exit(0)\n"
        "signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGHUP,stop)\n"
        "record('START')\nprint('READY pid='+str(os.getpid()),flush=True)\nprint('ARGS '+'|'.join(sys.argv[1:]),flush=True)\n"
        "while True:\n line=sys.stdin.readline()\n"
        " if not line or line.strip()=='exit-fixture': stop()\n"
        " print('RECEIVED:'+line.strip(),flush=True)\n";
    const char *names[] = {"ssh", "telnet", "picocom"};
    for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
        char *path = g_build_filename(root, names[i], NULL); g_assert_true(g_file_set_contents(path, fixture, -1, NULL));
        g_assert_cmpint(g_chmod(path, 0700), ==, 0); g_free(path);
    }
    char *path = g_strconcat(root, ":", g_getenv("PATH"), NULL); g_setenv("PATH", path, TRUE); g_free(path);
    path = g_build_filename(root, "config", NULL); g_setenv("XDG_CONFIG_HOME", path, TRUE); g_free(path);
    if (!gtk_init_check()) return 77;
    window = GTK_WINDOW(gtk_window_new()); gtk_window_set_default_size(window, 1000, 700);
    g_test_add_func("/reconnect/layout-history-draft", test_layout_history);
    g_test_add_data_func("/reconnect/ssh", GINT_TO_POINTER(SESSION_SSH), test_protocol);
    g_test_add_data_func("/reconnect/telnet", GINT_TO_POINTER(SESSION_TELNET), test_protocol);
    g_test_add_data_func("/reconnect/serial", GINT_TO_POINTER(SESSION_SERIAL), test_protocol);
    g_test_add_func("/reconnect/cancel-password", test_cancel_password);
    g_test_add_func("/reconnect/password-target", test_password_target);
    g_test_add_func("/reconnect/close-pending", test_close_pending);
    g_test_add_func("/reconnect/fail-retry", test_fail_and_retry);
    g_test_add_func("/reconnect/early-close", test_early_reconnect_close);
    int result = g_test_run(); gtk_window_destroy(window);
    g_assert_cmpuint(unexpected_tabs, ==, 0);
    const char *files[] = {"ssh", "telnet", "picocom", "trace", "lock"};
    for (guint i = 0; i < G_N_ELEMENTS(files); i++) { path = g_build_filename(root, files[i], NULL); g_remove(path); g_free(path); }
    g_rmdir(root); g_free(root); g_free(trace_path); return result;
}
