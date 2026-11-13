#include "../src/ssh-host-key.h"
#include <glib/gstdio.h>
#include <string.h>

static char *root, *path, *config_path, *scan_path, *old_key, *new_key, *new_fp;
static SessionProfile *profile;
static void put(const char *file, const char *text) { GError *error = NULL; g_assert_true(g_file_set_contents(file, text, -1, &error)); g_assert_no_error(error); }
static char *get(const char *file) { char *text = NULL; GError *error = NULL; g_assert_true(g_file_get_contents(file, &text, NULL, &error)); g_assert_no_error(error); return text; }
static char *command(const char *const *args) {
    GError *error = NULL; GSubprocess *process = g_subprocess_newv(args, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    g_assert_no_error(error); char *out = NULL, *err = NULL;
    g_assert_true(g_subprocess_communicate_utf8(process, NULL, NULL, &out, &err, &error)); g_assert_no_error(error);
    if (!g_subprocess_get_successful(process)) g_error("Command failed: %s", err);
    g_free(err); g_object_unref(process); return out;
}
static void configuration(const char *host, int port, const char *extra) {
    char *text = g_strdup_printf("hostname %s\nport %d\nuserknownhostsfile %s\n%s", host, port, path, extra ? extra : "");
    put(config_path, text); g_free(text);
}
static void reset(void) {
    configuration("192.168.1.1", 65422, NULL); g_chmod(path, 0600);
    char *text = g_strdup_printf("# giữ nguyên 東京\n[192.168.1.1]:65422 %sother.example %s[192.168.1.1]:22 %s", old_key, new_key, old_key);
    put(path, text); g_free(text);
    text = g_strdup_printf("[192.168.1.1]:65422 %s", new_key); put(scan_path, text); g_free(text);
}
static char *diagnostics(const char *file, int line) {
    return g_strdup_printf("WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!\nThe fingerprint for the ED25519 key sent by the remote host is\n%s\nOffending ED25519 key in %s:%d\nHost key verification failed.\n", new_fp, file, line);
}
static SshHostKeyReview *prepare(GError **error) { char *text = diagnostics(path, 2); SshHostKeyReview *r = ssh_host_key_prepare(profile, text, NULL, error); g_free(text); return r; }
static void test_replace(void) {
    reset(); char *before = get(path); GError *error = NULL; SshHostKeyReview *r = prepare(&error);
    g_assert_no_error(error); g_assert_nonnull(r); g_assert_cmpstr(r->fingerprint, ==, new_fp);
    char *text = get(path); g_assert_cmpstr(text, ==, before); g_free(text); g_assert_false(g_file_test(r->backup, G_FILE_TEST_EXISTS));
    g_assert_true(ssh_host_key_replace(r, NULL, &error)); g_assert_no_error(error);
    text = get(r->backup); g_assert_cmpstr(text, ==, before); g_free(text);
    text = get(path); g_assert_nonnull(strstr(text, "# giữ nguyên 東京\n"));
    char *other = g_strdup_printf("other.example %s[192.168.1.1]:22 %s", new_key, old_key); g_assert_nonnull(strstr(text, other)); g_free(other);
    other = g_strdup_printf("[192.168.1.1]:65422 %s", new_key); g_assert_nonnull(strstr(text, other)); g_free(other); g_free(text);
    GStatBuf st; g_assert_cmpint(g_stat(r->backup, &st), ==, 0); g_assert_cmpint(st.st_mode & 0777, ==, 0600);
    ssh_host_key_unref(r); g_free(before);
}
static void test_hash_and_aliases(void) {
    reset(); char *text = g_strdup_printf("# header\n[192.168.1.1]:65422 %sother.example %s", old_key, new_key); put(path, text); g_free(text);
    const char *hash[] = {"ssh-keygen", "-H", "-f", path, NULL}; text = command(hash); g_free(text);
    char *before = get(path); g_assert_nonnull(strstr(before, "|1|")); GError *error = NULL;
    SshHostKeyReview *r = prepare(&error); g_assert_no_error(error); g_assert_nonnull(r);
    g_assert_true(ssh_host_key_replace(r, NULL, &error)); g_assert_no_error(error);
    const char *find[] = {"ssh-keygen", "-F", "other.example", "-f", path, NULL}; text = command(find); g_assert_nonnull(strstr(text, "|1|")); g_free(text); ssh_host_key_unref(r); g_free(before);
    reset(); text = g_strdup_printf("# header\n[192.168.1.1]:65422,another.example %s", old_key); put(path, text); g_free(text);
    r = prepare(&error); g_assert_no_error(error); g_assert_nonnull(r); g_assert_true(ssh_host_key_replace(r, NULL, &error)); g_assert_no_error(error);
    text = get(path); char *alias = g_strdup_printf("another.example %s", old_key); g_assert_nonnull(strstr(text, alias)); g_free(alias); g_free(text); ssh_host_key_unref(r);
}
static void test_cancel_and_conflict(void) {
    reset(); GError *error = NULL; SshHostKeyReview *r = prepare(&error); g_assert_no_error(error);
    char *before = get(path); GCancellable *cancel = g_cancellable_new(); g_cancellable_cancel(cancel);
    g_assert_false(ssh_host_key_replace(r, cancel, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&error);
    g_assert_false(g_file_test(r->backup, G_FILE_TEST_EXISTS)); char *text = get(path); g_assert_cmpstr(text, ==, before); g_free(text); g_object_unref(cancel);
    char *changed = g_strconcat(before, "# another application updated this file\n", NULL); put(path, changed);
    g_assert_false(ssh_host_key_replace(r, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
    text = get(path); g_assert_cmpstr(text, ==, changed); g_assert_false(g_file_test(r->backup, G_FILE_TEST_EXISTS));
    g_free(text); g_free(changed); g_free(before); ssh_host_key_unref(r);
}
static void test_candidate_changed(void) {
    reset(); put(scan_path, "# server unavailable or has changed again\n"); char *before = get(path); GError *error = NULL;
    g_assert_null(prepare(&error)); g_assert_nonnull(error); g_clear_error(&error);
    char *text = get(path); g_assert_cmpstr(text, ==, before); g_free(text); g_free(before);
    reset(); text = g_strdup_printf("# header\n[192.168.1.1]:65422 %s", new_key); put(path, text); g_free(text);
    g_assert_null(prepare(&error)); g_assert_nonnull(error); g_clear_error(&error);
}
static void test_protected_entries(void) {
    const char *prefixes[] = {"@revoked ", "@cert-authority ", ""};
    for (guint i = 0; i < G_N_ELEMENTS(prefixes); i++) {
        reset(); char *text = g_strdup_printf("# header\n%s%s %s", prefixes[i], i == 2 ? "[192.168.1.1]:*" : "[192.168.1.1]:65422", old_key);
        put(path, text); GError *error = NULL; g_assert_null(prepare(&error)); g_assert_nonnull(error); g_clear_error(&error);
        char *after = get(path); g_assert_cmpstr(after, ==, text); g_free(after); g_free(text);
    }
    reset(); g_chmod(path, 0400); GError *error = NULL; g_assert_null(prepare(&error)); g_assert_nonnull(error); g_clear_error(&error); g_chmod(path, 0600);
    char *other = g_build_filename(root, "not-an-effective-user-file", NULL); put(other, old_key);
    char *diag = diagnostics(other, 1); g_assert_null(ssh_host_key_prepare(profile, diag, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error); g_free(diag); g_free(other);
    configuration("192.168.1.1", 65422, "proxyjump gateway.example\n"); g_assert_null(prepare(&error)); g_assert_nonnull(error); g_clear_error(&error);
}
static void test_rsa_candidate(void) {
    reset(); char *key = g_build_filename(root, "rsa", NULL);
    const char *gen[] = {"ssh-keygen", "-q", "-t", "rsa", "-b", "2048", "-N", "", "-f", key, NULL}; char *out = command(gen); g_free(out);
    char *pub = g_strconcat(key, ".pub", NULL); char *public = get(pub);
    char *scan = g_strdup_printf("[192.168.1.1]:65422 %s", public); put(scan_path, scan);
    const char *fp[] = {"ssh-keygen", "-lf", pub, "-E", "sha256", NULL}; out = command(fp); char **parts = g_strsplit(out, " ", -1);
    char *saved_fp = new_fp; new_fp = g_strdup(parts[1]); char *diag = diagnostics(path, 2); g_free(new_fp); new_fp = saved_fp;
    GError *error = NULL; SshHostKeyReview *r = ssh_host_key_prepare(profile, diag, NULL, &error); g_assert_no_error(error); g_assert_nonnull(r);
    g_assert_cmpstr(r->algorithm, ==, "ssh-rsa"); g_assert_cmpstr(r->fingerprint, ==, parts[1]);
    g_assert_true(ssh_host_key_replace(r, NULL, &error)); g_assert_no_error(error);
    ssh_host_key_unref(r); g_free(key); g_free(pub); g_free(public); g_free(scan); g_free(out); g_strfreev(parts); g_free(diag);
}
static void test_default_port_ipv6(void) {
    const char *hosts[] = {"192.168.1.1", "2001:db8::1"};
    const int ports[] = {22, 65422};
    for (guint i = 0; i < 2; i++) {
        reset(); configuration(hosts[i], ports[i], NULL);
        char *endpoint = ports[i] == 22 ? g_strdup(hosts[i]) : g_strdup_printf("[%s]:%d", hosts[i], ports[i]);
        char *text = g_strdup_printf("# header\n%s %s", endpoint, old_key); put(path, text); g_free(text);
        text = g_strdup_printf("%s %s", endpoint, new_key); put(scan_path, text); g_free(text);
        GError *error = NULL; SshHostKeyReview *r = prepare(&error); g_assert_no_error(error); g_assert_nonnull(r); g_assert_cmpstr(r->endpoint, ==, endpoint);
        g_assert_true(ssh_host_key_replace(r, NULL, &error)); g_assert_no_error(error); ssh_host_key_unref(r); g_free(endpoint);
    }
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    char *keygen = g_find_program_in_path("ssh-keygen"); if (!keygen) return 77; g_free(keygen);
    root = g_dir_make_tmp("devterm-hostkey-test-XXXXXX", NULL); path = g_build_filename(root, "known_hosts", NULL);
    config_path = g_build_filename(root, "config", NULL); scan_path = g_build_filename(root, "scan", NULL);
    const char *keys[] = {"old", "new"};
    for (guint i = 0; i < 2; i++) {
        char *keypath = g_build_filename(root, keys[i], NULL); const char *gen[] = {"ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", keypath, NULL};
        char *output = command(gen); g_free(output); char *pub = g_strconcat(keypath, ".pub", NULL);
        if (i == 0) old_key = get(pub); else {
            char *public = get(pub); char **key_parts = g_strsplit(public, " ", -1);
            new_key = g_strdup_printf("%s %s\n", key_parts[0], key_parts[1]); g_strfreev(key_parts); g_free(public);
            const char *fp[] = {"ssh-keygen", "-lf", pub, "-E", "sha256", NULL}; output = command(fp); char **parts = g_strsplit(output, " ", -1); new_fp = g_strdup(parts[1]); g_strfreev(parts); g_free(output);
        }
        g_free(pub); g_free(keypath);
    }
    char *bin = g_build_filename(root, "bin", NULL); g_mkdir(bin, 0700);
    char *script = g_build_filename(bin, "ssh", NULL), *quoted = g_shell_quote(config_path);
    char *text = g_strdup_printf("#!/bin/sh\n[ \"$1\" = -G ] || exit 95\nexec cat %s\n", quoted); put(script, text); g_chmod(script, 0700); g_free(script); g_free(quoted); g_free(text);
    script = g_build_filename(bin, "ssh-keyscan", NULL); quoted = g_shell_quote(scan_path); text = g_strdup_printf("#!/bin/sh\nexec cat %s\n", quoted); put(script, text); g_chmod(script, 0700); g_free(script); g_free(quoted); g_free(text);
    text = g_strconcat(bin, ":", g_getenv("PATH"), NULL); g_setenv("PATH", text, TRUE); g_free(text); g_free(bin);
    profile = session_profile_new(); profile->type = SESSION_SSH;
    g_test_add_func("/host-key/replace-backup-preserve", test_replace);
    g_test_add_func("/host-key/hash-aliases", test_hash_and_aliases);
    g_test_add_func("/host-key/cancel-conflict", test_cancel_and_conflict);
    g_test_add_func("/host-key/candidate-changed", test_candidate_changed);
    g_test_add_func("/host-key/protected-entries", test_protected_entries);
    g_test_add_func("/host-key/rsa-candidate", test_rsa_candidate);
    g_test_add_func("/host-key/default-port-ipv6", test_default_port_ipv6);
    int result = g_test_run(); session_profile_free(profile);
    const char *remove[] = {"rm", "-rf", "--", root, NULL}; text = command(remove); g_free(text);
    g_free(root); g_free(path); g_free(config_path); g_free(scan_path); g_free(old_key); g_free(new_key); g_free(new_fp); return result;
}
