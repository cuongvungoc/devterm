#include "quick-commands-ui.h"
#include "split-layout.h"
#include "text-export.h"
#include "sessions-ui.h"
#include "draft.h"
#include "confirm-dialog.h"
#include "ui-style.h"
#include "devterm-version.h"
#include "runtime.h"
#include <gtk/gtk.h>
#include <string.h>
#include <sys/prctl.h>

typedef struct App App;
typedef struct TerminalTab TerminalTab;
struct TerminalTab {
    App *app;
    guint id;
    SplitLayout *layout;
    Draft *draft;
    GtkWidget *page;
    char *title, *message;
    GtkWidget *close_dialog;
};
struct App {
    GtkWidget *window, *workspace, *notebook, *quick_bar, *status;
    GtkWidget *draft_button, *counts, *command_add, *command_manage;
    GtkCssProvider *style; /* Borrowed from GtkApplication; shared by windows. */
    GCancellable *export_cancel;
    GtkWidget *window_close_dialog;
    gboolean close_confirmed;
    GPtrArray *tabs;
    TerminalTab *active_tab;
    guint next_tab_id, focus_idle;
    gboolean shutting_down;
    QuickCommandsUI *quick_commands;
    SessionManager *sessions;
};

static void on_new(GtkButton *button, gpointer data);
static void request_close_tab(TerminalTab *tab);
static void on_draft_toggle(GtkToggleButton *button, gpointer data);
static void sync_workspace(App *app) {
    gboolean active = app->active_tab != NULL;
    const char *actions[] = {"split-right", "split-down", "find-in-pane", "close-pane", "close-tab", "copy-all", NULL};
    for (int i = 0; actions[i]; i++)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(app->window), actions[i])), active);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(app->window), "export-text")), active && !app->export_cancel);
    if (app->counts) {
        guint panes = active ? split_layout_pane_count(app->active_tab->layout) : 0;
        char *text = g_strdup_printf("%u tab%s  ·  %u pane%s", app->tabs->len, app->tabs->len == 1 ? "" : "s", panes, panes == 1 ? "" : "s");
        gtk_label_set_text(GTK_LABEL(app->counts), text); g_free(text);
    }
}

static void sync_draft_button(App *app) {
    g_signal_handlers_block_by_func(app->draft_button, on_draft_toggle, app);
    gtk_widget_set_sensitive(app->draft_button, app->active_tab != NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->draft_button), app->active_tab && draft_is_visible(app->active_tab->draft));
    g_signal_handlers_unblock_by_func(app->draft_button, on_draft_toggle, app);
    sync_workspace(app);
}

static void status(App *app, const char *message) {
    gtk_label_set_text(GTK_LABEL(app->status), message);
    gtk_widget_set_tooltip_text(app->status, message);
}
static gboolean focus_current(gpointer data) {
    App *app = data;
    app->focus_idle = 0;
    if (!app->shutting_down && app->active_tab) {
        TerminalTab *tab = app->active_tab;
        gboolean editor = draft_wants_focus(tab->draft);
        draft_track_focus(tab->draft, FALSE);
        split_layout_focus(tab->layout);
        if (editor) draft_focus(tab->draft);
        draft_track_focus(tab->draft, TRUE);
    }
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
    GtkWidget *dialog = g_steal_pointer(&tab->close_dialog);
    if (dialog) gtk_window_destroy(GTK_WINDOW(dialog));
    g_clear_pointer(&tab->draft, draft_free);
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
        draft_track_focus(tab->draft, FALSE);
    }
    app->active_tab = g_object_get_data(G_OBJECT(page), "terminal-tab");
    if (app->active_tab) status(app, app->active_tab->message);
    sync_draft_button(app);
    schedule_focus(app);
}
static void sync_current_tab(App *app) {
    int page_number = gtk_notebook_get_current_page(GTK_NOTEBOOK(app->notebook));
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(app->notebook), page_number);
    app->active_tab = page ? g_object_get_data(G_OBJECT(page), "terminal-tab") : NULL;
    gtk_widget_set_visible(app->notebook, page != NULL);
    gtk_stack_set_visible_child_name(GTK_STACK(app->workspace), page ? "tabs" : "empty");
    status(app, app->active_tab ? app->active_tab->message : "No terminals open. Use New Terminal to create a tab.");
    sync_draft_button(app);
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
/* Resolve both window and tab at response time. A strong parent reference
 * here would keep a destroyed window alive while its modal child waits. */
typedef struct { GWeakRef window; guint tab_id; } CloseRequest;
static CloseRequest *close_request_new(GtkWidget *window, guint tab_id) {
    CloseRequest *request = g_new0(CloseRequest, 1);
    g_weak_ref_init(&request->window, window); request->tab_id = tab_id; return request;
}
static void close_request_free(gpointer data) {
    CloseRequest *request = data; g_weak_ref_clear(&request->window); g_free(request);
}
static void on_close_response(gboolean accepted, gpointer data) {
    CloseRequest *request = data;
    GtkWidget *window = g_weak_ref_get(&request->window);
    App *app = window ? g_object_get_data(G_OBJECT(window), "app") : NULL;
    if (app && !app->shutting_down) {
        TerminalTab *tab = find_tab(app, request->tab_id);
        if (tab) {
            tab->close_dialog = NULL;
            if (accepted) close_tab_now(tab);
            else schedule_focus(app);
        }
    }
    g_clear_object(&window);
}
static void request_close_tab(TerminalTab *tab) {
    if (!tab || tab->app->shutting_down || tab->close_dialog || tab->app->window_close_dialog) return;
    guint live = split_layout_live_sessions(tab->layout);
    gboolean draft = draft_has_text(tab->draft);
    if (live == 0 && !draft) { close_tab_now(tab); return; }
    char *title = g_strdup_printf("Close %s?", tab->title);
    GString *detail = g_string_new(NULL);
    if (live) g_string_append_printf(detail, "This will end all %u terminal session(s) in this tab, including any running programs.", live);
    if (draft) g_string_append_printf(detail, "%sThis tab has draft text (possibly hidden). Closing the tab discards it. Copying text does not save the draft.", live ? "\n\n" : "");
    CloseRequest *request = close_request_new(tab->app->window, tab->id);
    tab->close_dialog = confirm_dialog_show(GTK_WINDOW(tab->app->window), title, detail->str,
        "Close Tab", on_close_response, request, close_request_free);
    g_free(title); g_string_free(detail, TRUE);
}
static void on_tab_close(GtkButton *button, gpointer data) {
    (void)button;
    request_close_tab(data);
}
static void on_draft_visibility(gboolean visible, gpointer data) {
    TerminalTab *tab = data; App *app = tab->app;
    if (app->shutting_down || app->active_tab != tab) return;
    sync_draft_button(app);
    if (!visible) schedule_focus(app);
}
static void on_draft_toggle(GtkToggleButton *button, gpointer data) {
    App *app = data;
    if (app->active_tab && !app->shutting_down)
        draft_set_visible(app->active_tab->draft, gtk_toggle_button_get_active(button));
}
static void open_tab(App *app, const char *directory, const SessionProfile *profile, const char *password) {
    if (app->shutting_down) return;
    TerminalTab *tab = g_new0(TerminalTab, 1);
    tab->app = app;
    tab->id = ++app->next_tab_id;
    tab->title = profile ? g_strdup(profile->name) : g_strdup_printf("Terminal %u", tab->id);
    tab->message = g_strdup(profile ? "Session starting. Authentication status appears in the pane." : "Terminal ready");
    tab->layout = profile ? split_layout_new_session(profile, password, on_tab_status, tab) : split_layout_new(directory, on_tab_status, tab);
    tab->draft = draft_new(split_layout_widget(tab->layout), tab->title, on_draft_visibility, tab);
    tab->page = draft_widget(tab->draft);
    gtk_widget_add_css_class(tab->page, "tab-page");
    g_object_set_data(G_OBJECT(tab->page), "terminal-tab", tab);
    g_ptr_array_add(app->tabs, tab);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(header), ui_icon(profile ? "network-server-symbolic" : "utilities-terminal-symbolic"));
    GtkWidget *tab_label = gtk_label_new(tab->title);
    gtk_label_set_ellipsize(GTK_LABEL(tab_label), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(tab_label), 22);
    gtk_label_set_width_chars(GTK_LABEL(tab_label), 12);
    gtk_widget_set_tooltip_text(tab_label, tab->title);
    gtk_box_append(GTK_BOX(header), tab_label);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_focus_on_click(close, FALSE);
    gtk_widget_add_css_class(close, "flat");
    gtk_widget_add_css_class(close, "tab-close");
    gtk_widget_set_tooltip_text(close, "Close tab");
    gtk_accessible_update_property(GTK_ACCESSIBLE(close), GTK_ACCESSIBLE_PROPERTY_LABEL, "Close tab", -1);
    g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close), tab);
    gtk_box_append(GTK_BOX(header), close);
    int page = gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), tab->page, header);
    gtk_widget_set_visible(app->notebook, TRUE);
    gtk_stack_set_visible_child_name(GTK_STACK(app->workspace), "tabs");
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), page);
    sync_current_tab(app);
}
static void on_new(GtkButton *button, gpointer data) {
    (void)button; App *app = data;
    TerminalPane *active = app->active_tab ? split_layout_active(app->active_tab->layout) : NULL;
    char *directory = pane_dup_working_directory(active); open_tab(app, directory, NULL, NULL); g_free(directory);
}
static void on_session_connect(const SessionProfile *profile, const char *password, gpointer data) {
    open_tab(data, NULL, profile, password);
}
static void on_reconnect_ready(const SessionProfile *profile, const char *password, gpointer data) {
    pane_reconnect_ready(data, profile, password);
}
static void on_reconnect_request_done(gpointer data) {
    pane_finish_reconnect_request(data);
    pane_unref(data);
}
static void on_current_session(gboolean reconnect, gpointer data) {
    App *app = data; if (app->shutting_down) return;
    TerminalPane *pane = app->active_tab ? split_layout_active(app->active_tab->layout) : NULL;
    const SessionProfile *profile = pane_session_profile(pane);
    if (!profile) { status(app, "Select a connected-session pane first"); return; }
    if (reconnect) {
        GCancellable *cancel = pane_prepare_reconnect(pane);
        if (cancel) session_manager_connect_to(app->sessions, profile, cancel,
            on_reconnect_ready, pane_ref(pane), on_reconnect_request_done);
    } else {
        pane_disconnect(pane);
        status(app, "Disconnect requested; pane output is retained");
    }
}
static void on_sessions(GtkButton *button, gpointer data) { (void)button; App *app = data; session_manager_show(app->sessions); }
static void on_split_right(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->active_tab) split_layout_split(app->active_tab->layout, GTK_ORIENTATION_HORIZONTAL);
    else status(app, "Create a terminal tab first");
    sync_workspace(app);
}
static void on_split_down(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->active_tab) split_layout_split(app->active_tab->layout, GTK_ORIENTATION_VERTICAL);
    else status(app, "Create a terminal tab first");
    sync_workspace(app);
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
    sync_workspace(app);
}
static void action_new_tab(GSimpleAction *action, GVariant *parameter, gpointer data) {
    (void)action; (void)parameter; on_new(NULL, data);
}
static void action_close_tab(GSimpleAction *action, GVariant *parameter, gpointer data) {
    (void)action; (void)parameter;
    App *app = data;
    request_close_tab(app->active_tab);
}
static void action_find_in_pane(GSimpleAction *action, GVariant *parameter, gpointer data) {
    (void)action; (void)parameter;
    App *app = data;
    TerminalPane *pane = app->active_tab ? split_layout_active(app->active_tab->layout) : NULL;
    if (pane) pane_search_open(pane);
    else status(app, "Select a terminal pane to search");
}
static void on_copy_all(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    TerminalPane *pane = app->active_tab ? split_layout_active(app->active_tab->layout) : NULL;
    GError *error = NULL;
    if (pane_copy_all_text(pane, &error)) status(app, "All retained pane text copied to clipboard");
    else { status(app, error->message); g_clear_error(&error); }
    schedule_focus(app);
}
/* A request owns its snapshot rather than a pane/tab pointer. Hold the
 * application until cancellation/worker cleanup finishes even after closing
 * the last window, so a temporary export is never abandoned during shutdown. */
typedef struct { GtkWidget *window; GtkApplication *application; GBytes *text; } ExportRequest;
static void export_complete(ExportRequest *request, const char *message) {
    App *app = g_object_get_data(G_OBJECT(request->window), "app");
    if (app && !app->shutting_down) {
        g_clear_object(&app->export_cancel);
        sync_workspace(app);
        status(app, message);
        schedule_focus(app);
    }
    g_bytes_unref(request->text);
    g_object_unref(request->window);
    g_application_release(G_APPLICATION(request->application));
    g_object_unref(request->application);
    g_free(request);
}
static void on_export_written(GObject *source, GAsyncResult *result, gpointer data) {
    ExportRequest *request = data;
    GError *error = NULL;
    gboolean ok = text_export_finish(G_FILE(source), result, &error);
    char *path = g_file_get_parse_name(G_FILE(source));
    char *message = ok ? g_strdup_printf("Terminal text saved to %s", path) :
                        g_strdup_printf("Export failed: %s", error->message);
    export_complete(request, message);
    g_free(path); g_free(message); g_clear_error(&error);
}
static void on_export_destination(GObject *source, GAsyncResult *result, gpointer data) {
    ExportRequest *request = data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
    App *app = g_object_get_data(G_OBJECT(request->window), "app");
    if (!app || app->shutting_down || !file) {
        gboolean cancelled = !error || g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) ||
            g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) ||
            g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        char *message = cancelled ? g_strdup("Export cancelled") : g_strdup_printf("Cannot select export file: %s", error->message);
        export_complete(request, message);
        g_free(message); g_clear_error(&error); g_clear_object(&file);
        return;
    }
    status(app, "Saving terminal text…");
    text_export_async(file, request->text, app->export_cancel, on_export_written, request);
    g_object_unref(file);
}
static void on_export_text(GtkButton *button, gpointer data) {
    (void)button;
    App *app = data;
    if (app->shutting_down || app->export_cancel) return;
    TerminalPane *pane = app->active_tab ? split_layout_active(app->active_tab->layout) : NULL;
    GError *error = NULL;
    GBytes *text = pane_snapshot_text(pane, &error);
    if (!text) { status(app, error->message); g_clear_error(&error); return; }
    ExportRequest *request = g_new0(ExportRequest, 1);
    request->text = text;
    request->window = g_object_ref(app->window);
    request->application = g_object_ref(gtk_window_get_application(GTK_WINDOW(app->window)));
    g_application_hold(G_APPLICATION(request->application));
    app->export_cancel = g_cancellable_new();
    sync_workspace(app);
    status(app, "Pane text captured. Choose where to save it.");
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export Pane Text");
    gtk_file_dialog_set_modal(dialog, TRUE);
    GDateTime *now = g_date_time_new_now_local();
    char *name = g_date_time_format(now, "devterm-%Y%m%d-%H%M%S.txt");
    gtk_file_dialog_set_initial_name(dialog, name);
    g_free(name); g_date_time_unref(now);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Text files (*.txt)");
    gtk_file_filter_add_pattern(filter, "*.txt");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dialog, filter);
    g_object_unref(filter); g_object_unref(filters);
    gtk_file_dialog_save(dialog, GTK_WINDOW(app->window), app->export_cancel, on_export_destination, request);
    g_object_unref(dialog);
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

static void on_quick_status(gpointer data, const char *message) { status(data, message); }
static void on_quick_command(const QuickCommand *item, gpointer data) {
    App *app = data;
    TerminalPane *active = app->active_tab ? split_layout_active(app->active_tab->layout) : NULL;
    if (!active) { status(app, "Select a terminal pane before sending a command"); return; }
    if (!pane_is_running(active)) {
        status(app, pane_is_reconnecting(active) ? "Wait for reconnect to finish before sending Quick Commands." :
            "Selected pane has no running session. Reconnect it or create a new terminal."); return;
    }
    pane_send(active, item->command, item->send_enter);
    status(app, item->send_enter ? "Command sent to selected pane" : "Command inserted in selected pane");
}
static void on_manage_response(GtkDialog *dialog, int response, gpointer data) {
    (void)response; (void)data; gtk_window_destroy(GTK_WINDOW(dialog));
}
static void show_shortcuts(App *app) {
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Keyboard Shortcuts", GTK_WINDOW(app->window), GTK_DIALOG_MODAL, "Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_widget_add_css_class(dialog, "devterm-dialog"); gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 16); gtk_grid_set_column_spacing(GTK_GRID(grid), 32);
    gtk_widget_set_margin_start(grid, 24); gtk_widget_set_margin_end(grid, 24); gtk_widget_set_margin_top(grid, 24); gtk_widget_set_margin_bottom(grid, 24);
    const char *labels[] = {"New terminal tab", "Close tab", "Next tab", "Previous tab", "Find in selected pane", "Copy terminal selection", "Paste into terminal", "Undo in Draft", "Redo in Draft"};
    const char *keys[] = {"<Control><Shift>t", "<Control><Shift>w", "<Control>Page_Down", "<Control>Page_Up", "<Control><Shift>f", "<Control><Shift>c", "<Control><Shift>v", "<Control>z", "<Control><Shift>z"};
    for (guint i = 0; i < G_N_ELEMENTS(labels); i++) {
        GtkWidget *label = gtk_label_new(labels[i]); gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), label, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), gtk_shortcut_label_new(keys[i]), 1, i, 1, 1);
    }
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), grid);
    g_signal_connect(dialog, "response", G_CALLBACK(on_manage_response), NULL); gtk_window_present(GTK_WINDOW(dialog));
}
static void action_workspace(GSimpleAction *action, GVariant *parameter, gpointer data) {
    (void)parameter; App *app = data; const char *name = g_action_get_name(G_ACTION(action));
    if (g_str_equal(name, "split-right")) on_split_right(NULL, app);
    else if (g_str_equal(name, "split-down")) on_split_down(NULL, app);
    else if (g_str_equal(name, "close-pane")) on_close_pane(NULL, app);
    else if (g_str_equal(name, "copy-all")) on_copy_all(NULL, app);
    else if (g_str_equal(name, "export-text")) on_export_text(NULL, app);
    else if (g_str_equal(name, "sessions")) on_sessions(NULL, app);
    else if (g_str_equal(name, "shortcuts")) show_shortcuts(app);
}
static void action_appearance(GSimpleAction *action, GVariant *parameter, gpointer data) {
    (void)parameter; App *app = data; GVariant *state = g_action_get_state(G_ACTION(action));
    gboolean dark = !g_variant_get_boolean(state); g_variant_unref(state);
    GtkApplication *application = gtk_window_get_application(GTK_WINDOW(app->window));
    for (GList *item = gtk_application_get_windows(application); item; item = item->next) {
        if (!G_IS_ACTION_MAP(item->data)) continue;
        GAction *appearance = g_action_map_lookup_action(G_ACTION_MAP(item->data), "dark-appearance");
        if (appearance) g_simple_action_set_state(G_SIMPLE_ACTION(appearance), g_variant_new_boolean(dark));
    }
    ui_style_apply(app->style, dark);
}

static void on_window_destroy(GtkWidget *window, gpointer data) {
    (void)window;
    App *app = data;
    if (app->shutting_down) return;
    app->shutting_down = TRUE;
    g_clear_pointer(&app->quick_commands, quick_commands_ui_free);
    GtkWidget *dialog = g_steal_pointer(&app->window_close_dialog);
    if (dialog) gtk_window_destroy(GTK_WINDOW(dialog));
    g_clear_pointer(&app->sessions, session_manager_free);
    if (app->export_cancel) g_cancellable_cancel(app->export_cancel);
    app->active_tab = NULL;
    if (app->focus_idle) { g_source_remove(app->focus_idle); app->focus_idle = 0; }
    g_signal_handlers_disconnect_by_data(app->notebook, app);
    for (guint i = 0; i < app->tabs->len; i++) tab_dispose(g_ptr_array_index(app->tabs, i));
}
static void on_window_close_response(gboolean accepted, gpointer data) {
    CloseRequest *request = data;
    GtkWidget *window = g_weak_ref_get(&request->window);
    App *app = window ? g_object_get_data(G_OBJECT(window), "app") : NULL;
    if (app) app->window_close_dialog = NULL;
    if (app && !app->shutting_down) {
        if (accepted) {
            app->close_confirmed = TRUE;
            gtk_window_close(GTK_WINDOW(window));
        } else schedule_focus(app);
    }
    g_clear_object(&window);
}
static gboolean on_window_close_request(GtkWindow *window, gpointer data) {
    App *app = data;
    if (!app->shutting_down && !app->close_confirmed) {
        if (app->window_close_dialog) return TRUE;
        guint drafts = 0, live = 0; GString *names = g_string_new(NULL);
        for (guint i = 0; i < app->tabs->len; i++) {
            TerminalTab *tab = app->tabs->pdata[i];
            /* A tab close alert already owns the modal decision. */
            if (tab->close_dialog) { g_string_free(names, TRUE); return TRUE; }
            live += split_layout_live_sessions(tab->layout);
            if (draft_has_text(tab->draft)) {
                g_string_append_printf(names, "%s%s", drafts ? ", " : "", tab->title); drafts++;
            }
        }
        if (drafts) {
            char *detail = g_strdup_printf("%u tab(s) contain draft text, including hidden drafts:\n%s\n\nClosing DevTerm discards these drafts and ends %u running terminal session(s). Drafts are kept in memory only; copying text does not save them.", drafts, names->str, live);
            app->window_close_dialog = confirm_dialog_show(window, "Close DevTerm and discard drafts?", detail,
                "Close DevTerm", on_window_close_response, close_request_new(GTK_WIDGET(window), 0), close_request_free);
            g_free(detail); g_string_free(names, TRUE); return TRUE;
        }
        g_string_free(names, TRUE);
    }
    /* Cancel asynchronous dialogs before the default handler destroys the
     * window; their callback holds a reference until cancellation completes. */
    gtk_window_set_focus(window, NULL);
    on_window_destroy(GTK_WIDGET(window), data);
    return FALSE;
}
static void app_free(gpointer data) {
    App *app = data;
    if (app->focus_idle) g_source_remove(app->focus_idle);
    g_ptr_array_unref(app->tabs);
    g_clear_pointer(&app->quick_commands, quick_commands_ui_free);
    g_clear_object(&app->notebook);
    g_clear_object(&app->export_cancel);
    g_clear_pointer(&app->sessions, session_manager_free);
    g_free(app);
}
static void activate(GtkApplication *application, gpointer data) {
    (void)data;
    App *app = g_new0(App, 1);
    app->tabs = g_ptr_array_new_with_free_func(tab_free);
    app->window = gtk_application_window_new(application);
    gtk_window_set_icon_name(GTK_WINDOW(app->window), "com.example.DevTerm");
    g_object_set_data_full(G_OBJECT(app->window), "app", app, app_free);
    g_signal_connect(app->window, "destroy", G_CALLBACK(on_window_destroy), app);
    g_signal_connect(app->window, "close-request", G_CALLBACK(on_window_close_request), app);
    const GActionEntry actions[] = {
        {.name = "new-tab", .activate = action_new_tab},
        {.name = "close-tab", .activate = action_close_tab},
        {.name = "find-in-pane", .activate = action_find_in_pane},
        {.name = "next-tab", .activate = action_cycle_tab},
        {.name = "previous-tab", .activate = action_cycle_tab},
        {.name = "split-right", .activate = action_workspace},
        {.name = "split-down", .activate = action_workspace},
        {.name = "close-pane", .activate = action_workspace},
        {.name = "copy-all", .activate = action_workspace},
        {.name = "export-text", .activate = action_workspace},
        {.name = "sessions", .activate = action_workspace},
        {.name = "shortcuts", .activate = action_workspace},
        {.name = "dark-appearance", .activate = action_appearance, .state = "true"}
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app->window), actions, G_N_ELEMENTS(actions), app);
    gtk_application_set_accels_for_action(application, "win.new-tab", (const char *[]){"<Control><Shift>t", NULL});
    gtk_application_set_accels_for_action(application, "win.close-tab", (const char *[]){"<Control><Shift>w", NULL});
    gtk_application_set_accels_for_action(application, "win.find-in-pane", (const char *[]){"<Control><Shift>f", NULL});
    gtk_application_set_accels_for_action(application, "win.next-tab", (const char *[]){"<Control>Page_Down", NULL});
    gtk_application_set_accels_for_action(application, "win.previous-tab", (const char *[]){"<Control>Page_Up", NULL});
    gtk_window_set_title(GTK_WINDOW(app->window), "DevTerm");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1200, 800);
    gtk_widget_set_size_request(app->window, 760, 480);
    gtk_widget_add_css_class(app->window, "devterm-window");
    app->style = g_object_get_data(G_OBJECT(application), "ui-style");
    if (!app->style) {
        app->style = ui_style_install(gtk_widget_get_display(app->window), TRUE);
        g_object_set_data_full(G_OBJECT(application), "ui-style", app->style, (GDestroyNotify)ui_style_uninstall);
    }
    gboolean dark;
    g_object_get(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", &dark, NULL);
    g_simple_action_set_state(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(app->window), "dark-appearance")), g_variant_new_boolean(dark));
    GtkWidget *header = gtk_header_bar_new();
    GtkWidget *brand = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8), *brand_icon = ui_icon("utilities-terminal-symbolic");
    gtk_widget_add_css_class(brand_icon, "brand-icon"); gtk_box_append(GTK_BOX(brand), brand_icon);
    GtkWidget *brand_label = gtk_label_new("DevTerm"); gtk_widget_add_css_class(brand_label, "brand"); gtk_box_append(GTK_BOX(brand), brand_label);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), brand);
    GtkWidget *new_button = ui_button("list-add-symbolic", "New Terminal", "New terminal tab (Ctrl+Shift+T)");
    gtk_widget_add_css_class(new_button, "primary"); gtk_actionable_set_action_name(GTK_ACTIONABLE(new_button), "win.new-tab");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), new_button);
    GtkWidget *sessions = ui_button("network-server-symbolic", "Sessions", "Manage SSH, Telnet and Serial profiles");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(sessions), "win.sessions"); gtk_header_bar_pack_start(GTK_HEADER_BAR(header), sessions);
    GMenu *app_menu = g_menu_new(); g_menu_append(app_menu, "Dark appearance", "win.dark-appearance");
    g_menu_append(app_menu, "Keyboard Shortcuts", "win.shortcuts");
    GtkWidget *main_menu = gtk_menu_button_new(); gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(main_menu), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(main_menu), G_MENU_MODEL(app_menu)); g_object_unref(app_menu);
    gtk_widget_set_tooltip_text(main_menu, "Appearance and keyboard shortcuts");
    gtk_accessible_update_property(GTK_ACCESSIBLE(main_menu), GTK_ACCESSIBLE_PROPERTY_LABEL, "Application menu", -1);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), main_menu); gtk_window_set_titlebar(GTK_WINDOW(app->window), header);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(app->window), outer);
    GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(tools, "workspace-tools"); gtk_box_append(GTK_BOX(outer), tools);
    GtkWidget *split_right = ui_button("view-dual-symbolic", "Split Right", "Split the selected pane to the right");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(split_right), "win.split-right"); gtk_box_append(GTK_BOX(tools), split_right);
    GtkWidget *split_down = ui_button("view-dual-symbolic", "Split Down", "Split the selected pane below");
    gtk_widget_add_css_class(gtk_widget_get_first_child(gtk_button_get_child(GTK_BUTTON(split_down))), "split-down-icon");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(split_down), "win.split-down"); gtk_box_append(GTK_BOX(tools), split_down);
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); gtk_widget_set_hexpand(spacer, TRUE); gtk_box_append(GTK_BOX(tools), spacer);
    GtkWidget *search = ui_button("edit-find-symbolic", "Search", "Find in selected pane (Ctrl+Shift+F)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(search), "win.find-in-pane");
    gtk_box_append(GTK_BOX(tools), search);
    app->draft_button = gtk_toggle_button_new_with_label("Draft");
    gtk_widget_set_tooltip_text(app->draft_button, "Show or hide this tab's draft (text stays in memory)");
    gtk_widget_set_focus_on_click(app->draft_button, FALSE);
    g_signal_connect(app->draft_button, "toggled", G_CALLBACK(on_draft_toggle), app);
    gtk_box_append(GTK_BOX(tools), app->draft_button);
    GMenu *pane_menu = g_menu_new(), *text_menu = g_menu_new(), *close_menu = g_menu_new();
    g_menu_append(text_menu, "Copy All Text", "win.copy-all"); g_menu_append(text_menu, "Export Text…", "win.export-text");
    g_menu_append(close_menu, "Close Pane", "win.close-pane"); g_menu_append(close_menu, "Close Tab", "win.close-tab");
    g_menu_append_section(pane_menu, "Selected pane", G_MENU_MODEL(text_menu)); g_menu_append_section(pane_menu, NULL, G_MENU_MODEL(close_menu));
    GtkWidget *pane_more = gtk_menu_button_new(); gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(pane_more), "view-more-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(pane_more), G_MENU_MODEL(pane_menu));
    gtk_widget_set_tooltip_text(pane_more, "Pane actions: copy, export and close");
    gtk_accessible_update_property(GTK_ACCESSIBLE(pane_more), GTK_ACCESSIBLE_PROPERTY_LABEL, "Pane actions", -1);
    gtk_box_append(GTK_BOX(tools), pane_more); g_object_unref(pane_menu); g_object_unref(text_menu); g_object_unref(close_menu);
    app->workspace = gtk_stack_new();
    gtk_stack_set_hhomogeneous(GTK_STACK(app->workspace), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(app->workspace), FALSE);
    gtk_widget_set_vexpand(app->workspace, TRUE);
    gtk_box_append(GTK_BOX(outer), app->workspace);
    app->notebook = gtk_notebook_new();
    /* GtkWindow may dispose its child hierarchy before emitting destroy.
     * Keep the notebook valid until tab callbacks have been disconnected. */
    g_object_ref_sink(app->notebook);
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(app->notebook), TRUE);
    gtk_stack_add_named(GTK_STACK(app->workspace), app->notebook, "tabs");
    g_signal_connect(app->notebook, "switch-page", G_CALLBACK(on_switch_page), app);
    GtkWidget *empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(empty, "empty-state");
    gtk_widget_set_halign(empty, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(empty, GTK_ALIGN_CENTER);
    GtkWidget *empty_icon = ui_icon("utilities-terminal-symbolic"); gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 48); gtk_widget_add_css_class(empty_icon, "brand-icon");
    gtk_box_append(GTK_BOX(empty), empty_icon);
    GtkWidget *empty_title = gtk_label_new("A fresh workspace"); gtk_widget_add_css_class(empty_title, "empty-title"); gtk_box_append(GTK_BOX(empty), empty_title);
    GtkWidget *empty_hint = gtk_label_new("Start a local terminal or connect to a saved session."); gtk_widget_add_css_class(empty_hint, "muted"); gtk_box_append(GTK_BOX(empty), empty_hint);
    GtkWidget *empty_new = ui_button("list-add-symbolic", "New Terminal", "New terminal tab (Ctrl+Shift+T)"); gtk_widget_add_css_class(empty_new, "primary");
    g_signal_connect(empty_new, "clicked", G_CALLBACK(on_new), app);
    gtk_box_append(GTK_BOX(empty), empty_new);
    gtk_stack_add_named(GTK_STACK(app->workspace), empty, "empty");
    GtkWidget *dock = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_widget_add_css_class(dock, "quick-dock");
    GtkWidget *dock_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *dock_title = gtk_label_new("Quick Commands"); gtk_widget_add_css_class(dock_title, "section-title");
    gtk_widget_set_halign(dock_title, GTK_ALIGN_START); gtk_widget_set_hexpand(dock_title, TRUE); gtk_box_append(GTK_BOX(dock_header), dock_title);
    app->command_add = ui_button("list-add-symbolic", "Add Command", "Create a custom command button");
    gtk_box_append(GTK_BOX(dock_header), app->command_add);
    app->command_manage = ui_button("emblem-system-symbolic", "Manage", "Edit, reorder or remove Quick Commands");
    gtk_box_append(GTK_BOX(dock_header), app->command_manage);
    gtk_box_append(GTK_BOX(dock), dock_header);
    GtkWidget *quick_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(quick_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(quick_scroll), FALSE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(quick_scroll), 112);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(quick_scroll), 36);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(quick_scroll), TRUE);
    app->quick_bar = gtk_flow_box_new(); gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(app->quick_bar), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(app->quick_bar), 1); gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(app->quick_bar), 100);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(app->quick_bar), 6); gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(app->quick_bar), 6);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(quick_scroll), app->quick_bar);
    gtk_box_append(GTK_BOX(dock), quick_scroll); gtk_box_append(GTK_BOX(outer), dock);
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16); gtk_widget_add_css_class(footer, "status-bar");
    app->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->status), 0); gtk_label_set_ellipsize(GTK_LABEL(app->status), PANGO_ELLIPSIZE_END); gtk_widget_set_hexpand(app->status, TRUE);
    gtk_box_append(GTK_BOX(footer), app->status); app->counts = gtk_label_new(""); gtk_box_append(GTK_BOX(footer), app->counts);
    gtk_widget_set_tooltip_text(app->counts, "Open tabs · Terminal panes in the current tab");
    gtk_box_append(GTK_BOX(outer), footer);
    app->quick_commands = quick_commands_ui_new(GTK_WINDOW(app->window), GTK_FLOW_BOX(app->quick_bar),
        GTK_SCROLLED_WINDOW(quick_scroll), app->command_add, app->command_manage,
        on_quick_command, on_quick_status, app);
    char *config_error = g_strdup(quick_commands_ui_load_error(app->quick_commands));
    app->sessions = session_manager_new(GTK_WINDOW(app->window), on_session_connect, on_current_session, app);
    on_new(NULL, app);
    if (config_error) { status(app, config_error); g_free(config_error); }
    gtk_window_present(GTK_WINDOW(app->window));
    schedule_focus(app);
}
int main(int argc, char **argv) {
    if (argc == 2 && g_str_equal(argv[1], "--version")) {
        g_print("DevTerm %s\n", DEVTERM_VERSION);
        return 0;
    }
    /* Password entry buffers must not end up in core dumps. Exec resets this
     * flag, so local shells keep their usual debugging/core-dump behavior. */
    prctl(PR_SET_DUMPABLE, 0);
#if DEVTERM_BUNDLED_RUNTIME
    if (!runtime_init_schemas()) return 1;
#endif
    GtkApplication *app = gtk_application_new("com.example.DevTerm", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int result = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return result;
}
