#include "pane.h"
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

struct TerminalPane {
    int refs;
    GtkWidget *root;
    VteTerminal *terminal;
    GtkWidget *label;
    GPid child;
    gboolean closed;
    gboolean running;
    gboolean exited;
    PaneEvent selected;
    PaneEvent status;
    gpointer user_data;
};

static TerminalPane *pane_ref(TerminalPane *p) { p->refs++; return p; }
static void pane_unref(TerminalPane *p) {
    if (--p->refs) return;
    g_object_unref(p->root);
    g_free(p);
}

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

static void on_exit(VteTerminal *terminal, int status, gpointer data) {
    (void)terminal;
    TerminalPane *p = data;
    p->running = FALSE;
    p->exited = TRUE;
    p->child = 0;
    if (p->closed) return;
    char *message;
    if (WIFEXITED(status)) message = g_strdup_printf("Shell exited (code %d). Close this pane or create another terminal.", WEXITSTATUS(status));
    else if (WIFSIGNALED(status)) message = g_strdup_printf("Shell ended (signal %d). Close this pane or create another terminal.", WTERMSIG(status));
    else message = g_strdup("Shell ended. Close this pane or create another terminal.");
    gtk_label_set_text(GTK_LABEL(p->label), message);
    if (p->status) p->status(p, message, p->user_data);
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
    if (p->closed) {
        if (pid > 0) kill(pid, SIGHUP);
    } else if (error) {
        char *message = g_strdup_printf("Cannot start shell: %s", error->message);
        gtk_label_set_text(GTK_LABEL(p->label), message);
        if (p->status) p->status(p, message, p->user_data);
        g_free(message);
    } else {
        if (!p->exited) {
            p->child = pid;
            p->running = TRUE;
            gtk_label_set_text(GTK_LABEL(p->label), "Terminal");
        }
    }
    pane_unref(p);
}

TerminalPane *pane_new(PaneEvent selected, PaneEvent status, gpointer user_data) {
    TerminalPane *p = g_new0(TerminalPane, 1);
    p->refs = 1;
    p->selected = selected;
    p->status = status;
    p->user_data = user_data;
    p->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref_sink(p->root);
    gtk_widget_add_css_class(p->root, "terminal-pane");
    p->label = gtk_label_new("Starting shell…");
    gtk_widget_set_halign(p->label, GTK_ALIGN_START);
    gtk_widget_add_css_class(p->label, "pane-label");
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_label_click), p);
    gtk_widget_add_controller(p->label, GTK_EVENT_CONTROLLER(click));
    gtk_box_append(GTK_BOX(p->root), p->label);
    p->terminal = VTE_TERMINAL(vte_terminal_new());
    vte_terminal_set_scrollback_lines(p->terminal, 10000);
    gtk_widget_set_hexpand(GTK_WIDGET(p->terminal), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(p->terminal), TRUE);
    gtk_box_append(GTK_BOX(p->root), GTK_WIDGET(p->terminal));
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(on_focus), p);
    gtk_widget_add_controller(GTK_WIDGET(p->terminal), focus);
    g_signal_connect(p->terminal, "child-exited", G_CALLBACK(on_exit), p);

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

    const char *shell = g_getenv("SHELL");
    if (!shell || !g_path_is_absolute(shell) || !g_file_test(shell, G_FILE_TEST_IS_REGULAR) ||
        !g_file_test(shell, G_FILE_TEST_IS_EXECUTABLE)) shell = "/bin/sh";
    char *argv[] = {(char *)shell, NULL};
    vte_terminal_spawn_async(p->terminal, VTE_PTY_DEFAULT, g_get_home_dir(), argv, NULL,
                             G_SPAWN_DEFAULT, NULL, NULL, NULL, -1, NULL, on_spawn, pane_ref(p));
    return p;
}

GtkWidget *pane_widget(TerminalPane *p) { return p->root; }
void pane_set_active(TerminalPane *p, gboolean active) {
    if (active) gtk_widget_add_css_class(p->root, "active-pane");
    else gtk_widget_remove_css_class(p->root, "active-pane");
}
void pane_focus(TerminalPane *p) { if (!p->closed) gtk_widget_grab_focus(GTK_WIDGET(p->terminal)); }
gboolean pane_is_running(TerminalPane *p) { return p && !p->closed && p->running; }
void pane_send(TerminalPane *p, const char *command, gboolean send_enter) {
    vte_terminal_feed_child(p->terminal, command, -1);
    if (send_enter) vte_terminal_feed_child(p->terminal, "\r", 1);
    pane_focus(p);
}
void pane_close(TerminalPane *p) {
    if (!p || p->closed) return;
    p->closed = TRUE;
    if (p->child > 0) kill(p->child, SIGHUP);
    g_signal_handlers_disconnect_by_data(p->terminal, p);
    pane_unref(p);
}
