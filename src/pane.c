#define _GNU_SOURCE
#include "pane.h"
#include "ssh-host-key.h"
#include <glib-unix.h>
#include <sys/socket.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum { RECONNECT_NONE, RECONNECT_PASSWORD, RECONNECT_STOPPING, RECONNECT_STARTING } ReconnectState;

struct TerminalPane {
    int refs;
    GtkWidget *root;
    VteTerminal *terminal;
    GtkWidget *label;
    GtkWidget *state;
    GtkWidget *search_box, *search_entry, *match_case, *whole_words, *search_message;
    GtkWidget *search_previous, *search_next;
    guint search_timeout;
    gboolean search_dirty;
    GPid child;
    gboolean closed;
    gboolean running;
    gboolean starting;
    gboolean exited;
    char *working_directory;
    SessionProfile *profile;
    GCancellable *spawn_cancel;
    gboolean disconnecting;
    ReconnectState reconnect;
    GCancellable *reconnect_cancel;
    int reconnect_fd;
    guint reconnect_idle, stop_timeout;
    GtkWidget *host_review_button, *host_dialog, *host_detail, *host_verified;
    GCancellable *host_cancel;
    SshHostKeyReview *host_review;
    char *host_diagnostics;
    int host_socket;
    guint host_watch;
    PaneEvent reconnect_requested;
    PaneEvent selected;
    PaneEvent status;
    gpointer user_data;
};

TerminalPane *pane_ref(TerminalPane *p) { p->refs++; return p; }
void pane_unref(TerminalPane *p) {
    if (--p->refs) return;
    g_object_unref(p->root);
    g_free(p->working_directory);
    session_profile_free(p->profile);
    g_free(p->host_diagnostics);
    g_clear_pointer(&p->host_review, ssh_host_key_unref);
    g_clear_object(&p->host_cancel);
    g_clear_object(&p->spawn_cancel);
    g_clear_object(&p->reconnect_cancel);
    g_free(p);
}

static void pane_message(TerminalPane *p, const char *message) {
    if (p->closed) return;
    gtk_label_set_text(GTK_LABEL(p->label), message);
    if (p->status) p->status(p, message, p->user_data);
}
static void host_review_cancel(TerminalPane *p) {
    if (p->host_cancel) g_cancellable_cancel(p->host_cancel);
    g_clear_object(&p->host_cancel);
    g_clear_pointer(&p->host_review, ssh_host_key_unref);
    GtkWidget *dialog = g_steal_pointer(&p->host_dialog);
    if (dialog) gtk_window_destroy(GTK_WINDOW(dialog));
}
static void host_notice_close(TerminalPane *p) {
    if (p->host_watch) { g_source_remove(p->host_watch); p->host_watch = 0; }
    if (p->host_socket >= 0) { close(p->host_socket); p->host_socket = -1; }
}
static void host_notice_clear(TerminalPane *p) {
    host_review_cancel(p); host_notice_close(p);
    g_clear_pointer(&p->host_diagnostics, g_free); gtk_widget_set_visible(p->host_review_button, FALSE);
}
static gboolean host_notice(int fd, GIOCondition condition, gpointer data) {
    (void)condition; TerminalPane *p = data; char packet[SSH_HOST_KEY_NOTICE_LIMIT + 128];
    ssize_t size = recv(fd, packet, sizeof packet - 1, MSG_DONTWAIT);
    if (!p->closed && size > (ssize_t)strlen(SSH_HOST_KEY_NOTICE) && size < (ssize_t)sizeof packet - 1 &&
        !memcmp(packet, SSH_HOST_KEY_NOTICE, strlen(SSH_HOST_KEY_NOTICE)) && !memchr(packet, 0, size) && g_utf8_validate(packet, size, NULL)) {
        packet[size] = 0; g_free(p->host_diagnostics); p->host_diagnostics = g_strdup(packet + strlen(SSH_HOST_KEY_NOTICE));
        gtk_widget_set_visible(p->host_review_button, TRUE);
        pane_message(p, "SSH host key has changed. Review the fingerprint before reconnecting.");
    }
    close(p->host_socket); p->host_socket = -1; p->host_watch = 0; return G_SOURCE_REMOVE;
}
static void host_dialog_destroyed(GtkWidget *dialog, gpointer data) {
    TerminalPane *p = data;
    if (p->host_dialog == dialog) {
        p->host_dialog = NULL; host_review_cancel(p);
        if (!p->closed && gtk_widget_get_mapped(p->root)) pane_focus(p);
    }
}
static void host_replaced(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source; TerminalPane *p = data; GError *error = NULL;
    gboolean ok = ssh_host_key_replace_finish(result, &error);
    gboolean current = !p->closed && g_task_get_cancellable(G_TASK(result)) == p->host_cancel;
    if (current) {
        char *message = ok ? g_strdup_printf("SSH host key replaced. Backup: %s", p->host_review->backup) : g_strdup(error->message);
        host_review_cancel(p); pane_message(p, message); g_free(message);
        if (ok) {
            g_clear_pointer(&p->host_diagnostics, g_free); gtk_widget_set_visible(p->host_review_button, FALSE);
            if (p->reconnect_requested) p->reconnect_requested(p, NULL, p->user_data);
        }
    }
    g_clear_error(&error); pane_unref(p);
}
static void host_verified(GtkCheckButton *button, gpointer data) {
    TerminalPane *p = data;
    gtk_dialog_set_response_sensitive(GTK_DIALOG(p->host_dialog), GTK_RESPONSE_ACCEPT,
        p->host_review && gtk_check_button_get_active(button));
}
static void host_response(GtkDialog *dialog, int response, gpointer data) {
    TerminalPane *p = data;
    if (response == 1 && p->host_review) { gdk_clipboard_set_text(gtk_widget_get_clipboard(p->root), p->host_review->fingerprint); return; }
    if (response == GTK_RESPONSE_ACCEPT) {
        if (!p->host_review || !gtk_check_button_get_active(GTK_CHECK_BUTTON(p->host_verified))) return;
        gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, FALSE);
        gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_CANCEL, FALSE);
        gtk_dialog_set_response_sensitive(dialog, 1, FALSE); gtk_widget_set_sensitive(p->host_verified, FALSE);
        gtk_label_set_text(GTK_LABEL(p->host_detail), "Updating known_hosts and creating a backup…");
        ssh_host_key_replace_async(p->host_review, p->host_cancel, host_replaced, pane_ref(p)); return;
    }
    host_review_cancel(p); if (!p->closed && gtk_widget_get_mapped(p->root)) pane_focus(p);
}
static gboolean host_dialog_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state, gpointer data) {
    (void)controller; (void)code; (void)state; TerminalPane *p = data;
    if (key != GDK_KEY_Escape) return FALSE;
    host_response(GTK_DIALOG(p->host_dialog), GTK_RESPONSE_CANCEL, p); return TRUE;
}
static void host_prepared(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source; TerminalPane *p = data; GError *error = NULL;
    SshHostKeyReview *r = ssh_host_key_prepare_finish(result, &error);
    if (!p->closed && p->host_dialog && g_task_get_cancellable(G_TASK(result)) == p->host_cancel) {
        if (r) {
            p->host_review = r; r = NULL;
            char *text = g_strdup_printf("Session: %s\nHost: %s\n\nPreviously saved key(s):\n%s\n\nNew key: %s\n%s\n\nFile: %s\nBackup: %s\n\nA changed host key may mean the device was reset/replaced, or that you are connecting to a different device. Verify the new fingerprint using the device console or your administrator. This file is also used by other SSH applications.",
                p->profile->name, p->host_review->endpoint, p->host_review->old_fingerprints,
                p->host_review->algorithm, p->host_review->fingerprint, p->host_review->path, p->host_review->backup);
            gtk_label_set_text(GTK_LABEL(p->host_detail), text); g_free(text);
            gtk_widget_set_sensitive(p->host_verified, TRUE);
            gtk_dialog_set_response_sensitive(GTK_DIALOG(p->host_dialog), 1, TRUE);
        } else gtk_label_set_text(GTK_LABEL(p->host_detail), error->message);
    }
    g_clear_pointer(&r, ssh_host_key_unref); g_clear_error(&error); pane_unref(p);
}
static void review_host_clicked(GtkButton *button, gpointer data) {
    (void)button; TerminalPane *p = data;
    if (p->closed || !p->host_diagnostics || p->reconnect != RECONNECT_NONE) return;
    if (p->host_dialog) { gtk_window_present(GTK_WINDOW(p->host_dialog)); return; }
    GtkRoot *root = gtk_widget_get_root(p->root); if (!GTK_IS_WINDOW(root)) return;
    p->host_cancel = g_cancellable_new();
    p->host_dialog = gtk_dialog_new_with_buttons("SSH Host Key Changed", GTK_WINDOW(root), GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL, "Copy Fingerprint", 1, "Replace Key & Reconnect", GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_add_css_class(p->host_dialog, "devterm-dialog");
    gtk_window_set_destroy_with_parent(GTK_WINDOW(p->host_dialog), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(p->host_dialog), GTK_RESPONSE_CANCEL);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(p->host_dialog), GTK_RESPONSE_ACCEPT, FALSE);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(p->host_dialog), 1, FALSE);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(p->host_dialog));
    gtk_widget_set_margin_start(box, 20); gtk_widget_set_margin_end(box, 20); gtk_widget_set_margin_top(box, 20); gtk_widget_set_margin_bottom(box, 20);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_size_request(scroll, 620, 350);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    p->host_detail = gtk_label_new("Inspecting the SSH configuration and retrieving the candidate host key…");
    gtk_label_set_wrap(GTK_LABEL(p->host_detail), TRUE); gtk_label_set_wrap_mode(GTK_LABEL(p->host_detail), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(p->host_detail), 78); gtk_label_set_selectable(GTK_LABEL(p->host_detail), TRUE);
    gtk_label_set_xalign(GTK_LABEL(p->host_detail), 0); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), p->host_detail);
    gtk_box_append(GTK_BOX(box), scroll);
    p->host_verified = gtk_check_button_new_with_label("I verified this fingerprint using the device console or my administrator.");
    gtk_widget_set_sensitive(p->host_verified, FALSE); gtk_box_append(GTK_BOX(box), p->host_verified);
    g_signal_connect(p->host_verified, "toggled", G_CALLBACK(host_verified), p);
    g_signal_connect(p->host_dialog, "response", G_CALLBACK(host_response), p);
    g_signal_connect(p->host_dialog, "destroy", G_CALLBACK(host_dialog_destroyed), p);
    GtkEventController *keys = gtk_event_controller_key_new(); gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(host_dialog_key), p); gtk_widget_add_controller(p->host_dialog, keys);
    gtk_window_present(GTK_WINDOW(p->host_dialog));
    gtk_widget_grab_focus(gtk_dialog_get_widget_for_response(GTK_DIALOG(p->host_dialog), GTK_RESPONSE_CANCEL));
    ssh_host_key_prepare_async(p->profile, p->host_diagnostics, p->host_cancel, host_prepared, pane_ref(p));
}
void pane_set_reconnect_handler(TerminalPane *p, PaneEvent callback) { p->reconnect_requested = callback; }
static void clear_reconnect(TerminalPane *p) {
    if (p->reconnect_idle) { g_source_remove(p->reconnect_idle); p->reconnect_idle = 0; }
    if (p->stop_timeout) { g_source_remove(p->stop_timeout); p->stop_timeout = 0; }
    if (p->reconnect_fd >= 0) { close(p->reconnect_fd); p->reconnect_fd = -1; }
    p->reconnect = RECONNECT_NONE;
    g_clear_object(&p->reconnect_cancel);
    if (!p->closed) vte_terminal_set_input_enabled(p->terminal, TRUE);
}
static void cancel_reconnect(TerminalPane *p) {
    /* Cancelling a password dialog can synchronously release the request and
     * clear p->reconnect_cancel. Keep the cancellable alive during emission. */
    if (p->reconnect_cancel) {
        GCancellable *cancel = g_object_ref(p->reconnect_cancel);
        g_cancellable_cancel(cancel); g_object_unref(cancel);
    }
    clear_reconnect(p);
}
static void maybe_reconnect(TerminalPane *p);

static void on_focus(GtkEventControllerFocus *controller, gpointer data) {
    (void)controller;
    TerminalPane *p = data;
    if (!p->closed && p->selected) p->selected(p, NULL, p->user_data);
}

static void on_copy(GtkWidget *widget, const char *name, GVariant *parameter, gpointer data) {
    (void)widget; (void)name; (void)parameter;
    vte_terminal_copy_clipboard_format(VTE_TERMINAL(data), VTE_FORMAT_TEXT);
}

static void on_paste(GtkWidget *widget, const char *name, GVariant *parameter, gpointer data) {
    (void)widget; (void)name; (void)parameter;
    vte_terminal_paste_clipboard(VTE_TERMINAL(data));
}

static void search_cancel_timeout(TerminalPane *p) {
    if (p->search_timeout) { g_source_remove(p->search_timeout); p->search_timeout = 0; }
}
static void search_feedback(TerminalPane *p, const char *message) {
    gtk_label_set_text(GTK_LABEL(p->search_message), message);
    gtk_widget_set_tooltip_text(p->search_message, message);
    if (*message) gtk_widget_add_css_class(p->search_entry, "error");
    else gtk_widget_remove_css_class(p->search_entry, "error");
}
static void search_enable_navigation(TerminalPane *p, gboolean enabled) {
    gtk_widget_set_sensitive(p->search_previous, enabled);
    gtk_widget_set_sensitive(p->search_next, enabled);
}
static gboolean search_find(TerminalPane *p, gboolean previous) {
    if (p->closed || !gtk_widget_get_visible(p->search_box)) return FALSE;
    search_cancel_timeout(p);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(p->search_entry));
    if (!*text) {
        vte_terminal_search_set_regex(p->terminal, NULL, 0);
        vte_terminal_unselect_all(p->terminal);
        search_feedback(p, "");
        search_enable_navigation(p, FALSE);
        return FALSE;
    }
    /* VTE needs an allocated, mapped terminal to scroll/select a match. A
     * query changed just before switching tabs is retried when it maps again. */
    if (!gtk_widget_get_mapped(GTK_WIDGET(p->terminal))) return FALSE;
    if (p->search_dirty || !vte_terminal_search_get_regex(p->terminal)) {
        /* VTE searches with PCRE2; escaping makes every query a literal string. */
        char *pattern = g_regex_escape_string(text, -1);
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(p->whole_words))) {
            /* Include Unicode combining marks so decomposed accents remain
             * part of a word too, including on older PCRE2 versions. */
            char *bounded = g_strdup_printf("(?<![\\p{L}\\p{M}\\p{N}_])(?:%s)(?![\\p{L}\\p{M}\\p{N}_])", pattern);
            g_free(pattern);
            pattern = bounded;
        }
        guint32 flags = VTE_REGEX_FLAGS_DEFAULT | PCRE2_MULTILINE | PCRE2_UCP;
        if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(p->match_case))) flags |= PCRE2_CASELESS;
        GError *error = NULL;
        VteRegex *regex = vte_regex_new_for_search(pattern, -1, flags, &error);
        g_free(pattern);
        if (!regex) {
            char *message = g_strdup_printf("Cannot search: %s", error->message);
            search_feedback(p, message);
            search_enable_navigation(p, FALSE);
            g_free(message); g_clear_error(&error);
            return FALSE;
        }
        vte_terminal_search_set_regex(p->terminal, regex, 0);
        vte_regex_unref(regex);
        p->search_dirty = FALSE;
    }
    search_enable_navigation(p, TRUE);
    gboolean found = previous ? vte_terminal_search_find_previous(p->terminal) : vte_terminal_search_find_next(p->terminal);
    if (!found) {
        /* VTE 0.76 excludes the selected line when wrapping. Reset the
         * selection so a sole matching line remains a valid result. */
        vte_terminal_unselect_all(p->terminal);
        found = previous ? vte_terminal_search_find_previous(p->terminal) : vte_terminal_search_find_next(p->terminal);
    }
    search_feedback(p, found ? "" : "Not found");
    return found;
}
static gboolean on_search_timeout(gpointer data) {
    TerminalPane *p = data;
    p->search_timeout = 0;
    search_find(p, FALSE);
    return G_SOURCE_REMOVE;
}
static void on_search_map(GtkWidget *terminal, gpointer data) {
    (void)terminal;
    TerminalPane *p = data;
    if (!p->closed && gtk_widget_get_visible(p->search_box) &&
        (p->search_dirty || !vte_terminal_search_get_regex(p->terminal)) &&
        *gtk_editable_get_text(GTK_EDITABLE(p->search_entry))) {
        search_cancel_timeout(p);
        p->search_timeout = g_timeout_add(150, on_search_timeout, p);
    }
}
static void on_search_changed(GObject *object, gpointer data) {
    (void)object;
    TerminalPane *p = data;
    if (p->closed) return;
    search_cancel_timeout(p);
    p->search_dirty = TRUE;
    vte_terminal_search_set_regex(p->terminal, NULL, 0);
    vte_terminal_unselect_all(p->terminal);
    search_feedback(p, "");
    gboolean has_text = *gtk_editable_get_text(GTK_EDITABLE(p->search_entry)) != '\0';
    search_enable_navigation(p, has_text);
    if (gtk_widget_get_visible(p->search_box) && has_text)
        p->search_timeout = g_timeout_add(150, on_search_timeout, p);
}
static void search_close(TerminalPane *p) {
    search_cancel_timeout(p);
    gtk_widget_set_visible(p->search_box, FALSE);
    vte_terminal_search_set_regex(p->terminal, NULL, 0);
    vte_terminal_unselect_all(p->terminal);
    search_feedback(p, "");
    pane_focus(p);
}
static void on_search_close(GtkButton *button, gpointer data) { (void)button; search_close(data); }
static void on_search_previous(GtkButton *button, gpointer data) {
    (void)button;
    TerminalPane *p = data;
    search_find(p, TRUE);
}
static void on_search_next(GtkButton *button, gpointer data) {
    (void)button;
    TerminalPane *p = data;
    search_find(p, FALSE);
}
static gboolean on_search_key(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
    (void)controller; (void)keycode;
    TerminalPane *p = data;
    if (p->closed || !gtk_widget_get_visible(p->search_box)) return FALSE;
    if (keyval == GDK_KEY_Escape) { search_close(p); return TRUE; }
    GtkRoot *root = gtk_widget_get_root(p->root);
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    if (focus && gtk_widget_is_ancestor(focus, p->search_box) &&
        (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
        !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))) {
        search_find(p, (state & GDK_SHIFT_MASK) != 0);
        return TRUE;
    }
    return FALSE;
}
static void build_search(TerminalPane *p) {
    p->search_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(p->search_box, "search-bar");
    gtk_widget_set_margin_start(p->search_box, 4); gtk_widget_set_margin_end(p->search_box, 4);
    gtk_widget_set_margin_top(p->search_box, 4); gtk_widget_set_margin_bottom(p->search_box, 4);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    p->search_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->search_entry), "Find in this pane");
    gtk_accessible_update_property(GTK_ACCESSIBLE(p->search_entry), GTK_ACCESSIBLE_PROPERTY_LABEL, "Find in this pane", -1);
    gtk_editable_set_width_chars(GTK_EDITABLE(p->search_entry), 8);
    gtk_widget_set_hexpand(p->search_entry, TRUE);
    gtk_box_append(GTK_BOX(row), p->search_entry);
    p->search_previous = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(p->search_previous, "Previous match (Shift+Enter)");
    gtk_accessible_update_property(GTK_ACCESSIBLE(p->search_previous), GTK_ACCESSIBLE_PROPERTY_LABEL, "Previous match", -1);
    g_signal_connect(p->search_previous, "clicked", G_CALLBACK(on_search_previous), p);
    gtk_box_append(GTK_BOX(row), p->search_previous);
    p->search_next = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(p->search_next, "Next match (Enter)");
    gtk_accessible_update_property(GTK_ACCESSIBLE(p->search_next), GTK_ACCESSIBLE_PROPERTY_LABEL, "Next match", -1);
    g_signal_connect(p->search_next, "clicked", G_CALLBACK(on_search_next), p);
    gtk_box_append(GTK_BOX(row), p->search_next);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_tooltip_text(close, "Close search (Esc)");
    gtk_accessible_update_property(GTK_ACCESSIBLE(close), GTK_ACCESSIBLE_PROPERTY_LABEL, "Close search", -1);
    g_signal_connect(close, "clicked", G_CALLBACK(on_search_close), p);
    gtk_box_append(GTK_BOX(row), close);
    gtk_box_append(GTK_BOX(p->search_box), row);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    p->match_case = gtk_check_button_new_with_label("Match case");
    gtk_box_append(GTK_BOX(row), p->match_case);
    p->search_message = gtk_label_new("");
    gtk_widget_set_hexpand(p->search_message, TRUE);
    gtk_widget_set_halign(p->search_message, GTK_ALIGN_END);
    gtk_label_set_ellipsize(GTK_LABEL(p->search_message), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(row), p->search_message);
    gtk_box_append(GTK_BOX(p->search_box), row);
    p->whole_words = gtk_check_button_new_with_label("Match whole word");
    gtk_widget_set_tooltip_text(p->whole_words, "Match only when no letter, accent mark, number or underscore borders the query");
    gtk_box_append(GTK_BOX(p->search_box), p->whole_words);
    gtk_box_append(GTK_BOX(p->root), p->search_box);
    gtk_widget_set_visible(p->search_box, FALSE);
    search_enable_navigation(p, FALSE);
    g_signal_connect(p->search_entry, "changed", G_CALLBACK(on_search_changed), p);
    g_signal_connect(p->match_case, "toggled", G_CALLBACK(on_search_changed), p);
    g_signal_connect(p->whole_words, "toggled", G_CALLBACK(on_search_changed), p);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_search_key), p);
    gtk_widget_add_controller(p->root, keys);
}

static void on_child_exit(VteTerminal *terminal, int status, gpointer data) {
    (void)terminal;
    TerminalPane *p = data;
    p->running = FALSE;
    p->exited = TRUE;
    p->child = 0;
    if (p->closed) return;
    if (p->reconnect == RECONNECT_STOPPING) { maybe_reconnect(p); return; }
    if (p->reconnect == RECONNECT_STARTING) clear_reconnect(p);
    if (p->host_diagnostics) { pane_message(p, "SSH host key has changed. Review the fingerprint before reconnecting."); return; }
    char *message;
    const char *kind = p->profile ? "Session" : "Shell";
    if (WIFEXITED(status)) message = g_strdup_printf("%s exited (code %d). Output is retained; close the pane or reconnect.", kind, WEXITSTATUS(status));
    else if (WIFSIGNALED(status)) message = g_strdup_printf("%s ended (signal %d). Output is retained.", kind, WTERMSIG(status));
    else message = g_strdup_printf("%s ended. Output is retained.", kind);
    pane_message(p, message);
    g_free(message);
}

static void on_label_click(GtkGestureClick *gesture, int presses, double x, double y, gpointer data) {
    (void)gesture; (void)presses; (void)x; (void)y;
    TerminalPane *p = data;
    if (!p->closed && p->selected) p->selected(p, NULL, p->user_data);
    pane_focus(p);
}

static void on_spawn(VteTerminal *terminal, GPid pid, GError *error, gpointer data) {
    (void)terminal;
    TerminalPane *p = data;
    p->starting = FALSE;
    if (p->closed || p->disconnecting) {
        if (pid > 0 && !p->exited) { p->child = pid; kill(pid, SIGHUP); }
        if (!p->closed && error && p->reconnect != RECONNECT_STOPPING) pane_message(p, "Session connection cancelled");
    } else if (error) {
        if (p->reconnect == RECONNECT_STARTING) clear_reconnect(p);
        char *message = g_strdup_printf("Cannot start %s: %s", p->profile ? "session" : "shell", error->message);
        pane_message(p, message);
        g_free(message);
    } else {
        if (!p->exited) {
            p->child = pid;
            p->running = TRUE;
            if (p->reconnect == RECONNECT_STARTING) clear_reconnect(p);
            pane_message(p, p->profile ? p->profile->name : "Terminal");
        }
    }
    maybe_reconnect(p);
    pane_unref(p);
}

static void spawn_session(TerminalPane *p, int fd) {
    char *self = g_file_read_link("/proc/self/exe", NULL);
    char *directory = self ? g_path_get_dirname(self) : g_get_current_dir();
    char *helper = g_build_filename(directory, "devterm-connect", NULL);
    g_free(self); g_free(directory);
    host_notice_clear(p);
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets) != 0) {
        close(fd); g_free(helper); clear_reconnect(p); pane_message(p, "Cannot create session status channel"); return;
    }
    p->host_socket = sockets[0];
    p->host_watch = g_unix_fd_add_full(G_PRIORITY_DEFAULT, p->host_socket, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                     host_notice, pane_ref(p), (GDestroyNotify)pane_unref);
    const char *argv[] = {helper, "--session", NULL}; int fds[] = {fd, sockets[1]}, maps[] = {3, 4};
    p->starting = TRUE;
    /* VTE owns both passed descriptors, including on spawn failure. */
    vte_terminal_spawn_with_fds_async(p->terminal, VTE_PTY_DEFAULT, p->working_directory, argv, NULL,
        fds, 2, maps, 2, G_SPAWN_DEFAULT, NULL, NULL, NULL, -1, p->spawn_cancel, on_spawn, pane_ref(p));
    g_free(helper);
}
static gboolean restart_session(gpointer data) {
    TerminalPane *p = data; p->reconnect_idle = 0;
    if (p->closed || p->reconnect != RECONNECT_STOPPING) return G_SOURCE_REMOVE;
    if (p->stop_timeout) { g_source_remove(p->stop_timeout); p->stop_timeout = 0; }
    g_clear_object(&p->spawn_cancel); p->spawn_cancel = g_cancellable_new();
    p->exited = FALSE; p->disconnecting = FALSE;
    p->reconnect = RECONNECT_STARTING;
    /* VTE 0.76 ignores API reset while input is disabled. Enable only for
     * this synchronous call; no user input is dispatched in between. */
    vte_terminal_set_input_enabled(p->terminal, TRUE);
    vte_terminal_reset(p->terminal, TRUE, FALSE);
    vte_terminal_set_input_enabled(p->terminal, FALSE);
    /* reset(clear_history=FALSE) leaves the active screen unchanged. Return
     * to the normal buffer and append below its visible content. Switching
     * screens can restore a cursor above old output, which must not be
     * overwritten. These bytes are display output, never child input. */
    char *restore = g_strdup_printf("\033[?47l\033[%ld;1H\r\n", vte_terminal_get_row_count(p->terminal));
    vte_terminal_feed(p->terminal, restore, -1); g_free(restore);
    pane_message(p, "Reconnecting…");
    int fd = p->reconnect_fd; p->reconnect_fd = -1;
    spawn_session(p, fd);
    return G_SOURCE_REMOVE;
}
static void maybe_reconnect(TerminalPane *p) {
    if (p->closed || p->reconnect != RECONNECT_STOPPING || p->starting || p->running || p->child > 0 || p->reconnect_idle) return;
    /* child-exited has a VTE default handler. Respawn only after signal
     * dispatch and the old spawn callback have both finished. */
    p->reconnect_idle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, restart_session, pane_ref(p), (GDestroyNotify)pane_unref);
}
static gboolean stop_timed_out(gpointer data) {
    TerminalPane *p = data; p->stop_timeout = 0;
    clear_reconnect(p);
    pane_message(p, "Reconnect stopped: previous connection has not exited. Wait for it to end and retry.");
    return G_SOURCE_REMOVE;
}

static TerminalPane *pane_create(const char *working_directory, const SessionProfile *profile, const char *password, PaneEvent selected, PaneEvent status, gpointer user_data) {
    TerminalPane *p = g_new0(TerminalPane, 1);
    p->reconnect_fd = -1; p->host_socket = -1;
    p->refs = 1;
    p->selected = selected;
    p->status = status;
    p->user_data = user_data;
    p->profile = profile ? session_profile_copy(profile) : NULL;
    p->spawn_cancel = g_cancellable_new();
    p->working_directory = g_strdup(working_directory && g_path_is_absolute(working_directory) &&
                                   g_file_test(working_directory, G_FILE_TEST_IS_DIR) ? working_directory : g_get_home_dir());
    p->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref_sink(p->root);
    gtk_widget_add_css_class(p->root, "terminal-pane");
    gtk_widget_set_overflow(p->root, GTK_OVERFLOW_HIDDEN);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(header, "pane-header");
    p->label = gtk_label_new(profile ? "Starting session…" : "Starting shell…");
    gtk_widget_set_hexpand(p->label, TRUE); gtk_label_set_xalign(GTK_LABEL(p->label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(p->label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(p->label, "pane-label");
    p->state = gtk_label_new("Selected"); gtk_widget_add_css_class(p->state, "pane-state");
    gtk_widget_set_visible(p->state, FALSE);
    gtk_box_append(GTK_BOX(header), p->label); gtk_box_append(GTK_BOX(header), p->state);
    p->host_review_button = gtk_button_new_with_label("Review Host Key…");
    gtk_widget_set_visible(p->host_review_button, FALSE);
    g_signal_connect(p->host_review_button, "clicked", G_CALLBACK(review_host_clicked), p);
    gtk_box_append(GTK_BOX(header), p->host_review_button);
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_label_click), p);
    gtk_widget_add_controller(header, GTK_EVENT_CONTROLLER(click));
    gtk_box_append(GTK_BOX(p->root), header);
    p->terminal = VTE_TERMINAL(vte_terminal_new());
    GdkRGBA foreground, background, cursor, palette[16];
    gdk_rgba_parse(&foreground, "#E2E8F0"); gdk_rgba_parse(&background, "#101723"); gdk_rgba_parse(&cursor, "#5DD6C0");
    const char *colors[] = {"#202B3B", "#FCA5A5", "#86EFAC", "#FDE68A", "#93C5FD", "#C4B5FD", "#67E8F9", "#CBD5E1",
        "#8293AA", "#FECACA", "#BBF7D0", "#FEF3C7", "#BFDBFE", "#DDD6FE", "#A5F3FC", "#F8FAFC"};
    for (guint i = 0; i < G_N_ELEMENTS(colors); i++) gdk_rgba_parse(&palette[i], colors[i]);
    vte_terminal_set_colors(p->terminal, &foreground, &background, palette, 16);
    vte_terminal_set_color_cursor(p->terminal, &cursor);
    vte_terminal_set_scrollback_lines(p->terminal, 10000);
    vte_terminal_set_scroll_on_output(p->terminal, FALSE);
    vte_terminal_set_scroll_on_keystroke(p->terminal, TRUE);
    vte_terminal_search_set_wrap_around(p->terminal, TRUE);
    build_search(p);
    gtk_widget_set_hexpand(GTK_WIDGET(p->terminal), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(p->terminal), TRUE);
    GtkWidget *terminal_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_direction(terminal_row, GTK_TEXT_DIR_LTR);
    gtk_widget_set_vexpand(terminal_row, TRUE);
    gtk_box_append(GTK_BOX(terminal_row), GTK_WIDGET(p->terminal));
    GtkWidget *scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL,
        gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(p->terminal)));
    gtk_widget_set_focusable(scrollbar, FALSE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(scrollbar), GTK_ACCESSIBLE_PROPERTY_LABEL, "Pane scrollback", -1);
    /* Observe the press before GtkRange claims a drag. Do not claim the
     * sequence: the scrollbar must keep receiving its normal pointer events. */
    click = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_label_click), p);
    gtk_widget_add_controller(scrollbar, GTK_EVENT_CONTROLLER(click));
    gtk_box_append(GTK_BOX(terminal_row), scrollbar);
    gtk_box_append(GTK_BOX(p->root), terminal_row);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(on_focus), p);
    gtk_widget_add_controller(p->root, focus);
    g_signal_connect(p->terminal, "child-exited", G_CALLBACK(on_child_exit), p);
    g_signal_connect(p->terminal, "map", G_CALLBACK(on_search_map), p);

    GSimpleActionGroup *actions = g_simple_action_group_new();
    GSimpleAction *copy = g_simple_action_new("copy", NULL);
    GSimpleAction *paste = g_simple_action_new("paste", NULL);
    g_signal_connect(copy, "activate", G_CALLBACK(on_copy), p->terminal);
    g_signal_connect(paste, "activate", G_CALLBACK(on_paste), p->terminal);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(copy));
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(paste));
    gtk_widget_insert_action_group(GTK_WIDGET(p->terminal), "term", G_ACTION_GROUP(actions));
    g_object_unref(copy); g_object_unref(paste); g_object_unref(actions);
    GtkShortcutController *shortcuts = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
    gtk_shortcut_controller_add_shortcut(shortcuts, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_c, GDK_CONTROL_MASK | GDK_SHIFT_MASK), gtk_named_action_new("term.copy")));
    gtk_shortcut_controller_add_shortcut(shortcuts, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_v, GDK_CONTROL_MASK | GDK_SHIFT_MASK), gtk_named_action_new("term.paste")));
    gtk_widget_add_controller(GTK_WIDGET(p->terminal), GTK_EVENT_CONTROLLER(shortcuts));

    if (profile) {
        GError *error = NULL;
        int fd = session_launch_fd(profile, password, &error);
        if (fd < 0) { gtk_label_set_text(GTK_LABEL(p->label), error->message); g_clear_error(&error); return p; }
        spawn_session(p, fd); return p;
    }
    const char *shell = g_getenv("SHELL");
    if (!shell || !g_path_is_absolute(shell) || !g_file_test(shell, G_FILE_TEST_IS_REGULAR) ||
        !g_file_test(shell, G_FILE_TEST_IS_EXECUTABLE)) shell = "/bin/sh";
    char *argv[] = {(char *)shell, NULL};
    p->starting = TRUE;
    vte_terminal_spawn_async(p->terminal, VTE_PTY_DEFAULT, p->working_directory, argv, NULL,
                             G_SPAWN_DEFAULT, NULL, NULL, NULL, -1, p->spawn_cancel, on_spawn, pane_ref(p));
    return p;
}
TerminalPane *pane_new(const char *directory, PaneEvent selected, PaneEvent status, gpointer data) {
    return pane_create(directory, NULL, NULL, selected, status, data);
}
TerminalPane *pane_new_session(const SessionProfile *profile, const char *password, PaneEvent selected, PaneEvent status, gpointer data) {
    return pane_create(NULL, profile, password, selected, status, data);
}
const SessionProfile *pane_session_profile(TerminalPane *p) { return p ? p->profile : NULL; }
GCancellable *pane_prepare_reconnect(TerminalPane *p) {
    if (!p || p->closed || !p->profile) return NULL;
    if (p->reconnect != RECONNECT_NONE) { pane_message(p, "Reconnect already in progress for this pane"); return NULL; }
    host_review_cancel(p);
    p->reconnect = RECONNECT_PASSWORD;
    p->reconnect_cancel = g_cancellable_new();
    vte_terminal_set_input_enabled(p->terminal, FALSE);
    pane_message(p, "Preparing reconnect… Existing connection is retained until credentials are ready.");
    return p->reconnect_cancel;
}
void pane_reconnect_ready(TerminalPane *p, const SessionProfile *profile, const char *password) {
    if (p->closed || p->reconnect != RECONNECT_PASSWORD || g_cancellable_is_cancelled(p->reconnect_cancel)) return;
    GError *error = NULL;
    /* Validate/serialize before touching the old connection. The request's
     * profile is a snapshot of this pane, with its latest remember choice. */
    int fd = session_launch_fd(profile, password, &error);
    if (fd < 0) {
        clear_reconnect(p); pane_message(p, error->message); g_clear_error(&error); return;
    }
    p->profile->remember = profile->remember;
    p->reconnect_fd = fd;
    p->reconnect = RECONNECT_STOPPING;
    p->disconnecting = TRUE;
    pane_message(p, "Stopping previous connection…");
    g_cancellable_cancel(p->spawn_cancel);
    if (p->child > 0) kill(p->child, SIGHUP);
    p->stop_timeout = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 5, stop_timed_out, pane_ref(p), (GDestroyNotify)pane_unref);
    maybe_reconnect(p);
}
void pane_finish_reconnect_request(TerminalPane *p) {
    if (p->closed || p->reconnect != RECONNECT_PASSWORD) return;
    clear_reconnect(p);
    pane_message(p, p->running && !p->disconnecting ? p->profile->name :
        "Reconnect cancelled; pane output is retained.");
}
gboolean pane_is_reconnecting(TerminalPane *p) { return p && !p->closed && p->reconnect != RECONNECT_NONE; }
void pane_disconnect(TerminalPane *p) {
    if (!p || p->closed || !p->profile) return;
    host_review_cancel(p);
    cancel_reconnect(p);
    p->disconnecting = TRUE; g_cancellable_cancel(p->spawn_cancel);
    if (p->child > 0) kill(p->child, SIGHUP);
}

GtkWidget *pane_widget(TerminalPane *p) { return p->root; }
void pane_set_active(TerminalPane *p, gboolean active) {
    gtk_widget_set_visible(p->state, active);
    if (active) gtk_widget_add_css_class(p->root, "active-pane");
    else gtk_widget_remove_css_class(p->root, "active-pane");
}
void pane_focus(TerminalPane *p) { if (!p->closed) gtk_widget_grab_focus(GTK_WIDGET(p->terminal)); }
void pane_search_open(TerminalPane *p) {
    if (!p || p->closed) return;
    if (p->selected) p->selected(p, NULL, p->user_data);
    gtk_widget_set_visible(p->search_box, TRUE);
    gtk_widget_grab_focus(p->search_entry);
    gtk_editable_select_region(GTK_EDITABLE(p->search_entry), 0, -1);
    if (*gtk_editable_get_text(GTK_EDITABLE(p->search_entry))) search_find(p, FALSE);
}
GBytes *pane_snapshot_text(TerminalPane *p, GError **error) {
    if (!p || p->closed) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Select an open terminal pane first");
        return NULL;
    }
    /* This VTE API includes the active buffer's scrollback and joins soft
     * wraps. Only memory extraction runs on GTK's thread; file I/O must not. */
    GOutputStream *stream = g_memory_output_stream_new_resizable();
    gboolean ok = vte_terminal_write_contents_sync(p->terminal, stream, VTE_WRITE_DEFAULT, NULL, error);
    if (ok) ok = g_output_stream_close(stream, NULL, error);
    GBytes *text = ok ? g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(stream)) : NULL;
    g_object_unref(stream);
    return text;
}
gboolean pane_copy_all_text(TerminalPane *p, GError **error) {
    GBytes *text = pane_snapshot_text(p, error);
    if (!text) return FALSE;
    gsize size;
    const char *data = g_bytes_get_data(text, &size);
    char *string = size ? g_strndup(data, size) : g_strdup("");
    gdk_clipboard_set_text(gtk_widget_get_clipboard(p->root), string);
    g_free(string);
    g_bytes_unref(text);
    return TRUE;
}
gboolean pane_is_running(TerminalPane *p) { return p && !p->closed && !p->disconnecting && p->reconnect == RECONNECT_NONE && p->running; }
gboolean pane_has_live_session(TerminalPane *p) { return p && !p->closed && (p->starting || p->running); }
char *pane_dup_working_directory(TerminalPane *p) {
    /* Read the local shell process, never a remote OSC 7 directory from SSH. */
    if (p && !p->closed && p->child > 0) {
        char *proc_path = g_strdup_printf("/proc/%ld/cwd", (long)p->child);
        char *directory = g_file_read_link(proc_path, NULL);
        g_free(proc_path);
        if (directory && g_path_is_absolute(directory) && g_file_test(directory, G_FILE_TEST_IS_DIR)) return directory;
        g_free(directory);
    }
    if (p && !p->closed && p->starting) return g_strdup(p->working_directory);
    return g_strdup(g_get_home_dir());
}
void pane_send(TerminalPane *p, const char *command, gboolean send_enter) {
    if (!pane_is_running(p)) return;
    vte_terminal_feed_child(p->terminal, command, -1);
    if (send_enter) vte_terminal_feed_child(p->terminal, "\r", 1);
    pane_focus(p);
}
void pane_close(TerminalPane *p) {
    if (!p || p->closed) return;
    p->closed = TRUE;
    host_notice_clear(p);
    cancel_reconnect(p);
    g_cancellable_cancel(p->spawn_cancel);
    search_cancel_timeout(p);
    if (p->child > 0) kill(p->child, SIGHUP);
    g_signal_handlers_disconnect_by_data(p->terminal, p);
    pane_unref(p);
}
