#define _GNU_SOURCE
#include "sessions.h"
#include "login.h"
#include <gtk/gtk.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <glib/gstdio.h>

static volatile sig_atomic_t stopping, resizing;
static void handle_signal(int sig) { if (sig == SIGWINCH) resizing = 1; else stopping = 1; }
static gboolean write_all(int fd, const char *s, gsize size) {
    while (size) { ssize_t n = write(fd, s, size); if (n < 0 && errno == EINTR) continue; if (n <= 0) return FALSE; s += n; size -= n; }
    return TRUE;
}
static void message(const char *s) { write_all(1, "\r\n[DevTerm] ", 12); write_all(1, s, strlen(s)); write_all(1, "\r\n", 2); }
typedef struct { GMainLoop *loop; gboolean accepted; } Trust;
static void trust_response(GtkDialog *dialog, int response, gpointer data) {
    Trust *trust = data; trust->accepted = response == GTK_RESPONSE_ACCEPT;
    gtk_window_destroy(GTK_WINDOW(dialog)); g_main_loop_quit(trust->loop);
}
static int askpass(const char *prompt) {
    /* OpenSSH 9.6 host verification uses RP_ECHO, not the askpass "confirm"
     * hint. Only its explicit host-key question may receive a yes response. */
    if (strstr(prompt, "fingerprint") && strstr(prompt, "Are you sure")) {
        if (!gtk_init_check()) return 1;
        Trust trust = {g_main_loop_new(NULL, FALSE), FALSE};
        GtkWidget *dialog = gtk_dialog_new_with_buttons("DevTerm — Verify SSH Host Key", NULL, GTK_DIALOG_MODAL,
            "Cancel", GTK_RESPONSE_CANCEL, "Trust Host Key", GTK_RESPONSE_ACCEPT, NULL);
        GtkWidget *label = gtk_label_new(prompt); gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_selectable(GTK_LABEL(label), TRUE); gtk_widget_set_size_request(label, 540, -1);
        gtk_widget_set_margin_start(label, 16); gtk_widget_set_margin_end(label, 16);
        gtk_widget_set_margin_top(label, 16); gtk_widget_set_margin_bottom(label, 16);
        gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), label);
        gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
        g_signal_connect(dialog, "response", G_CALLBACK(trust_response), &trust);
        gtk_window_present(GTK_WINDOW(dialog)); g_main_loop_run(trust.loop); g_main_loop_unref(trust.loop);
        if (trust.accepted) { write_all(1, "yes\n", 4); return 0; }
        return 1;
    }
    char *lower = g_ascii_strdown(prompt, -1);
    gboolean password_prompt = g_str_has_suffix(lower, "password: ") || g_str_has_suffix(lower, "password:");
    g_free(lower);
    const char *path = g_getenv("DEVTERM_AUTH_SOCKET");
    if (!password_prompt || !path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return 1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); if (fd < 0) return 1;
    struct sockaddr_un address = {.sun_family = AF_UNIX}; g_strlcpy(address.sun_path, path, sizeof address.sun_path);
    struct timeval timeout = {5, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) { close(fd); return 1; }
    char secret[1024] = {0}; gsize size = 0;
    while (size < sizeof secret - 1) {
        ssize_t n = read(fd, secret + size, sizeof secret - 1 - size);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { explicit_bzero(secret, sizeof secret); close(fd); return 1; }
        if (!n) break;
        size += n;
    }
    close(fd);
    gboolean ok = size && write_all(1, secret, size) && write_all(1, "\n", 1);
    explicit_bzero(secret, sizeof secret); return ok ? 0 : 1;
}
static pid_t parent_pid(pid_t pid) {
    char *path = g_strdup_printf("/proc/%d/stat", pid), *text = NULL; pid_t parent = -1; char state;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char *end = strrchr(text, ')'); if (end) sscanf(end + 1, " %c %d", &state, &parent);
    }
    g_free(path); g_free(text); return parent;
}
static void serve_password(int listener, pid_t ssh_pid, char **secret) {
    int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK); if (fd < 0) return;
    struct ucred peer; socklen_t size = sizeof peer;
    if (*secret && getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 &&
        peer.uid == getuid() && parent_pid(peer.pid) == ssh_pid) {
        write_all(fd, *secret, strlen(*secret)); session_secret_free(*secret); *secret = NULL;
    }
    close(fd);
}
static int auth_listener(char **directory, char **path) {
    *directory = g_build_filename(g_get_user_runtime_dir(), "devterm-auth-XXXXXX", NULL);
    if (!g_mkdtemp(*directory)) return -1;
    *path = g_build_filename(*directory, "password.sock", NULL);
    if (strlen(*path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); if (fd < 0) return -1;
    struct sockaddr_un addr = {.sun_family = AF_UNIX}; g_strlcpy(addr.sun_path, *path, sizeof addr.sun_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 4) != 0) { close(fd); return -1; }
    return fd;
}
static void arg(GPtrArray *args, const char *text) { g_ptr_array_add(args, g_strdup(text)); }
static GPtrArray *client_args(SessionProfile *p, GError **error) {
    const char *name = p->type == SESSION_SSH ? "ssh" : p->type == SESSION_TELNET ? "telnet" : "picocom";
    char *program = g_find_program_in_path(name);
    if (!program) { g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Install %s to use this session type", name); return NULL; }
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free); g_ptr_array_add(a, program);
    char *port = g_strdup_printf("%d", p->port);
    if (p->type == SESSION_SSH) {
        arg(a, "-tt"); arg(a, "-p"); arg(a, port); arg(a, "-l"); arg(a, p->username);
        const char *options[] = {"StrictHostKeyChecking=ask", "NumberOfPasswordPrompts=1", "ConnectionAttempts=1",
            "ControlPath=none", "ForwardAgent=no", "ServerAliveInterval=30", "ServerAliveCountMax=3", NULL};
        for (int i = 0; options[i]; i++) { arg(a, "-o"); arg(a, options[i]); }
        char *timeout = g_strdup_printf("ConnectTimeout=%d", p->timeout); arg(a, "-o"); arg(a, timeout); g_free(timeout);
        if (p->password_auth) {
            arg(a, "-o"); arg(a, "PreferredAuthentications=password"); arg(a, "-o"); arg(a, "KbdInteractiveAuthentication=no");
        } else { arg(a, "-o"); arg(a, "PreferredAuthentications=publickey"); }
        if (*p->identity) { arg(a, "-i"); arg(a, p->identity); }
        arg(a, "--"); arg(a, p->host);
    } else if (p->type == SESSION_TELNET) {
        /* Preserve high-bit UTF-8 bytes and prevent password/input bytes from
         * becoming telnet's local escape commands. Disconnect is owned by UI. */
        arg(a, "-8"); arg(a, "-E"); arg(a, "-c"); arg(a, p->host); arg(a, port);
    }
    else {
        char *baud = g_strdup_printf("%d", p->baud), *bits = g_strdup_printf("%d", p->data_bits), *stop = g_strdup_printf("%d", p->stop_bits);
        arg(a, "--quiet"); arg(a, "--no-escape"); arg(a, "--baud"); arg(a, baud);
        arg(a, "--databits"); arg(a, bits); arg(a, "--stopbits"); arg(a, stop);
        arg(a, "--parity"); arg(a, p->parity); arg(a, "--flow"); arg(a, p->flow);
        /* picocom sends this only after opening/configuring the device;
         * queuing stdin before startup loses the byte during its tty flush. */
        if (p->send_initial_enter) { arg(a, "--initstring"); arg(a, "\r"); }
        arg(a, p->device);
        g_free(baud); g_free(bits); g_free(stop);
    }
    g_free(port); g_ptr_array_add(a, NULL); return a;
}
int main(int argc, char **argv) {
    struct rlimit limit = {0, 0}; setrlimit(RLIMIT_CORE, &limit); prctl(PR_SET_DUMPABLE, 0);
    pid_t parent = getppid(); prctl(PR_SET_PDEATHSIG, SIGHUP); if (getppid() != parent) return 1;
    if (argc == 2 && strcmp(argv[1], "--session")) return askpass(argv[1]);
    if (argc != 2) return 1;
    GError *error = NULL; char *secret = NULL;
    SessionProfile *p = session_read_launch(3, &secret, &error); close(3);
    if (!p) { message(error->message); g_error_free(error); return 1; }
    GPtrArray *args = client_args(p, &error);
    if (!args) { message(error->message); g_error_free(error); session_secret_free(secret); session_profile_free(p); return 1; }
    char *auth_dir = NULL, *auth_path = NULL;
    int listener = -1;
    if (p->type == SESSION_SSH && p->password_auth) {
        listener = auth_listener(&auth_dir, &auth_path);
        if (listener < 0) { message("Cannot create private SSH authentication channel"); goto cleanup; }
        char *self = g_file_read_link("/proc/self/exe", NULL);
        g_setenv("SSH_ASKPASS", self, TRUE); g_setenv("SSH_ASKPASS_REQUIRE", "force", TRUE);
        g_setenv("DEVTERM_AUTH_SOCKET", auth_path, TRUE); g_unsetenv("SSH_ASKPASS_PROMPT"); g_free(self);
    }
    struct termios original, raw; gboolean have_termios = tcgetattr(0, &original) == 0;
    if (have_termios) { raw = original; cfmakeraw(&raw); tcsetattr(0, TCSANOW, &raw); }
    struct winsize dimensions = {0}; ioctl(0, TIOCGWINSZ, &dimensions);
    int master; pid_t child = forkpty(&master, NULL, NULL, &dimensions);
    if (child == 0) {
        pid_t helper = getppid(); prctl(PR_SET_PDEATHSIG, SIGHUP); if (getppid() != helper) _exit(1);
        execv(args->pdata[0], (char **)args->pdata); _exit(127);
    }
    if (child < 0) { message("Cannot create session PTY"); if (have_termios) tcsetattr(0, TCSANOW, &original); goto cleanup; }
    struct sigaction sa = {.sa_handler = handle_signal}; sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGWINCH, &sa, NULL);
    signal(SIGPIPE, SIG_IGN); fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    gboolean authenticating = p->type != SESSION_SSH && p->auto_login;
    LoginMachine *login = authenticating ? login_machine_new(p) : NULL;
    GString *diagnostics = g_string_new(NULL);
    gboolean password_submitted = FALSE;
    GByteArray *pending = g_byte_array_new();
    if (authenticating) message("Automatic login in progress; typed input and Quick Commands are ignored until ready. Esc/Ctrl+C cancels. Authentication output is hidden.");
    if (p->type == SESSION_TELNET && p->send_initial_enter) g_byte_array_append(pending, (guint8 *)"\r", 1);
    gint64 deadline = g_get_monotonic_time() + p->timeout * G_TIME_SPAN_SECOND;
    int status = 0; gboolean ended = FALSE, failed = FALSE;
    while (!stopping) {
        if (!ended) ended = waitpid(child, &status, WNOHANG) == child;
        if (resizing) { ioctl(0, TIOCGWINSZ, &dimensions); ioctl(master, TIOCSWINSZ, &dimensions); resizing = 0; }
        if (authenticating && g_get_monotonic_time() > deadline) { message("Automatic login timed out. Connection stopped; check profile prompts and credentials."); failed = TRUE; break; }
        struct pollfd fds[] = {{master, POLLIN | (pending->len ? POLLOUT : 0), 0}, {0, pending->len < 65536 ? POLLIN : 0, 0}, {listener, POLLIN, 0}};
        int n = poll(fds, 3, 100); if (n < 0 && errno == EINTR) continue; if (n < 0) break;
        if (fds[2].revents & POLLIN) serve_password(listener, child, &secret);
        if (fds[0].revents & POLLOUT) {
            ssize_t sent = write(master, pending->data, pending->len);
            if (sent > 0) {
                GByteArray *remaining = g_byte_array_new();
                if (pending->len > (gsize)sent) g_byte_array_append(remaining, pending->data + sent, pending->len - sent);
                explicit_bzero(pending->data, pending->len); g_byte_array_unref(pending); pending = remaining;
            }
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            char input[4096]; ssize_t count = read(0, input, sizeof input);
            if (count <= 0) break;
            if (!authenticating) g_byte_array_append(pending, (guint8 *)input, count);
            else if (memchr(input, 3, count) || memchr(input, 27, count)) { message("Automatic login cancelled"); failed = TRUE; break; }
            explicit_bzero(input, sizeof input);
        }
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char output[8192]; ssize_t count = read(master, output, sizeof output);
            if (count <= 0) { if (count == 0 || errno == EIO || ended) break; }
            else if (!authenticating) { if (!write_all(1, output, count)) break; }
            else {
                if (!password_submitted && diagnostics->len < 16384)
                    g_string_append_len(diagnostics, output, MIN((gsize)count, 16384 - diagnostics->len));
                LoginAction action = login_machine_feed(login, output, count);
                if (action == LOGIN_SEND_USER || action == LOGIN_SEND_PASSWORD) {
                    const char *value = action == LOGIN_SEND_USER ? p->username : secret;
                    if (!value) { failed = TRUE; break; }
                    g_byte_array_append(pending, (guint8 *)value, strlen(value)); g_byte_array_append(pending, (guint8 *)"\r", 1);
                    if (action == LOGIN_SEND_PASSWORD) {
                        password_submitted = TRUE; explicit_bzero(diagnostics->str, diagnostics->allocated_len); g_string_truncate(diagnostics, 0);
                    }
                } else if (action == LOGIN_READY) {
                    authenticating = FALSE; login_machine_free(login); login = NULL;
                    session_secret_free(secret); secret = NULL;
                    message("Automatic login complete. Press Enter to display the device prompt.");
                } else if (action == LOGIN_FAILED) { message("Login was rejected. Connection stopped; update the profile or password before retrying."); failed = TRUE; explicit_bzero(output, sizeof output); break; }
            }
            explicit_bzero(output, sizeof output);
        } else if (ended) break;
    }
    if (authenticating && !failed) {
        message("Connection ended before automatic login completed"); failed = TRUE;
    }
    if (authenticating && !password_submitted && diagnostics->len) {
        write_all(1, diagnostics->str, diagnostics->len);
        message("Connection stopped before password submission. Check the client output above and the profile prompts.");
    }
    kill(-child, SIGTERM); close(master);
    if (!ended) {
        gint64 end = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
        while (waitpid(child, &status, WNOHANG) == 0 && g_get_monotonic_time() < end) g_usleep(10000);
        kill(-child, SIGKILL); while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    if (have_termios) tcsetattr(0, TCSANOW, &original);
    login_machine_free(login); if (pending->len) explicit_bzero(pending->data, pending->len); g_byte_array_unref(pending);
    explicit_bzero(diagnostics->str, diagnostics->allocated_len); g_string_free(diagnostics, TRUE);
    if (!failed && !stopping && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        session_secret_free(secret); session_profile_free(p); g_ptr_array_unref(args);
        if (listener >= 0) close(listener);
        if (auth_path) g_unlink(auth_path);
        if (auth_dir) g_rmdir(auth_dir);
        g_free(auth_path); g_free(auth_dir); return 0;
    }
cleanup:
    session_secret_free(secret); session_profile_free(p); g_ptr_array_unref(args);
    if (listener >= 0) close(listener);
    if (auth_path) g_unlink(auth_path);
    if (auth_dir) g_rmdir(auth_dir);
    g_free(auth_path); g_free(auth_dir); return 1;
}
