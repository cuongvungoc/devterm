#include "commands.h"
#include "pane.h"
#include <gtk/gtk.h>
#include <string.h>

typedef struct LayoutNode LayoutNode;
typedef struct App App;
struct LayoutNode {
    LayoutNode *parent, *first, *second;
    TerminalPane *pane;
    GtkWidget *widget;
};
struct App {
    GtkWidget *window, *workspace, *quick_bar, *status, *manage_dialog, *manage_rows;
    LayoutNode *layout;
    TerminalPane *active;
    GPtrArray *commands;
    char *config_path;
    gboolean config_valid;
};

static void refresh_commands(App *app);
static void refresh_manage(App *app);
static void show_editor(App *app, const char *id);

static void status(App *app, const char *message) { gtk_label_set_text(GTK_LABEL(app->status), message); }
static void on_pane_selected(TerminalPane *pane, const char *message, gpointer data) {
    (void)message;
    App *app = data;
    if (app->active) pane_set_active(app->active, FALSE);
    app->active = pane;
    pane_set_active(pane, TRUE);
}
static void on_pane_status(TerminalPane *pane, const char *message, gpointer data) {
    (void)pane;
    status(data, message);
}

static LayoutNode *leaf_new(App *app) {
    LayoutNode *node = g_new0(LayoutNode, 1);
    node->pane = pane_new(on_pane_selected, on_pane_status, app);
    node->widget = pane_widget(node->pane);
    return node;
}
static LayoutNode *first_leaf(LayoutNode *node) {
    while (node && !node->pane) node = node->first;
    return node;
}
static LayoutNode *find_pane(LayoutNode *node, TerminalPane *pane) {
    if (!node) return NULL;
    if (node->pane == pane) return node;
    LayoutNode *found = find_pane(node->first, pane);
    return found ? found : find_pane(node->second, pane);
}
static void attach_at(GtkWidget *parent, gboolean start, GtkWidget *child) {
    if (GTK_IS_PANED(parent)) {
        if (start) gtk_paned_set_start_child(GTK_PANED(parent), child);
        else gtk_paned_set_end_child(GTK_PANED(parent), child);
    } else gtk_box_append(GTK_BOX(parent), child);
}
static gboolean is_start(GtkWidget *parent, GtkWidget *child) {
    return GTK_IS_PANED(parent) && gtk_paned_get_start_child(GTK_PANED(parent)) == child;
}
static void detach_widget(GtkWidget *widget) {
    GtkWidget *parent = gtk_widget_get_parent(widget);
    if (!parent) return;
    if (GTK_IS_PANED(parent)) {
        if (gtk_paned_get_start_child(GTK_PANED(parent)) == widget)
            gtk_paned_set_start_child(GTK_PANED(parent), NULL);
        else gtk_paned_set_end_child(GTK_PANED(parent), NULL);
    } else gtk_box_remove(GTK_BOX(parent), widget);
}
static void select_first(App *app) {
    LayoutNode *node = first_leaf(app->layout);
    app->active = NULL;
    if (node) { on_pane_selected(node->pane, NULL, app); pane_focus(node->pane); }
}
static void show_empty(App *app) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    GtkWidget *label = gtk_label_new("No terminal is open");
    gtk_box_append(GTK_BOX(box), label);
    GtkWidget *button = gtk_button_new_with_label("New Terminal");
    gtk_box_append(GTK_BOX(box), button);
    gtk_box_append(GTK_BOX(app->workspace), box);
    g_object_set_data(G_OBJECT(app->workspace), "empty-button", button);
}
static void on_new(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->layout) return;
    GtkWidget *old = gtk_widget_get_first_child(app->workspace);
    if (old) gtk_box_remove(GTK_BOX(app->workspace), old);
    app->layout = leaf_new(app);
    gtk_box_append(GTK_BOX(app->workspace), app->layout->widget);
    select_first(app);
    status(app, "Terminal ready");
}
static void split_active(App *app, GtkOrientation orientation) {
    if (!app->active) { status(app, "Select a terminal pane first"); return; }
    LayoutNode *node = find_pane(app->layout, app->active);
    if (!node) { status(app, "Selected pane is no longer available"); return; }
    LayoutNode *old = g_new0(LayoutNode, 1);
    old->pane = node->pane;
    old->widget = node->widget;
    LayoutNode *fresh = leaf_new(app);
    GtkWidget *parent_widget = gtk_widget_get_parent(node->widget);
    gboolean start = is_start(parent_widget, node->widget);
    detach_widget(node->widget);
    node->pane = NULL;
    node->widget = gtk_paned_new(orientation);
    g_object_ref_sink(node->widget);
    node->first = old; node->second = fresh;
    old->parent = node; fresh->parent = node;
    gtk_paned_set_start_child(GTK_PANED(node->widget), old->widget);
    gtk_paned_set_end_child(GTK_PANED(node->widget), fresh->widget);
    attach_at(parent_widget, start, node->widget);
    on_pane_selected(fresh->pane, NULL, app);
    pane_focus(fresh->pane);
}
static void on_split_right(GtkButton *button, gpointer data) { (void)button; split_active(data, GTK_ORIENTATION_HORIZONTAL); }
static void on_split_down(GtkButton *button, gpointer data) { (void)button; split_active(data, GTK_ORIENTATION_VERTICAL); }

static void on_close_pane(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (!app->active) { status(app, "No terminal pane to close"); return; }
    LayoutNode *node = find_pane(app->layout, app->active);
    if (!node) return;
    pane_set_active(node->pane, FALSE);
    if (!node->parent) {
        detach_widget(node->widget);
        pane_close(node->pane);
        g_free(node);
        app->layout = NULL;
        app->active = NULL;
        show_empty(app);
        GtkWidget *empty_button = g_object_get_data(G_OBJECT(app->workspace), "empty-button");
        g_signal_connect(empty_button, "clicked", G_CALLBACK(on_new), app);
        status(app, "All terminal panes closed");
        return;
    }
    LayoutNode *branch = node->parent;
    LayoutNode *other = branch->first == node ? branch->second : branch->first;
    LayoutNode *grand = branch->parent;
    GtkWidget *container = gtk_widget_get_parent(branch->widget);
    gboolean start = is_start(container, branch->widget);
    detach_widget(other->widget);
    detach_widget(node->widget);
    detach_widget(branch->widget);
    attach_at(container, start, other->widget);
    other->parent = grand;
    if (grand) {
        if (grand->first == branch) grand->first = other;
        else grand->second = other;
    } else app->layout = other;
    app->active = NULL;
    pane_close(node->pane);
    g_object_unref(branch->widget);
    g_free(node); g_free(branch);
    select_first(app);
    status(app, "Pane closed");
}

static gint command_index(App *app, const char *id) {
    for (guint i = 0; i < app->commands->len; i++) {
        QuickCommand *item = g_ptr_array_index(app->commands, i);
        if (g_strcmp0(item->id, id) == 0) return (gint)i;
    }
    return -1;
}
static void save_commands(App *app) {
    GError *error = NULL;
    if (!commands_save(app->config_path, app->commands, &error)) {
        char *message = g_strdup_printf("Cannot save Quick Commands: %s", error->message);
        status(app, message); g_free(message); g_clear_error(&error);
    } else status(app, "Quick Commands saved");
}
static void on_command(GtkButton *button, gpointer data) {
    App *app = data;
    const char *id = g_object_get_data(G_OBJECT(button), "command-id");
    gint index = command_index(app, id);
    if (index < 0) return;
    if (!app->active) { status(app, "Select a terminal pane before sending a command"); return; }
    if (!pane_is_running(app->active)) { status(app, "Selected pane has no running shell. Close it and create a new terminal."); return; }
    QuickCommand *item = g_ptr_array_index(app->commands, index);
    pane_send(app->active, item->command, item->send_enter);
    status(app, item->send_enter ? "Command sent to selected pane" : "Command inserted in selected pane");
}
static void on_add(GtkButton *button, gpointer data) { (void)button; show_editor(data, NULL); }
static void on_manage(GtkButton *button, gpointer data);
static void refresh_commands(App *app) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app->quick_bar))) gtk_box_remove(GTK_BOX(app->quick_bar), child);
    GtkWidget *label = gtk_label_new("Quick Commands");
    gtk_box_append(GTK_BOX(app->quick_bar), label);
    if (app->commands->len == 0) {
        label = gtk_label_new(app->config_valid ? "No commands yet. Add one to send text to the selected pane." : "Configuration error. Fix the file to edit commands.");
        gtk_box_append(GTK_BOX(app->quick_bar), label);
        GtkWidget *add = gtk_button_new_with_label("Add Command");
        gtk_widget_set_sensitive(add, app->config_valid);
        g_signal_connect(add, "clicked", G_CALLBACK(on_add), app);
        gtk_box_append(GTK_BOX(app->quick_bar), add);
    } else {
        for (guint i = 0; i < app->commands->len; i++) {
            QuickCommand *item = g_ptr_array_index(app->commands, i);
            GtkWidget *button = gtk_button_new_with_label(item->name);
            gtk_widget_set_tooltip_text(button, item->command);
            g_object_set_data_full(G_OBJECT(button), "command-id", g_strdup(item->id), g_free);
            g_signal_connect(button, "clicked", G_CALLBACK(on_command), app);
            gtk_box_append(GTK_BOX(app->quick_bar), button);
        }
    }
    GtkWidget *manage = gtk_button_new_with_label("Manage…");
    gtk_widget_set_sensitive(manage, app->config_valid);
    g_signal_connect(manage, "clicked", G_CALLBACK(on_manage), app);
    gtk_box_append(GTK_BOX(app->quick_bar), manage);
}

typedef struct { App *app; char *id; GtkWidget *name, *command, *enter, *error; } Editor;
static void editor_free(gpointer data) { Editor *e = data; g_free(e->id); g_free(e); }
static void on_editor_response(GtkDialog *dialog, int response, gpointer data) {
    Editor *e = data;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_window_destroy(GTK_WINDOW(dialog)); return; }
    const char *name = gtk_editable_get_text(GTK_EDITABLE(e->name));
    const char *command = gtk_editable_get_text(GTK_EDITABLE(e->command));
    gboolean name_ok = FALSE, command_ok = FALSE;
    for (const char *p = name; *p; p = g_utf8_next_char(p)) if (!g_unichar_isspace(g_utf8_get_char(p))) { name_ok = TRUE; break; }
    for (const char *p = command; *p; p = g_utf8_next_char(p)) if (!g_unichar_isspace(g_utf8_get_char(p))) { command_ok = TRUE; break; }
    if (!name_ok || !command_ok) { gtk_label_set_text(GTK_LABEL(e->error), "Name and command are required."); return; }
    gint index = e->id ? command_index(e->app, e->id) : -1;
    if (e->id && index < 0) { gtk_window_destroy(GTK_WINDOW(dialog)); return; }
    QuickCommand *item;
    if (index >= 0) {
        item = g_ptr_array_index(e->app->commands, index);
        g_free(item->name); g_free(item->command);
        item->name = g_strdup(name); item->command = g_strdup(command);
        item->send_enter = gtk_check_button_get_active(GTK_CHECK_BUTTON(e->enter));
    } else {
        char *id = g_uuid_string_random();
        item = quick_command_new(id, name, command, gtk_check_button_get_active(GTK_CHECK_BUTTON(e->enter)));
        g_ptr_array_add(e->app->commands, item);
        g_free(id);
    }
    save_commands(e->app);
    refresh_commands(e->app);
    refresh_manage(e->app);
    gtk_window_destroy(GTK_WINDOW(dialog));
}
static void show_editor(App *app, const char *id) {
    if (!app->config_valid) return;
    GtkWidget *parent = app->manage_dialog ? app->manage_dialog : app->window;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(id ? "Edit Command" : "Add Command", GTK_WINDOW(parent), GTK_DIALOG_MODAL,
                                                    "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    Editor *e = g_new0(Editor, 1); e->app = app; e->id = g_strdup(id);
    g_object_set_data_full(G_OBJECT(dialog), "editor", e, editor_free);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_set_spacing(GTK_BOX(box), 8);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Name"));
    e->name = gtk_entry_new(); gtk_box_append(GTK_BOX(box), e->name);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Command"));
    e->command = gtk_entry_new(); gtk_widget_set_size_request(e->command, 420, -1);
    gtk_box_append(GTK_BOX(box), e->command);
    e->enter = gtk_check_button_new_with_label("Send Enter after command");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(e->enter), TRUE);
    gtk_box_append(GTK_BOX(box), e->enter);
    gtk_box_append(GTK_BOX(box), gtk_label_new("Input goes to the foreground program in the selected terminal (including SSH, vim or top)."));
    e->error = gtk_label_new(""); gtk_widget_add_css_class(e->error, "error"); gtk_box_append(GTK_BOX(box), e->error);
    gint index = id ? command_index(app, id) : -1;
    if (index >= 0) {
        QuickCommand *item = g_ptr_array_index(app->commands, index);
        gtk_editable_set_text(GTK_EDITABLE(e->name), item->name);
        gtk_editable_set_text(GTK_EDITABLE(e->command), item->command);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(e->enter), item->send_enter);
    }
    g_signal_connect(dialog, "response", G_CALLBACK(on_editor_response), e);
    gtk_window_present(GTK_WINDOW(dialog));
}

typedef struct { App *app; char *id; int action; } RowAction;
static void row_action_free(gpointer data) { RowAction *a = data; g_free(a->id); g_free(a); }
static void on_row_action(GtkButton *button, gpointer data) {
    (void)button;
    RowAction *a = data;
    gint index = command_index(a->app, a->id);
    if (index < 0) return;
    if (a->action == 0) { show_editor(a->app, a->id); return; }
    if (a->action == 1) g_ptr_array_remove_index(a->app->commands, index);
    else {
        gint target = index + (a->action == 2 ? -1 : 1);
        if (target < 0 || target >= (gint)a->app->commands->len) return;
        gpointer temp = a->app->commands->pdata[index];
        a->app->commands->pdata[index] = a->app->commands->pdata[target];
        a->app->commands->pdata[target] = temp;
    }
    save_commands(a->app);
    refresh_commands(a->app);
    refresh_manage(a->app);
}
static GtkWidget *row_button(App *app, const char *id, const char *text, int action, gboolean enabled) {
    GtkWidget *button = gtk_button_new_with_label(text);
    RowAction *a = g_new0(RowAction, 1); a->app = app; a->id = g_strdup(id); a->action = action;
    g_object_set_data_full(G_OBJECT(button), "row-action", a, row_action_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_row_action), a);
    gtk_widget_set_sensitive(button, enabled);
    return button;
}
static void refresh_manage(App *app) {
    if (!app->manage_rows) return;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(app->manage_rows))) gtk_box_remove(GTK_BOX(app->manage_rows), child);
    for (guint i = 0; i < app->commands->len; i++) {
        QuickCommand *item = g_ptr_array_index(app->commands, i);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *label = gtk_label_new(item->name);
        gtk_widget_set_hexpand(label, TRUE); gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(label, item->command);
        gtk_box_append(GTK_BOX(row), label);
        gtk_box_append(GTK_BOX(row), row_button(app, item->id, "←", 2, i > 0));
        gtk_box_append(GTK_BOX(row), row_button(app, item->id, "→", 3, i + 1 < app->commands->len));
        gtk_box_append(GTK_BOX(row), row_button(app, item->id, "Edit", 0, TRUE));
        gtk_box_append(GTK_BOX(row), row_button(app, item->id, "Delete", 1, TRUE));
        gtk_box_append(GTK_BOX(app->manage_rows), row);
    }
}
static void on_manage_destroy(GtkWidget *widget, gpointer data) {
    App *app = data;
    if (app->manage_dialog == widget) { app->manage_dialog = NULL; app->manage_rows = NULL; }
}
static void on_manage_response(GtkDialog *dialog, int response, gpointer data) {
    (void)response; (void)data; gtk_window_destroy(GTK_WINDOW(dialog));
}
static void on_manage(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->manage_dialog) { gtk_window_present(GTK_WINDOW(app->manage_dialog)); return; }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Manage Quick Commands", GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
                                                    "Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    app->manage_dialog = dialog;
    gtk_window_set_default_size(GTK_WINDOW(dialog), 650, 350);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_set_spacing(GTK_BOX(area), 8);
    gtk_widget_set_margin_start(area, 12); gtk_widget_set_margin_end(area, 12);
    gtk_widget_set_margin_top(area, 12); gtk_widget_set_margin_bottom(area, 12);
    GtkWidget *add = gtk_button_new_with_label("Add Command");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add), app);
    gtk_box_append(GTK_BOX(area), add);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    app->manage_rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), app->manage_rows);
    gtk_box_append(GTK_BOX(area), scroll);
    refresh_manage(app);
    g_signal_connect(dialog, "response", G_CALLBACK(on_manage_response), app);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_manage_destroy), app);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void free_layout(LayoutNode *node) {
    if (!node) return;
    if (node->pane) pane_close(node->pane);
    else { free_layout(node->first); free_layout(node->second); g_object_unref(node->widget); }
    g_free(node);
}
static void app_free(gpointer data) {
    App *app = data;
    free_layout(app->layout);
    g_ptr_array_unref(app->commands);
    g_free(app->config_path);
    g_free(app);
}
static void activate(GtkApplication *application, gpointer data) {
    (void)data;
    App *app = g_new0(App, 1);
    app->window = gtk_application_window_new(application);
    g_object_set_data_full(G_OBJECT(app->window), "app", app, app_free);
    gtk_window_set_title(GTK_WINDOW(app->window), "DevTerm");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1100, 700);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, ".terminal-pane { border: 2px solid transparent; } .terminal-pane.active-pane { border-color: #3584e4; } .pane-label { padding: 3px 8px; background: #303030; color: white; } .error { color: #c01c28; }", -1);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_window_set_child(GTK_WINDOW(app->window), outer);
    GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(outer), tools);
    GtkWidget *new_button = gtk_button_new_with_label("New Terminal");
    g_signal_connect(new_button, "clicked", G_CALLBACK(on_new), app); gtk_box_append(GTK_BOX(tools), new_button);
    GtkWidget *split_right = gtk_button_new_with_label("Split Right");
    g_signal_connect(split_right, "clicked", G_CALLBACK(on_split_right), app); gtk_box_append(GTK_BOX(tools), split_right);
    GtkWidget *split_down = gtk_button_new_with_label("Split Down");
    g_signal_connect(split_down, "clicked", G_CALLBACK(on_split_down), app); gtk_box_append(GTK_BOX(tools), split_down);
    GtkWidget *close = gtk_button_new_with_label("Close Pane");
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_pane), app); gtk_box_append(GTK_BOX(tools), close);
    app->workspace = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(app->workspace, TRUE);
    gtk_box_append(GTK_BOX(outer), app->workspace);
    GtkWidget *quick_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(quick_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    app->quick_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(quick_scroll), app->quick_bar);
    gtk_box_append(GTK_BOX(outer), quick_scroll);
    app->status = gtk_label_new("");
    gtk_widget_set_halign(app->status, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(outer), app->status);
    app->config_path = commands_config_path();
    GError *error = NULL;
    app->commands = commands_load(app->config_path, &error);
    app->config_valid = app->commands != NULL;
    if (!app->config_valid) {
        app->commands = g_ptr_array_new_with_free_func(quick_command_free);
        char *message = g_strdup_printf("Quick Commands configuration error: %s. Fix the file before editing commands.", error->message);
        status(app, message); g_free(message); g_clear_error(&error);
    }
    refresh_commands(app);
    app->layout = leaf_new(app);
    gtk_box_append(GTK_BOX(app->workspace), app->layout->widget);
    on_pane_selected(app->layout->pane, NULL, app);
    gtk_window_present(GTK_WINDOW(app->window));
    pane_focus(app->layout->pane);
}
int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.example.DevTerm", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int result = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return result;
}
