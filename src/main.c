#include "commands.h"
#include "split-layout.h"
#include <gtk/gtk.h>
#include <string.h>

typedef struct App App;
typedef struct TerminalTab TerminalTab;
struct TerminalTab {
    App *app;
    guint id;
    SplitLayout *layout;
    GtkWidget *page;
    char *title, *message;
    GCancellable *close_cancel;
};
struct App {
    GtkWidget *window, *workspace, *notebook, *quick_bar, *status, *manage_dialog, *manage_rows;
    GPtrArray *tabs;
    TerminalTab *active_tab;
    guint next_tab_id, focus_idle;
    gboolean shutting_down;
    GPtrArray *commands;
    char *config_path;
    gboolean config_valid;
};

static void refresh_commands(App *app);
static void refresh_manage(App *app);
static void show_editor(App *app, const char *id);
static void on_new(GtkButton *button, gpointer data);
static void request_close_tab(TerminalTab *tab);

static void status(App *app, const char *message) {
    gtk_label_set_text(GTK_LABEL(app->status), message);
    gtk_widget_set_tooltip_text(app->status, message);
}
static gboolean focus_current(gpointer data) {
    App *app = data;
    app->focus_idle = 0;
    if (!app->shutting_down && app->active_tab) split_layout_focus(app->active_tab->layout);
    return G_SOURCE_REMOVE;
}
static void schedule_focus(App *app) {
    if (app->focus_idle) g_source_remove(app->focus_idle);
    app->focus_idle = g_idle_add(focus_current, app);
}
static void on_tab_status(SplitLayout *layout, const char *message, gpointer data) {
    (void)layout;
    TerminalTab *tab = data;
    g_free(tab->message);
    tab->message = g_strdup(message);
    if (!tab->app->shutting_down && tab->app->active_tab == tab) status(tab->app, message);
}
static void tab_dispose(TerminalTab *tab) {
    if (tab->close_cancel) g_cancellable_cancel(tab->close_cancel);
    g_clear_object(&tab->close_cancel);
    split_layout_free(tab->layout);
    tab->layout = NULL;
}
static void tab_free(gpointer data) {
    TerminalTab *tab = data;
    tab_dispose(tab);
    g_free(tab->title); g_free(tab->message); g_free(tab);
}
static TerminalTab *find_tab(App *app, guint id) {
    for (guint i = 0; i < app->tabs->len; i++) {
        TerminalTab *tab = g_ptr_array_index(app->tabs, i);
        if (tab->id == id) return tab;
    }
    return NULL;
}
static void on_switch_page(GtkNotebook *notebook, GtkWidget *page, guint number, gpointer data) {
    (void)notebook; (void)number;
    App *app = data;
    if (app->shutting_down) return;
    for (guint i = 0; i < app->tabs->len; i++) {
        TerminalTab *tab = g_ptr_array_index(app->tabs, i);
        split_layout_suspend_focus(tab->layout);
    }
    app->active_tab = g_object_get_data(G_OBJECT(page), "terminal-tab");
    if (app->active_tab) status(app, app->active_tab->message);
    schedule_focus(app);
}
static void sync_current_tab(App *app) {
    int page_number = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), page_number);
    app->active_tab = page ? g_object_get_data(G_OBJECT(page), "terminal-tab") : NULL;
    gtk_widget_set_visible(app->notebook, page != NULL);
    gtk_stack_set_visible_child_name(GTK_STACK(app->workspace), page ? "tabs" : "empty");
    status(app, app->active_tab ? app->active_tab->message : "No terminals open. Use New Terminal to create a tab.");
    schedule_focus(app);
}
static void close_tab_now(TerminalTab *tab) {
    App *app = tab->app;
    int page = gtk_notebook_page_num(GTK_NOTEBOOK(app->notebook), tab->page);
    if (page < 0) return;
    if (app->active_tab == tab) app->active_tab = NULL;
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(app->notebook)) == 1) {
        gtk_stack_set_visible_child_name(GTK_STACK(app->workspace), "empty");
        gtk_widget_set_visible(app->notebook, FALSE);
    }
    g_object_set_data(G_OBJECT(tab->page), "terminal-tab", NULL);
    gtk_notebook_remove_page(GTK_NOTEBOOK(app->notebook), page);
    g_ptr_array_remove(app->tabs, tab);
    sync_current_tab(app);
}
/* Keep the window alive for the asynchronous response; identify the tab by ID
 * so closing the window or removing a tab never leaves a dangling tab pointer. */
typedef struct { GtkWidget *window; guint tab_id; } CloseRequest;
static void on_close_response(GObject *source, GAsyncResult *result, gpointer data) {
    CloseRequest *request = data;
    GError *error = NULL;
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
    App *app = g_object_get_data(G_OBJECT(request->window), "app");
    if (app && !app->shutting_down) {
        TerminalTab *tab = find_tab(app, request->tab_id);
        if (tab) {
            g_clear_object(&tab->close_cancel);
            if (!error && choice == 1) close_tab_now(tab);
            else schedule_focus(app);
        }
    }
    g_clear_error(&error);
    g_object_unref(request->window);
    g_free(request);
}
static void request_close_tab(TerminalTab *tab) {
    if (!tab || tab->app->shutting_down || tab->close_cancel) return;
    guint live = split_layout_live_sessions(tab->layout);
    if (live == 0) { close_tab_now(tab); return; }
    GtkAlertDialog *dialog = gtk_alert_dialog_new("Close %s?", tab->title);
    char *detail = g_strdup_printf("This will end all %u terminal session(s) in this tab, including any running programs.", live);
    gtk_alert_dialog_set_detail(dialog, detail);
    g_free(detail);
    const char *buttons[] = {"Cancel", "Close Tab", NULL};
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_default_button(dialog, 0);
    gtk_alert_dialog_set_cancel_button(dialog, 0);
    gtk_alert_dialog_set_modal(dialog, TRUE);
    tab->close_cancel = g_cancellable_new();
    CloseRequest *request = g_new0(CloseRequest, 1);
    request->window = g_object_ref(tab->app->window);
    request->tab_id = tab->id;
    gtk_alert_dialog_choose(dialog, GTK_WINDOW(tab->app->window), tab->close_cancel, on_close_response, request);
    g_object_unref(dialog);
}
static void on_tab_close(GtkButton *button, gpointer data) {
    (void)button;
    request_close_tab(data);
}
static void on_new(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->shutting_down) return;
    TerminalPane *active = app->active_tab ? split_layout_active(app->active_tab->layout) : NULL;
    char *directory = pane_dup_working_directory(active);
    TerminalTab *tab = g_new0(TerminalTab, 1);
    tab->app = app;
    tab->id = ++app->next_tab_id;
    tab->title = g_strdup_printf("Terminal %u", tab->id);
    tab->message = g_strdup("Terminal ready");
    tab->layout = split_layout_new(directory, on_tab_status, tab);
    g_free(directory);
    tab->page = split_layout_widget(tab->layout);
    g_object_set_data(G_OBJECT(tab->page), "terminal-tab", tab);
    g_ptr_array_add(app->tabs, tab);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(header), gtk_label_new(tab->title));
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_focus_on_click(close, FALSE);
    gtk_widget_add_css_class(close, "flat");
    gtk_widget_set_tooltip_text(close, "Close tab");
    g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close), tab);
    gtk_box_append(GTK_BOX(header), close);
    int page = gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), tab->page, header);
    gtk_widget_set_visible(app->notebook, TRUE);
    gtk_stack_set_visible_child_name(GTK_STACK(app->workspace), "tabs");
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), page);
    sync_current_tab(app);
}
static void on_split_right(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->active_tab) split_layout_split(app->active_tab->layout, GTK_ORIENTATION_HORIZONTAL);
    else status(app, "Create a terminal tab first");
}
static void on_split_down(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->active_tab) split_layout_split(app->active_tab->layout, GTK_ORIENTATION_VERTICAL);
    else status(app, "Create a terminal tab first");
}
static void on_close_pane(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (!app->active_tab) { status(app, "No terminal pane to close"); return; }
    if (split_layout_pane_count(app->active_tab->layout) == 1) request_close_tab(app->active_tab);
    else {
        split_layout_close_active(app->active_tab->layout);
        status(app, "Pane closed");
    }
}
static void action_new_tab(GSimpleAction *action, GVariant *parameter, gpointer data) {
    (void)action; (void)parameter; on_new(NULL, data);
}
static void action_close_tab(GSimpleAction *action, GVariant *parameter, gpointer data) {
    (void)action; (void)parameter;
    App *app = data;
    request_close_tab(app->active_tab);
}
static void action_cycle_tab(GSimpleAction *action, GVariant *parameter, gpointer data) {
    (void)parameter;
    App *app = data;
    int total = gtk_notebook_get_n_pages(GTK_NOTEBOOK(app->notebook));
    if (total < 1) return;
    int current = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    int step = g_str_equal(g_action_get_name(G_ACTION(action)), "next-tab") ? 1 : -1;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), (current + step + total) % total);
    schedule_focus(app);
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
    TerminalPane *active = app->active_tab ? split_layout_active(app->active_tab->layout) : NULL;
    if (!active) { status(app, "Select a terminal pane before sending a command"); return; }
    if (!pane_is_running(active)) { status(app, "Selected pane has no running shell. Close it and create a new terminal."); return; }
    QuickCommand *item = g_ptr_array_index(app->commands, index);
    pane_send(active, item->command, item->send_enter);
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

static void on_window_destroy(GtkWidget *window, gpointer data) {
    (void)window;
    App *app = data;
    if (app->shutting_down) return;
    app->shutting_down = TRUE;
    app->active_tab = NULL;
    if (app->focus_idle) { g_source_remove(app->focus_idle); app->focus_idle = 0; }
    g_signal_handlers_disconnect_by_data(app->notebook, app);
    for (guint i = 0; i < app->tabs->len; i++) tab_dispose(g_ptr_array_index(app->tabs, i));
}
static gboolean on_window_close_request(GtkWindow *window, gpointer data) {
    /* Cancel asynchronous dialogs before the default handler destroys the
     * window; their callback holds a reference until cancellation completes. */
    on_window_destroy(GTK_WIDGET(window), data);
    return FALSE;
}
static void app_free(gpointer data) {
    App *app = data;
    if (app->focus_idle) g_source_remove(app->focus_idle);
    g_ptr_array_unref(app->tabs);
    g_ptr_array_unref(app->commands);
    g_free(app->config_path);
    g_free(app);
}
static void activate(GtkApplication *application, gpointer data) {
    (void)data;
    App *app = g_new0(App, 1);
    app->tabs = g_ptr_array_new_with_free_func(tab_free);
    app->window = gtk_application_window_new(application);
    g_object_set_data_full(G_OBJECT(app->window), "app", app, app_free);
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_window_destroy), app);
    g_signal_connect(app->window, "close-request", G_CALLBACK(on_window_close_request), app);
    const GActionEntry actions[] = {
        {.name = "new-tab", .activate = action_new_tab},
        {.name = "close-tab", .activate = action_close_tab},
        {.name = "next-tab", .activate = action_cycle_tab},
        {.name = "previous-tab", .activate = action_cycle_tab}
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app->window), actions, G_N_ELEMENTS(actions), app);
    gtk_application_set_accels_for_action(application, "win.new-tab", (const char *[]){"<Control><Shift>t", NULL});
    gtk_application_set_accels_for_action(application, "win.close-tab", (const char *[]){"<Control><Shift>w", NULL});
    gtk_application_set_accels_for_action(application, "win.next-tab", (const char *[]){"<Control>Page_Down", NULL});
    gtk_application_set_accels_for_action(application, "win.previous-tab", (const char *[]){"<Control>Page_Up", NULL});
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
    gtk_widget_set_tooltip_text(new_button, "New tab (Ctrl+Shift+T)");
    g_signal_connect(new_button, "clicked", G_CALLBACK(on_new), app); gtk_box_append(GTK_BOX(tools), new_button);
    GtkWidget *split_right = gtk_button_new_with_label("Split Right");
    g_signal_connect(split_right, "clicked", G_CALLBACK(on_split_right), app); gtk_box_append(GTK_BOX(tools), split_right);
    GtkWidget *split_down = gtk_button_new_with_label("Split Down");
    g_signal_connect(split_down, "clicked", G_CALLBACK(on_split_down), app); gtk_box_append(GTK_BOX(tools), split_down);
    GtkWidget *close = gtk_button_new_with_label("Close Pane");
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_pane), app); gtk_box_append(GTK_BOX(tools), close);
    app->workspace = gtk_stack_new();
    gtk_stack_set_hhomogeneous(GTK_STACK(app->workspace), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(app->workspace), FALSE);
    gtk_widget_set_vexpand(app->workspace, TRUE);
    gtk_box_append(GTK_BOX(outer), app->workspace);
    app->notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(app->notebook), TRUE);
    gtk_stack_add_named(GTK_STACK(app->workspace), app->notebook, "tabs");
    g_signal_connect(app->notebook, "switch-page", G_CALLBACK(on_switch_page), app);
    GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(empty), gtk_label_new("No terminal tabs are open"));
    GtkWidget *empty_new = gtk_button_new_with_label("New Terminal");
    g_signal_connect(empty_new, "clicked", G_CALLBACK(on_new), app);
    gtk_box_append(GTK_BOX(empty), empty_new);
    gtk_stack_add_named(GTK_STACK(app->workspace), empty, "empty");
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
    char *config_error = NULL;
    app->commands = commands_load(app->config_path, &error);
    app->config_valid = app->commands != NULL;
    if (!app->config_valid) {
        app->commands = g_ptr_array_new_with_free_func(quick_command_free);
        config_error = g_strdup_printf("Quick Commands configuration error: %s. Fix the file before editing commands.", error->message);
        g_clear_error(&error);
    }
    refresh_commands(app);
    on_new(NULL, app);
    if (config_error) { status(app, config_error); g_free(config_error); }
    gtk_window_present(GTK_WINDOW(app->window));
    schedule_focus(app);
}
int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new("com.example.DevTerm", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int result = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return result;
}
