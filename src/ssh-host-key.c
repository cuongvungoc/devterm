#define _POSIX_C_SOURCE 200809L
#include "ssh-host-key.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

SshHostKeyReview *ssh_host_key_ref(SshHostKeyReview *r) { g_atomic_int_inc(&r->refs); return r; }
void ssh_host_key_unref(SshHostKeyReview *r) {
    if (!r || !g_atomic_int_dec_and_test(&r->refs)) return;
    g_free(r->endpoint); g_free(r->path); g_free(r->old_fingerprints); g_free(r->fingerprint);
    g_free(r->algorithm); g_free(r->backup); g_free(r->original); g_free(r->replacement); g_free(r->etag); g_free(r);
}
static gboolean invalid(GError **error, const char *message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, message); return FALSE;
}
typedef struct { GMainLoop *loop; char *out, *err; GError *error; } Command;
static void communicated(GObject *object, GAsyncResult *result, gpointer data) {
    Command *c = data;
    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(object), result, &c->out, &c->err, &c->error);
    g_main_loop_quit(c->loop);
}
static void cancelled(GCancellable *cancel, gpointer data) { (void)cancel; g_subprocess_force_exit(data); }
static gboolean expired(gpointer data) { g_subprocess_force_exit(data); return G_SOURCE_REMOVE; }
static char *run(const char *const *argv, int timeout, GCancellable *cancel, gboolean allow_not_found, GError **error) {
    if (g_cancellable_set_error_if_cancelled(cancel, error)) return NULL;
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
    GSubprocess *process = g_subprocess_launcher_spawnv(launcher, argv, error); g_object_unref(launcher);
    if (!process) return NULL;
    GMainContext *context = g_main_context_new(); g_main_context_push_thread_default(context);
    Command c = {.loop = g_main_loop_new(context, FALSE)};
    GSource *timer = g_timeout_source_new_seconds(timeout);
    g_source_set_callback(timer, expired, process, NULL); g_source_attach(timer, context);
    gulong handler = cancel ? g_cancellable_connect(cancel, G_CALLBACK(cancelled), process, NULL) : 0;
    g_subprocess_communicate_utf8_async(process, NULL, NULL, communicated, &c); g_main_loop_run(c.loop);
    if (handler) g_cancellable_disconnect(cancel, handler);
    g_source_destroy(timer); g_source_unref(timer); g_main_loop_unref(c.loop);
    g_main_context_pop_thread_default(context); g_main_context_unref(context);
    if (g_cancellable_set_error_if_cancelled(cancel, error)) { g_clear_error(&c.error); g_clear_pointer(&c.out, g_free); }
    else if (c.error) { g_propagate_error(error, c.error); g_clear_pointer(&c.out, g_free); }
    else if (!g_subprocess_get_successful(process) && !(allow_not_found && g_subprocess_get_if_exited(process) && g_subprocess_get_exit_status(process) == 1)) {
        invalid(error, "Cannot inspect SSH host keys (command failed or timed out). Check the host, port and SSH configuration.");
        g_clear_pointer(&c.out, g_free);
    }
    if (c.out && strlen(c.out) > 8 * 1024 * 1024) { invalid(error, "SSH inspection returned too much data"); g_clear_pointer(&c.out, g_free); }
    g_free(c.err); g_object_unref(process); return c.out;
}
static char *capture(const char *text, const char *pattern, guint group) {
    GRegex *regex = g_regex_new(pattern, G_REGEX_MULTILINE, 0, NULL); GMatchInfo *match = NULL;
    char *value = g_regex_match(regex, text, 0, &match) ? g_match_info_fetch(match, group) : NULL;
    g_match_info_free(match); g_regex_unref(regex); return value;
}
static char *config_value(const char *config, const char *name) {
    char *pattern = g_strdup_printf("^%s ([^\\r\\n]+)$", name); char *value = capture(config, pattern, 1); g_free(pattern); return value;
}
static char *fingerprint(const char *algorithm, const char *base64) {
    gsize size; guchar *blob = g_base64_decode(base64, &size);
    if (size < 4 || size > 16384) { g_free(blob); return NULL; }
    guint32 length; memcpy(&length, blob, 4); length = GUINT32_FROM_BE(length);
    if (length != strlen(algorithm) || length + 4 > size || memcmp(blob + 4, algorithm, length)) { g_free(blob); return NULL; }
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256); g_checksum_update(sum, blob, size); g_free(blob);
    guchar hash[32]; gsize hash_size = sizeof hash; g_checksum_get_digest(sum, hash, &hash_size); g_checksum_free(sum);
    char *encoded = g_base64_encode(hash, hash_size); char *padding = strchr(encoded, '='); if (padding) *padding = 0;
    char *result = g_strconcat("SHA256:", encoded, NULL); g_free(encoded); return result;
}
static char **fields(const char *line) {
    GPtrArray *result = g_ptr_array_new(); char **parts = g_strsplit_set(line, " \t\r", -1);
    for (guint i = 0; parts[i]; i++) if (*parts[i]) g_ptr_array_add(result, g_strdup(parts[i]));
    g_strfreev(parts); g_ptr_array_add(result, NULL); return (char **)g_ptr_array_free(result, FALSE);
}
static gboolean regular_owned(const char *path, GError **error) {
    struct stat st;
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || !(st.st_mode & S_IWUSR) || st.st_size > 8 * 1024 * 1024)
        return invalid(error, "Automatic replacement requires a regular, writable known_hosts file owned by you (up to 8 MiB). Symlinks and system files require manual review.");
    return TRUE;
}
static gboolean allowed_file(const char *config, const char *path) {
    char *value = config_value(config, "userknownhostsfile"); int argc = 0; char **argv = NULL; gboolean allowed = FALSE;
    if (value && g_shell_parse_argv(value, &argc, &argv, NULL)) {
        for (int i = 0; i < argc; i++) {
            char *expanded = g_str_has_prefix(argv[i], "~/") ? g_build_filename(g_get_home_dir(), argv[i] + 2, NULL) : g_strdup(argv[i]);
            char *canonical = g_canonicalize_filename(expanded, NULL);
            if (g_str_equal(canonical, path)) allowed = TRUE;
            g_free(canonical); g_free(expanded);
        }
    }
    g_free(value); g_strfreev(argv); return allowed;
}
SshHostKeyReview *ssh_host_key_prepare(const SessionProfile *p, const char *diagnostics, GCancellable *cancel, GError **error) {
    SshHostKeyReview *r = g_new0(SshHostKeyReview, 1); r->refs = 1;
    char *config = NULL, *host = NULL, *port = NULL, *scan = NULL, *found = NULL, *temporary = NULL, *directory = NULL;
    char **lines = NULL; GHashTable *matched = NULL; GFile *file = NULL; GString *replacement = NULL, *old = NULL;
    char *line_number = capture(diagnostics, "^Offending [^\\r\\n]+ key in (.+):([0-9]+)\\r?$", 2);
    r->path = capture(diagnostics, "^Offending [^\\r\\n]+ key in (.+):([0-9]+)\\r?$", 1);
    char *hash = capture(diagnostics, "^SHA256:([A-Za-z0-9+/]+)\\.?\\r?$", 1);
    r->fingerprint = hash ? g_strconcat("SHA256:", hash, NULL) : NULL; g_free(hash);
    if (!p || p->type != SESSION_SSH || !r->path || !line_number || !r->fingerprint || !g_path_is_absolute(r->path)) {
        invalid(error, "Cannot safely identify the changed host key from OpenSSH diagnostics. Reconnect to obtain a fresh warning."); goto fail;
    }
    /* Only the effective user file may be changed; never trust a path printed
     * by an arbitrary remote program or edit a global/CA/revocation entry. */
    char *profile_port = g_strdup_printf("%d", p->port);
    const char *args[] = {"ssh", "-G", "-p", profile_port, "-l", p->username, "--", p->host, NULL};
    config = run(args, p->timeout, cancel, FALSE, error); g_free(profile_port); if (!config) goto fail;
    const char *unsupported[] = {"proxycommand", "proxyjump", "hostkeyalias", NULL};
    for (int i = 0; unsupported[i]; i++) {
        char *value = config_value(config, unsupported[i]); gboolean bad = value && !g_str_equal(value, "none"); g_free(value);
        if (bad) { invalid(error, "Automatic review currently supports direct SSH connections without ProxyCommand, ProxyJump or HostKeyAlias. Review this SSH configuration manually."); goto fail; }
    }
    if (!allowed_file(config, r->path) || !regular_owned(r->path, error)) {
        if (!error || !*error) invalid(error, "The offending entry is not in an effective user known_hosts file. Review system/SSH settings manually.");
        goto fail;
    }
    host = config_value(config, "hostname"); port = config_value(config, "port");
    if (!host || !port || host[0] == '-') { invalid(error, "Cannot resolve SSH host and port"); goto fail; }
    char *lower_host = g_ascii_strdown(host, -1);
    r->endpoint = g_str_equal(port, "22") ? g_strdup(lower_host) : g_strdup_printf("[%s]:%s", lower_host, port); g_free(lower_host);
    file = g_file_new_for_path(r->path); gsize size;
    if (!g_file_load_contents(file, cancel, &r->original, &size, &r->etag, error)) goto fail;
    if (size > 8 * 1024 * 1024 || memchr(r->original, 0, size)) { invalid(error, "Unsupported known_hosts contents"); goto fail; }
    directory = g_dir_make_tmp("devterm-hostkey-XXXXXX", error); if (!directory) goto fail;
    temporary = g_build_filename(directory, "known_hosts", NULL);
    if (!g_file_set_contents_full(temporary, r->original, size, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error)) goto fail;
    const char *find[] = {"ssh-keygen", "-F", r->endpoint, "-f", temporary, NULL};
    found = run(find, p->timeout, cancel, TRUE, error); if (!found) goto fail;
    matched = g_hash_table_new(g_direct_hash, g_direct_equal);
    char **matches = g_strsplit(found, "\n", -1);
    for (guint i = 0; matches[i]; i++) {
        char *number = capture(matches[i], "^# Host .+ found: line ([0-9]+)[ \t]*$", 1);
        if (number) g_hash_table_add(matched, GUINT_TO_POINTER((guint)g_ascii_strtoull(number, NULL, 10)));
        g_free(number);
    }
    g_strfreev(matches);
    if (!g_hash_table_contains(matched, GUINT_TO_POINTER((guint)g_ascii_strtoull(line_number, NULL, 10)))) {
        invalid(error, "The offending entry no longer matches this host/port. Reconnect and review the latest warning."); goto fail;
    }
    const char *scan_args[] = {"ssh-keyscan", "-T", "5", "-p", port, "--", host, NULL};
    scan = run(scan_args, p->timeout, cancel, FALSE, error); if (!scan) goto fail;
    char **candidates = g_strsplit(scan, "\n", -1); char *key = NULL;
    for (guint i = 0; candidates[i]; i++) {
        char **parts = fields(candidates[i]);
        if (g_strv_length(parts) >= 3 && parts[0][0] != '#') {
            char *fp = fingerprint(parts[1], parts[2]);
            if (g_strcmp0(fp, r->fingerprint) == 0 && !key) { r->algorithm = g_strdup(parts[1]); key = g_strdup_printf("%s %s", parts[1], parts[2]); }
            g_free(fp);
        }
        g_strfreev(parts);
    }
    g_strfreev(candidates);
    if (!key) { invalid(error, "The server no longer presents the fingerprint in the warning. Nothing was changed; reconnect and verify the new fingerprint."); goto fail; }
    lines = g_strsplit(r->original, "\n", -1); replacement = g_string_new(NULL); old = g_string_new(NULL);
    gboolean valid = TRUE, already_trusted = FALSE;
    for (guint i = 0; lines[i] && valid; i++) {
        if (!g_hash_table_contains(matched, GUINT_TO_POINTER(i + 1))) g_string_append(replacement, lines[i]);
        else {
            char **parts = fields(lines[i]); guint n = g_strv_length(parts);
            if (n < 3 || parts[0][0] == '@') valid = FALSE;
            char *fp = valid ? fingerprint(parts[1], parts[2]) : NULL;
            if (!fp) valid = FALSE;
            if (fp && g_str_equal(fp, r->fingerprint)) already_trusted = TRUE;
            if (valid) g_string_append_printf(old, "%s%s  %s", old->len ? "\n" : "", parts[1], fp);
            /* Keep other aliases sharing the same public key. Broad patterns
             * and negated/compound hashed patterns require manual editing. */
            if (valid && parts[0][0] != '|') {
                char **aliases = g_strsplit(parts[0], ",", -1); GString *keep = g_string_new(NULL); gboolean removed = FALSE;
                for (guint a = 0; aliases[a]; a++) {
                    if (strpbrk(aliases[a], "*?!|")) { valid = FALSE; break; }
                    if (g_ascii_strcasecmp(aliases[a], r->endpoint) == 0) removed = TRUE;
                    else g_string_append_printf(keep, "%s%s", keep->len ? "," : "", aliases[a]);
                }
                if (!removed) valid = FALSE;
                if (valid && keep->len) {
                    const char *suffix = lines[i] + strspn(lines[i], " \t") + strlen(parts[0]); g_string_append(replacement, keep->str); g_string_append(replacement, suffix);
                }
                g_string_free(keep, TRUE); g_strfreev(aliases);
            } else if (valid && strchr(parts[0], ',')) valid = FALSE;
            g_free(fp); g_strfreev(parts);
        }
        if (lines[i + 1]) g_string_append_c(replacement, '\n');
    }
    if (valid && already_trusted) { g_free(key); invalid(error, "This fingerprint is already saved for this endpoint. Nothing was changed; reconnect to obtain the current SSH status."); goto fail; }
    if (!valid) { g_free(key); invalid(error, "A matching known_hosts entry uses a certificate, revocation marker, wildcard or unsupported alias pattern. Edit it manually; nothing was changed."); goto fail; }
    if (replacement->len && replacement->str[replacement->len - 1] != '\n') g_string_append_c(replacement, '\n');
    g_string_append_printf(replacement, "%s %s\n", r->endpoint, key); g_free(key);
    r->replacement = g_string_free(replacement, FALSE); replacement = NULL;
    r->old_fingerprints = g_string_free(old, FALSE); old = NULL;
    char *uuid = g_uuid_string_random(); r->backup = g_strdup_printf("%s.devterm-%s.bak", r->path, uuid); g_free(uuid);
fail:
    g_free(line_number); g_free(config); g_free(host); g_free(port); g_free(scan); g_free(found);
    if (temporary) { g_unlink(temporary); g_free(temporary); } if (directory) { g_rmdir(directory); g_free(directory); }
    g_strfreev(lines); g_clear_pointer(&matched, g_hash_table_unref); g_clear_object(&file);
    if (replacement) g_string_free(replacement, TRUE);
    if (old) g_string_free(old, TRUE);
    if (!r->replacement) { ssh_host_key_unref(r); return NULL; }
    return r;
}
gboolean ssh_host_key_replace(SshHostKeyReview *r, GCancellable *cancel, GError **error) {
    if (g_cancellable_set_error_if_cancelled(cancel, error) || !regular_owned(r->path, error)) return FALSE;
    GFile *file = g_file_new_for_path(r->path); char *current = NULL, *etag = NULL; gsize size;
    gboolean ok = g_file_load_contents(file, cancel, &current, &size, &etag, error);
    if (ok && (size != strlen(r->original) || memcmp(current, r->original, size) || g_strcmp0(etag, r->etag)))
        ok = invalid(error, "known_hosts changed while this review was open. Nothing was replaced; reconnect and review again.");
    g_free(current); g_free(etag);
    if (ok) {
        int fd = open(r->backup, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot create backup: %s", g_strerror(errno)); ok = FALSE;
        } else {
            gsize offset = 0, length = strlen(r->original);
            while (ok && offset < length) {
                if (g_cancellable_set_error_if_cancelled(cancel, error)) { ok = FALSE; break; }
                ssize_t written = write(fd, r->original + offset, length - offset);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) { g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot write backup: %s", g_strerror(errno)); ok = FALSE; }
                else offset += written;
            }
            if (ok && fsync(fd) != 0) { g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot sync backup: %s", g_strerror(errno)); ok = FALSE; }
            if (close(fd) != 0 && ok) { g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot close backup: %s", g_strerror(errno)); ok = FALSE; }
            if (!ok) g_unlink(r->backup);
        }
    }
    if (ok) ok = regular_owned(r->path, error) && g_file_replace_contents(file, r->replacement, strlen(r->replacement), r->etag, FALSE,
                                         G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION, NULL, cancel, error);
    g_object_unref(file); return ok;
}
typedef struct { SessionProfile *profile; char *diagnostics; } Prepare;
static void prepare_free(gpointer data) { Prepare *p = data; session_profile_free(p->profile); g_free(p->diagnostics); g_free(p); }
static void prepare_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source; Prepare *p = data; GError *error = NULL;
    SshHostKeyReview *r = ssh_host_key_prepare(p->profile, p->diagnostics, cancel, &error);
    if (r) g_task_return_pointer(task, r, (GDestroyNotify)ssh_host_key_unref); else g_task_return_error(task, error);
}
void ssh_host_key_prepare_async(const SessionProfile *profile, const char *diagnostics, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
    GTask *task = g_task_new(NULL, cancel, callback, data); Prepare *p = g_new0(Prepare, 1);
    p->profile = session_profile_copy(profile); p->diagnostics = g_strdup(diagnostics);
    g_task_set_task_data(task, p, prepare_free); g_task_run_in_thread(task, prepare_worker); g_object_unref(task);
}
SshHostKeyReview *ssh_host_key_prepare_finish(GAsyncResult *result, GError **error) { return g_task_propagate_pointer(G_TASK(result), error); }
static void replace_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source; GError *error = NULL;
    if (ssh_host_key_replace(data, cancel, &error)) g_task_return_boolean(task, TRUE); else g_task_return_error(task, error);
}
void ssh_host_key_replace_async(SshHostKeyReview *r, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
    GTask *task = g_task_new(NULL, cancel, callback, data); g_task_set_check_cancellable(task, FALSE);
    g_task_set_task_data(task, ssh_host_key_ref(r), (GDestroyNotify)ssh_host_key_unref);
    g_task_run_in_thread(task, replace_worker); g_object_unref(task);
}
gboolean ssh_host_key_replace_finish(GAsyncResult *result, GError **error) { return g_task_propagate_boolean(G_TASK(result), error); }
