#include "pane.h"
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>

struct TerminalPane {
    int refs;
    GtkWidget *root;
    VteTerminal *terminal;
    GtkWidget *label;
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
    PaneEvent selected;
    PaneEvent status;
    gpointer user_data;
};

static TerminalPane *pane_ref(TerminalPane *p) { p->refs++; return p; }
static void pane_unref(TerminalPane *p) {
    if (--p->refs) return;
    g_object_unref(p->root);
    g_free(p->working_directory);
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
    g_signal_connect(p->search_previous, "clicked", G_CALLBACK(on_search_previous), p);
    gtk_box_append(GTK_BOX(row), p->search_previous);
    p->search_next = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(p->search_next, "Next match (Enter)");
    g_signal_connect(p->search_next, "clicked", G_CALLBACK(on_search_next), p);
    gtk_box_append(GTK_BOX(row), p->search_next);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_tooltip_text(close, "Close search (Esc)");
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

static void on_exit(VteTerminal *terminal, int status, gpointer data) {
    (void)terminal;
    TerminalPane *p = data;
    p->running = FALSE;
    p->starting = FALSE;
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
    p->starting = FALSE;
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

TerminalPane *pane_new(const char *working_directory, PaneEvent selected, PaneEvent status, gpointer user_data) {
    TerminalPane *p = g_new0(TerminalPane, 1);
    p->refs = 1;
    p->selected = selected;
    p->status = status;
    p->user_data = user_data;
    p->working_directory = g_strdup(working_directory && g_path_is_absolute(working_directory) &&
                                   g_file_test(working_directory, G_FILE_TEST_IS_DIR) ? working_directory : g_get_home_dir());
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
    vte_terminal_search_set_wrap_around(p->terminal, TRUE);
    build_search(p);
    gtk_widget_set_hexpand(GTK_WIDGET(p->terminal), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(p->terminal), TRUE);
    gtk_box_append(GTK_BOX(p->root), GTK_WIDGET(p->terminal));
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(on_focus), p);
    gtk_widget_add_controller(p->root, focus);
    g_signal_connect(p->terminal, "child-exited", G_CALLBACK(on_exit), p);
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

    const char *shell = g_getenv("SHELL");
    if (!shell || !g_path_is_absolute(shell) || !g_file_test(shell, G_FILE_TEST_IS_REGULAR) ||
        !g_file_test(shell, G_FILE_TEST_IS_EXECUTABLE)) shell = "/bin/sh";
    char *argv[] = {(char *)shell, NULL};
    p->starting = TRUE;
    vte_terminal_spawn_async(p->terminal, VTE_PTY_DEFAULT, p->working_directory, argv, NULL,
                             G_SPAWN_DEFAULT, NULL, NULL, NULL, -1, NULL, on_spawn, pane_ref(p));
    return p;
}

GtkWidget *pane_widget(TerminalPane *p) { return p->root; }
void pane_set_active(TerminalPane *p, gboolean active) {
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
gboolean pane_is_running(TerminalPane *p) { return p && !p->closed && p->running; }
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
    vte_terminal_feed_child(p->terminal, command, -1);
    if (send_enter) vte_terminal_feed_child(p->terminal, "\r", 1);
    pane_focus(p);
}
void pane_close(TerminalPane *p) {
    if (!p || p->closed) return;
    p->closed = TRUE;
    search_cancel_timeout(p);
    if (p->child > 0) kill(p->child, SIGHUP);
    g_signal_handlers_disconnect_by_data(p->terminal, p);
    pane_unref(p);
}
