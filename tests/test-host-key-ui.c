#include "../src/split-layout.h"
#include "../src/draft.h"
#include <glib/gstdio.h>
#include <string.h>

static char *root, *known, *original, *public_key;
static GtkWidget *window;
static SplitLayout *layout;
static Draft *draft;
static TerminalPane *target;
static guint requests;
static void iterate(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void settle(void) { gint64 end = g_get_monotonic_time() + 150000; while (g_get_monotonic_time() < end) iterate(); }
static GtkWidget *find(GtkWidget *root_widget, GType type, const char *name) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(root_widget, type) && (!name ||
        (GTK_IS_BUTTON(root_widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root_widget)), name) == 0))) return root_widget;
    for (GtkWidget *w = gtk_widget_get_first_child(root_widget); w; w = gtk_widget_get_next_sibling(w)) { GtkWidget *found = find(w, type, name); if (found) return found; }
    return NULL;
}
static GtkWidget *dialog(void) {
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
        GtkWidget *w = g_list_model_get_item(windows, i); gboolean match = g_strcmp0(gtk_window_get_title(GTK_WINDOW(w)), "SSH Host Key Changed") == 0;
        g_object_unref(w); if (match) return w;
    }
    return NULL;
}
static char *get(const char *path) { char *text = NULL; g_assert_true(g_file_get_contents(path, &text, NULL, NULL)); return text; }
static void put(const char *path, const char *text) { g_assert_true(g_file_set_contents(path, text, -1, NULL)); }
static char *snapshot(TerminalPane *pane) {
    GError *error = NULL; GBytes *bytes = pane_snapshot_text(pane, &error); g_assert_no_error(error);
    gsize size; const char *text = g_bytes_get_data(bytes, &size); char *copy = g_strndup(text, size); g_bytes_unref(bytes); return copy;
}
static void request(SplitLayout *owner, TerminalPane *pane, gpointer data) {
    (void)data; g_assert_true(owner == layout); g_assert_true(pane == target); requests++;
    g_assert_nonnull(pane_prepare_reconnect(pane)); pane_reconnect_ready(pane, pane_session_profile(pane), NULL); pane_finish_reconnect_request(pane);
}
static void present(void) {
    requests = 0; put(known, original);
    SessionProfile *profile = session_profile_new(); profile->type = SESSION_SSH; profile->password_auth = FALSE;
    g_free(profile->name); profile->name = g_strdup("Host key fixture");
    g_free(profile->host); profile->host = g_strdup("192.168.1.1"); g_free(profile->username); profile->username = g_strdup("tester"); profile->port = 65422;
    layout = split_layout_new_session(profile, NULL, NULL, NULL); split_layout_set_reconnect_handler(layout, request); session_profile_free(profile);
    target = split_layout_active(layout); draft = draft_new(split_layout_widget(layout), "Host review", NULL, NULL);
    window = gtk_window_new(); gtk_window_set_default_size(GTK_WINDOW(window), 1100, 800);
    gtk_window_set_child(GTK_WINDOW(window), draft_widget(draft)); gtk_window_present(GTK_WINDOW(window));
    gint64 end = g_get_monotonic_time() + 5000000;
    GtkWidget *button = find(pane_widget(target), GTK_TYPE_BUTTON, "Review Host Key…"); g_assert_nonnull(button);
    while ((pane_has_live_session(target) || !gtk_widget_get_visible(button)) && g_get_monotonic_time() < end) iterate();
    g_assert_false(pane_has_live_session(target));
    if (!gtk_widget_get_visible(button)) { char *output = snapshot(target); g_error("No review notice: %s", output); }
    settle();
}
static void destroy(void) {
    split_layout_free(layout); draft_free(draft); gtk_window_destroy(GTK_WINDOW(window)); settle();
}
static GtkWidget *review(gboolean wait) {
    g_signal_emit_by_name(find(pane_widget(target), GTK_TYPE_BUTTON, "Review Host Key…"), "clicked");
    GtkWidget *d = dialog(); g_assert_nonnull(d);
    if (wait) {
        gint64 end = g_get_monotonic_time() + 5000000; GtkWidget *check = find(d, GTK_TYPE_CHECK_BUTTON, NULL);
        while (!gtk_widget_get_sensitive(check) && g_get_monotonic_time() < end) iterate();
        g_assert_true(gtk_widget_get_sensitive(check));
    }
    return d;
}
static void accept(GtkWidget *d) {
    gtk_check_button_set_active(GTK_CHECK_BUTTON(find(d, GTK_TYPE_CHECK_BUTTON, NULL)), TRUE);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
}
static void test_cancel(void) {
    present(); GtkWidget *d = review(TRUE); GtkWidget *save = gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    g_assert_false(gtk_widget_get_sensitive(save)); gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); g_assert_true(dialog() == d);
    gtk_dialog_response(GTK_DIALOG(d), 1); g_assert_true(dialog() == d);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); settle(); g_assert_null(dialog());
    char *text = get(known); g_assert_cmpstr(text, ==, original); g_free(text); g_assert_cmpuint(requests, ==, 0); destroy();
}
static void test_accept_exact_pane(void) {
    present(); draft_set_visible(draft, TRUE); settle();
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(find(draft_widget(draft), GTK_TYPE_TEXT_VIEW, NULL)));
    gtk_text_buffer_set_text(buffer, "nháp 東京\nkeep", -1);
    split_layout_split(layout, GTK_ORIENTATION_HORIZONTAL); settle(); TerminalPane *other = split_layout_active(layout);
    GtkWidget *d = review(TRUE); accept(d);
    gint64 end = g_get_monotonic_time() + 6000000; gboolean ready = FALSE;
    while (!ready && g_get_monotonic_time() < end) { iterate(); char *text = snapshot(target); ready = strstr(text, "SSH_READY") != NULL; g_free(text); }
    g_assert_true(ready); g_assert_cmpuint(requests, ==, 1); g_assert_null(dialog());
    g_assert_true(split_layout_active(layout) == other); g_assert_cmpuint(split_layout_pane_count(layout), ==, 2);
    char *text = snapshot(target); g_assert_nonnull(strstr(text, "HOST IDENTIFICATION HAS CHANGED")); g_free(text);
    GtkTextIter start, finish; gtk_text_buffer_get_bounds(buffer, &start, &finish); text = gtk_text_buffer_get_text(buffer, &start, &finish, TRUE);
    g_assert_cmpstr(text, ==, "nháp 東京\nkeep"); g_free(text);
    text = get(known); g_assert_nonnull(strstr(text, "other.example")); g_assert_nonnull(strstr(text, public_key)); g_free(text); destroy();
}
static void test_conflict(void) {
    present(); GtkWidget *d = review(TRUE); char *changed = g_strconcat(original, "# concurrent edit\n", NULL); put(known, changed); accept(d);
    gint64 end = g_get_monotonic_time() + 5000000; while (dialog() && g_get_monotonic_time() < end) iterate();
    g_assert_null(dialog()); g_assert_cmpuint(requests, ==, 0);
    char *text = get(known); g_assert_cmpstr(text, ==, changed); g_free(text); g_free(changed); destroy();
}
static void test_close_pending(void) {
    present(); char *slow = g_build_filename(root, "slow", NULL); put(slow, "yes"); review(FALSE);
    char *scanning = g_build_filename(root, "scanning", NULL); gint64 started = g_get_monotonic_time() + 3000000;
    while (!g_file_test(scanning, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < started) iterate();
    g_assert_true(g_file_test(scanning, G_FILE_TEST_EXISTS)); g_free(scanning);
    gpointer page = pane_widget(target); g_object_add_weak_pointer(G_OBJECT(page), &page); destroy();
    gint64 end = g_get_monotonic_time() + 3000000; while (page && g_get_monotonic_time() < end) iterate(); g_assert_null(page); g_assert_null(dialog());
    g_unlink(slow); g_free(slow); char *text = get(known); g_assert_cmpstr(text, ==, original); g_free(text);
    present(); review(TRUE); destroy(); g_assert_null(dialog());
}
static char *command(const char *const *args) {
    GSubprocess *process = g_subprocess_newv(args, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, NULL); g_assert_nonnull(process);
    char *out = NULL, *err = NULL; g_assert_true(g_subprocess_communicate_utf8(process, NULL, NULL, &out, &err, NULL));
    if (!g_subprocess_get_successful(process)) g_error("Fixture command failed: %s", err);
    g_free(err); g_object_unref(process); return out;
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL); if (!gtk_init_check()) return 77;
    char *keygen = g_find_program_in_path("ssh-keygen"); if (!keygen) return 77; g_free(keygen);
    root = g_dir_make_tmp("devterm-host-ui-XXXXXX", NULL); known = g_build_filename(root, "known_hosts", NULL);
    char *key = g_build_filename(root, "key", NULL); const char *gen[] = {"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", key, NULL}; char *out = command(gen); g_free(out);
    char *pub = g_strconcat(key, ".pub", NULL); out = get(pub); char **parts = g_strsplit(out, " ", -1); public_key = g_strdup_printf("%s %s", parts[0], parts[1]); g_strfreev(parts); g_free(out);
    const char *fp_args[] = {"ssh-keygen", "-lf", pub, "-E", "sha256", NULL}; out = command(fp_args); parts = g_strsplit(out, " ", -1); char *fingerprint = g_strdup(parts[1]); g_strfreev(parts); g_free(out);
    char *old_path = g_build_filename(root, "old", NULL); const char *old_gen[] = {"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", old_path, NULL}; out = command(old_gen); g_free(out);
    char *old_pub = g_strconcat(old_path, ".pub", NULL), *old_text = get(old_pub);
    original = g_strdup_printf("# header\n[192.168.1.1]:65422 %sother.example %s", old_text, old_text); g_free(old_text);
    char *diag = g_strdup_printf("WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!\nThe fingerprint for the ED25519 key sent by the remote host is\n%s\nOffending ED25519 key in %s:2\nHost key verification failed.\n", fingerprint, known);
    char *path = g_build_filename(root, "diagnostics", NULL); put(path, diag); g_free(path); g_free(diag); g_free(fingerprint);
    path = g_build_filename(root, "bin", NULL); g_mkdir(path, 0700);
    char *script = g_build_filename(path, "ssh", NULL);
    put(script, "#!/usr/bin/python3\nimport os,sys\nfrom pathlib import Path\nr=Path(__file__).parent.parent\nif '-G' in sys.argv:\n print('hostname 192.168.1.1\\nport 65422\\nuserknownhostsfile '+str(r/'known_hosts'));sys.exit(0)\nkey=(r/'key.pub').read_text().split();key=' '.join(key[:2])\nif key not in (r/'known_hosts').read_text():\n with open(sys.argv[sys.argv.index('-E')+1],'w') as f:f.write((r/'diagnostics').read_text())\n sys.exit(255)\nprint('SSH_READY',flush=True)\nfor line in sys.stdin:\n if line.strip()=='exit':break\n print('RESULT:'+line,flush=True)\n"); g_chmod(script, 0700); g_free(script);
    script = g_build_filename(path, "ssh-keyscan", NULL);
    put(script, "#!/usr/bin/python3\nfrom pathlib import Path\nimport os,time\nr=Path(__file__).parent.parent\nif (r/'slow').exists():\n (r/'scanning').write_text('started');time.sleep(20)\nprint('[192.168.1.1]:65422 '+(r/'key.pub').read_text())\n"); g_chmod(script, 0700); g_free(script);
    char *env = g_strconcat(path, ":", g_getenv("PATH"), NULL); g_setenv("PATH", env, TRUE); g_free(env); g_free(path);
    g_test_add_func("/host-ui/cancel-default-copy", test_cancel);
    g_test_add_func("/host-ui/accept-exact-pane-state", test_accept_exact_pane);
    g_test_add_func("/host-ui/concurrent-edit", test_conflict);
    g_test_add_func("/host-ui/close-pending", test_close_pending);
    int result = g_test_run(); const char *remove[] = {"rm", "-rf", "--", root, NULL}; out = command(remove); g_free(out);
    g_free(root); g_free(known); g_free(original); g_free(public_key); g_free(key); g_free(pub); g_free(old_path); g_free(old_pub); return result;
}
