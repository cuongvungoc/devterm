#include "../src/sessions-ui.h"
#include "../src/credentials.h"
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>

static GtkWindow *parent;
static char *root;
static guint connections;
static char *connected_password;
static SessionProfile *connected_profile;
static void iterate(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void connected(const SessionProfile *p, const char *password, gpointer data) {
    (void)data; connections++; session_profile_free(connected_profile); connected_profile = session_profile_copy(p);
    session_secret_free(connected_password); connected_password = g_strdup(password);
}
static void current(gboolean reconnect, gpointer data) { (void)reconnect; (void)data; }
static GtkWidget *top(const char *title) {
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
        GtkWidget *w = g_list_model_get_item(windows, i);
        gboolean found = gtk_widget_get_visible(w) && g_strcmp0(gtk_window_get_title(GTK_WINDOW(w)), title) == 0;
        g_object_unref(w); if (found) return w;
    }
    return NULL;
}
static GtkWidget *find(GtkWidget *root_widget, GType type, const char *label) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(root_widget, type) && (!label || (GTK_IS_BUTTON(root_widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root_widget)), label) == 0))) return root_widget;
    for (GtkWidget *w = gtk_widget_get_first_child(root_widget); w; w = gtk_widget_get_next_sibling(w)) {
        GtkWidget *found = find(w, type, label); if (found) return found;
    }
    return NULL;
}
static GtkWidget *field(GtkWidget *root_widget, const char *name) {
    if (GTK_IS_LABEL(root_widget) && g_strcmp0(gtk_label_get_text(GTK_LABEL(root_widget)), name) == 0)
        return gtk_widget_get_next_sibling(root_widget);
    for (GtkWidget *w = gtk_widget_get_first_child(root_widget); w; w = gtk_widget_get_next_sibling(w)) {
        GtkWidget *found = field(w, name); if (found) return found;
    }
    return NULL;
}
static void click(GtkWidget *w, const char *label) {
    GtkWidget *button = find(w, GTK_TYPE_BUTTON, label); g_assert_nonnull(button); g_signal_emit_by_name(button, "clicked");
}
typedef struct { gboolean done; char *secret; GError *error; } Lookup;
static void looked_up(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source; Lookup *lookup = data; lookup->secret = credential_lookup_finish(result, &lookup->error); lookup->done = TRUE;
}
static char *lookup(const SessionProfile *p) {
    Lookup result = {0}; credential_lookup(p, NULL, looked_up, &result);
    gint64 deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    while (!result.done && g_get_monotonic_time() < deadline) iterate();
    g_assert_true(result.done); g_assert_no_error(result.error); return result.secret;
}
static void test_profile_keyring_lifecycle(void) {
    SessionManager *manager = session_manager_new(parent, connected, current, NULL);
    session_manager_show(manager); GtkWidget *window = top("Sessions"); g_assert_nonnull(window); click(window, "Add");
    GtkWidget *editor = top("Session Profile"); g_assert_nonnull(editor);
    gtk_editable_set_text(GTK_EDITABLE(field(editor, "Name")), "Router Đường 日本語");
    gtk_editable_set_text(GTK_EDITABLE(field(editor, "Host")), "127.0.0.1");
    gtk_editable_set_text(GTK_EDITABLE(field(editor, "Username")), "tester");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(field(editor, "Remember in system keyring")), TRUE);
    gtk_editable_set_text(GTK_EDITABLE(field(editor, "Password (keyring only)")), "dummy-ui-secret-Đường");
    gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_ACCEPT); g_assert_null(top("Session Profile"));
    char *path = sessions_config_path(); GError *error = NULL; GPtrArray *profiles = sessions_load(path, &error);
    g_assert_no_error(error); g_assert_cmpuint(profiles->len, ==, 1); SessionProfile *p = profiles->pdata[0];
    g_assert_cmpstr(p->name, ==, "Router Đường 日本語"); g_assert_true(p->remember);
    char *secret = lookup(p); g_assert_cmpstr(secret, ==, "dummy-ui-secret-Đường"); session_secret_free(secret);
    char *json = NULL; g_file_get_contents(path, &json, NULL, NULL); g_assert_null(strstr(json, "dummy-ui-secret")); g_free(json);
    /* A remembered secret is returned to Connect without a password dialog. */
    session_manager_connect(manager, p);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!connections && g_get_monotonic_time() < deadline) iterate();
    g_assert_cmpuint(connections, ==, 1); g_assert_cmpstr(connected_password, ==, "dummy-ui-secret-Đường");
    g_assert_null(top("Session Password")); g_assert_cmpstr(connected_profile->id, ==, p->id);
    session_manager_show(manager); window = top("Sessions");
    GtkListBox *list = GTK_LIST_BOX(find(window, GTK_TYPE_LIST_BOX, NULL));
    gtk_list_box_select_row(list, gtk_list_box_get_row_at_index(list, 0)); click(window, "Duplicate");
    editor = top("Session Profile"); g_assert_nonnull(editor);
    g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(field(editor, "Remember in system keyring"))));
    gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_ACCEPT);
    GPtrArray *copies = sessions_load(path, &error); g_assert_no_error(error); g_assert_cmpuint(copies->len, ==, 2);
    SessionProfile *duplicate = copies->pdata[1]; g_assert_cmpstr(duplicate->id, !=, p->id); g_assert_false(duplicate->remember);
    secret = lookup(duplicate); g_assert_null(secret);
    list = GTK_LIST_BOX(find(window, GTK_TYPE_LIST_BOX, NULL)); gtk_list_box_select_row(list, gtk_list_box_get_row_at_index(list, 0)); click(window, "Delete");
    secret = lookup(p); g_assert_null(secret);
    /* Remembering a password in the Connect dialog must update the live
     * profile too; a subsequent reconnect should not ask for it again. */
    session_manager_connect(manager, duplicate);
    GtkWidget *password_dialog = top("Session Password"); g_assert_nonnull(password_dialog);
    gtk_editable_set_text(GTK_EDITABLE(find(password_dialog, GTK_TYPE_PASSWORD_ENTRY, NULL)), "dummy-connect-secret");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(find(password_dialog, GTK_TYPE_CHECK_BUTTON, NULL)), TRUE);
    gtk_dialog_response(GTK_DIALOG(password_dialog), GTK_RESPONSE_ACCEPT);
    g_assert_cmpuint(connections, ==, 2); g_assert_true(connected_profile->remember);
    secret = lookup(duplicate); g_assert_cmpstr(secret, ==, "dummy-connect-secret"); session_secret_free(secret);
    session_manager_connect(manager, connected_profile);
    deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (connections < 3 && g_get_monotonic_time() < deadline) iterate();
    g_assert_cmpuint(connections, ==, 3); g_assert_null(top("Session Password"));
    session_manager_show(manager); window = top("Sessions"); list = GTK_LIST_BOX(find(window, GTK_TYPE_LIST_BOX, NULL));
    gtk_list_box_select_row(list, gtk_list_box_get_row_at_index(list, 0)); click(window, "Delete");
    secret = lookup(duplicate); g_assert_null(secret);
    g_ptr_array_unref(copies); g_ptr_array_unref(profiles); g_free(path);
    session_manager_free(manager);
}
static void test_cancel_and_shutdown(void) {
    SessionProfile *p = session_profile_new(); g_free(p->name); p->name = g_strdup("One-time password"); g_free(p->host); p->host = g_strdup("localhost");
    SessionManager *manager = session_manager_new(parent, connected, current, NULL);
    guint before = connections; session_manager_connect(manager, p);
    GtkWidget *dialog = top("Session Password"); g_assert_nonnull(dialog); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    g_assert_cmpuint(connections, ==, before);
    /* A pending lookup must never connect after the manager shuts down. */
    p->remember = TRUE; session_manager_connect(manager, p); session_manager_free(manager);
    gint64 deadline = g_get_monotonic_time() + 250 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < deadline) iterate();
    g_assert_cmpuint(connections, ==, before); session_profile_free(p);
}
static void test_invalid_config_is_preserved(void) {
    char *path = sessions_config_path(); const char *broken = "{invalid sessions";
    g_assert_true(g_file_set_contents(path, broken, -1, NULL));
    SessionManager *manager = session_manager_new(parent, connected, current, NULL); session_manager_show(manager);
    GtkWidget *window = top("Sessions"), *add = find(window, GTK_TYPE_BUTTON, "Add");
    g_assert_false(gtk_widget_get_sensitive(add)); session_manager_free(manager);
    char *text = NULL; g_file_get_contents(path, &text, NULL, NULL); g_assert_cmpstr(text, ==, broken); g_free(text); g_free(path);
}
static GSubprocess *start_keyring(void) {
    char *daemon = g_find_program_in_path("gnome-keyring-daemon"); if (!daemon) return NULL;
    char *control = g_build_filename(root, "keyring-control", NULL); g_mkdir(control, 0700);
    GError *error = NULL;
    GSubprocess *process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
        &error, daemon, "--foreground", "--unlock", "--components=secrets", "--control-directory", control, NULL);
    g_free(control); g_free(daemon); if (!process) { g_clear_error(&error); return NULL; }
    GOutputStream *input = g_subprocess_get_stdin_pipe(process);
    g_output_stream_write_all(input, "fixture-keyring-password\n", 25, NULL, NULL, NULL); g_output_stream_close(input, NULL, NULL);
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!bus) { g_subprocess_force_exit(process); g_object_unref(process); return NULL; }
    gboolean ready = FALSE; gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!ready && g_get_monotonic_time() < deadline) {
        GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
            g_variant_new("(s)", "org.freedesktop.secrets"), G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
        if (reply) { g_variant_get(reply, "(b)", &ready); g_variant_unref(reply); } iterate();
    }
    g_object_unref(bus);
    if (!ready) { g_subprocess_force_exit(process); g_object_unref(process); return NULL; }
    return process;
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    root = g_dir_make_tmp("devterm-session-ui-XXXXXX", NULL);
    char *config = g_build_filename(root, "config", NULL), *data = g_build_filename(root, "data", NULL);
    g_setenv("XDG_CONFIG_HOME", config, TRUE); g_setenv("XDG_DATA_HOME", data, TRUE); g_free(config); g_free(data);
    if (!gtk_init_check()) { g_free(root); return 77; }
    GSubprocess *keyring = start_keyring(); if (!keyring) { g_test_message("No isolated Secret Service available"); g_free(root); return 77; }
    parent = GTK_WINDOW(gtk_window_new()); gtk_window_set_default_size(parent, 800, 500); gtk_window_present(parent);
    g_test_add_func("/session-ui/profile-keyring-lifecycle", test_profile_keyring_lifecycle);
    g_test_add_func("/session-ui/cancel-shutdown", test_cancel_and_shutdown);
    g_test_add_func("/session-ui/invalid-config", test_invalid_config_is_preserved);
    int result = g_test_run(); gtk_window_destroy(parent);
    session_profile_free(connected_profile); session_secret_free(connected_password);
    g_subprocess_send_signal(keyring, SIGTERM); g_subprocess_wait(keyring, NULL, NULL); g_object_unref(keyring);
    g_free(root); return result;
}
