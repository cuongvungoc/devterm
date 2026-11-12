#include "quick-commands-ui.h"
#include "confirm-dialog.h"
#include <string.h>

enum { EDIT, DELETE, MOVE_UP, MOVE_DOWN };
struct QuickCommandsUI {
    GtkWindow *window; /* Parent outlives this controller. */
    GtkWidget *quick_bar, *quick_scroll, *command_add, *command_manage, *empty;
    GtkWidget *manage_dialog, *manage_rows, *manage_scroll, *editor, *confirmation, *menu, *menu_button;
    GPtrArray *commands;
    GHashTable *buttons, *rows; /* Widgets are owned by their containers. */
    char *config_path, *load_error;
    gboolean config_valid, closing;
    QuickCommandsRun run; QuickCommandsStatus status; gpointer data;
    GtkDropTarget *drop_target;
    GtkDragSource *drag_source;
    GObject *drag_payload; /* Local object only: never export text to a terminal drop target. */
    char *drag_id;
    gboolean dragging, drop_inside;
    double drop_x, drop_y;
    GtkWidget *drop_button;
    gboolean drop_after;
    guint drag_tick, manage_tick, manage_frames;
    gint64 last_frame;
    double saved_scroll;
    char *focus_id; int focus_action;
};
static void refresh_commands(QuickCommandsUI *ui);
static void refresh_manage(QuickCommandsUI *ui);
static void show_editor(QuickCommandsUI *ui, const char *id);
static void request_delete(QuickCommandsUI *ui, const char *id);
static void install_button_controls(QuickCommandsUI *ui, GtkWidget *button);
static void clear_drop(QuickCommandsUI *ui);
static void status(QuickCommandsUI *ui, const char *message) {
    if (!ui->closing && ui->status) ui->status(ui->data, message);
}
static gint command_index(QuickCommandsUI *ui, const char *id) {
    for (guint i = 0; i < ui->commands->len; i++)
        if (g_strcmp0(((QuickCommand *)ui->commands->pdata[i])->id, id) == 0) return i;
    return -1;
}
static gboolean save_commands(QuickCommandsUI *ui) {
    GError *error = NULL;
    if (!commands_save(ui->config_path, ui->commands, &error)) {
        char *message = g_strdup_printf("Cannot save Quick Commands: %s. Changes were not applied.", error->message);
        status(ui, message); g_free(message); g_clear_error(&error); return FALSE;
    }
    status(ui, "Quick Commands saved"); return TRUE;
}
static void close_menu(QuickCommandsUI *ui) {
    GtkWidget *menu = g_steal_pointer(&ui->menu);
    if (ui->menu_button) { gtk_widget_set_has_tooltip(ui->menu_button, TRUE); ui->menu_button = NULL; }
    if (menu) {
        g_signal_handlers_disconnect_by_data(menu, ui);
        gtk_popover_popdown(GTK_POPOVER(menu)); gtk_widget_unparent(menu);
    }
}
static void menu_closed(GtkPopover *popover, gpointer data) {
    (void)popover; QuickCommandsUI *ui = data;
    if (ui->menu_button) { gtk_widget_set_has_tooltip(ui->menu_button, TRUE); ui->menu_button = NULL; }
}
static void on_command(GtkButton *button, gpointer data) {
    QuickCommandsUI *ui = data;
    if (ui->closing || ui->dragging || g_object_get_data(G_OBJECT(button), "suppress-click")) return;
    gint index = command_index(ui, g_object_get_data(G_OBJECT(button), "command-id"));
    if (index >= 0 && ui->run) ui->run(ui->commands->pdata[index], ui->data);
}
static void on_add(GtkButton *button, gpointer data) { (void)button; show_editor(data, NULL); }
static gint button_order(GtkFlowBoxChild *a, GtkFlowBoxChild *b, gpointer data) {
    QuickCommandsUI *ui = data;
    GtkWidget *first = gtk_flow_box_child_get_child(a), *second = gtk_flow_box_child_get_child(b);
    return command_index(ui, g_object_get_data(G_OBJECT(first), "command-id")) -
           command_index(ui, g_object_get_data(G_OBJECT(second), "command-id"));
}
static void refresh_commands(QuickCommandsUI *ui) {
    close_menu(ui);
    gtk_widget_set_sensitive(ui->command_add, ui->config_valid);
    gtk_widget_set_sensitive(ui->command_manage, ui->config_valid);
    GHashTableIter iter; gpointer id, widget;
    g_hash_table_iter_init(&iter, ui->buttons);
    while (g_hash_table_iter_next(&iter, &id, &widget)) {
        if (command_index(ui, id) >= 0) continue;
        GtkWidget *child = gtk_widget_get_parent(widget);
        g_hash_table_iter_remove(&iter); gtk_flow_box_remove(GTK_FLOW_BOX(ui->quick_bar), child);
    }
    if (ui->commands->len && ui->empty) {
        gtk_flow_box_remove(GTK_FLOW_BOX(ui->quick_bar), gtk_widget_get_parent(ui->empty)); ui->empty = NULL;
    }
    if (!ui->commands->len && !ui->empty) {
        ui->empty = gtk_label_new(ui->config_valid ? "Your shortcuts, one click away. Use Add Command to get started." :
            "Configuration error. Fix commands.json to edit your shortcuts.");
        gtk_label_set_wrap(GTK_LABEL(ui->empty), TRUE); gtk_label_set_xalign(GTK_LABEL(ui->empty), 0);
        gtk_widget_add_css_class(ui->empty, "muted"); gtk_flow_box_append(GTK_FLOW_BOX(ui->quick_bar), ui->empty);
    }
    for (guint i = 0; i < ui->commands->len; i++) {
        QuickCommand *item = ui->commands->pdata[i];
        GtkWidget *button = g_hash_table_lookup(ui->buttons, item->id);
        if (!button) {
            button = gtk_button_new_with_label(item->name);
            g_object_set_data_full(G_OBJECT(button), "command-id", g_strdup(item->id), g_free);
            gtk_widget_add_css_class(button, "quick-command"); gtk_widget_set_focus_on_click(button, FALSE);
            g_signal_connect(button, "clicked", G_CALLBACK(on_command), ui);
            install_button_controls(ui, button);
            gtk_flow_box_append(GTK_FLOW_BOX(ui->quick_bar), button);
            g_hash_table_insert(ui->buttons, g_strdup(item->id), button);
        }
        gtk_button_set_label(GTK_BUTTON(button), item->name);
        GtkWidget *label = gtk_button_get_child(GTK_BUTTON(button));
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(label), 24);
        gtk_label_set_width_chars(GTK_LABEL(label), MIN(g_utf8_strlen(item->name, -1), 24));
        char *tip = g_strdup_printf("%s\n%s\n%s\nRight-click to edit/delete. Drag to reorder.", item->name, item->command,
            item->send_enter ? "Send text + Enter to the selected pane" : "Insert text without Enter into the selected pane");
        gtk_widget_set_tooltip_text(button, tip); g_free(tip);
    }
    gtk_flow_box_invalidate_sort(GTK_FLOW_BOX(ui->quick_bar));
}

/* Keep row widgets and restore scroll only after their new allocation. Focus
 * follows the acted-on ID, not its old index. */
static gboolean restore_manage(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)widget; (void)clock; QuickCommandsUI *ui = data;
    if (++ui->manage_frames < 2) return G_SOURCE_CONTINUE;
    ui->manage_tick = 0;
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ui->manage_scroll));
    gtk_adjustment_set_value(adjustment, ui->saved_scroll);
    GtkWidget *row = ui->focus_id ? g_hash_table_lookup(ui->rows, ui->focus_id) : NULL;
    if (row) {
        GtkWidget *buttons[4]; GtkWidget *child = gtk_widget_get_next_sibling(gtk_widget_get_first_child(row));
        for (guint i = 0; i < 4; i++) { buttons[i] = child; child = gtk_widget_get_next_sibling(child); }
        /* Row order is Up, Down, Edit, Delete. */
        guint target = ui->focus_action == MOVE_UP ? 0 : ui->focus_action == MOVE_DOWN ? 1 : 2;
        if (!gtk_widget_get_sensitive(buttons[target])) target = 2;
        gtk_widget_grab_focus(buttons[target]);
        graphene_rect_t bounds;
        if (gtk_widget_compute_bounds(row, ui->manage_scroll, &bounds)) {
            double value = gtk_adjustment_get_value(adjustment), height = gtk_widget_get_height(ui->manage_scroll);
            if (bounds.origin.y < 0) value += bounds.origin.y;
            else if (bounds.origin.y + bounds.size.height > height) value += bounds.origin.y + bounds.size.height - height;
            gtk_adjustment_set_value(adjustment, value);
        }
    }
    g_clear_pointer(&ui->focus_id, g_free); return G_SOURCE_REMOVE;
}
static void preserve_manage(QuickCommandsUI *ui, const char *id, int action) {
    if (!ui->manage_rows) return;
    if (ui->manage_tick) gtk_widget_remove_tick_callback(ui->manage_rows, ui->manage_tick);
    ui->saved_scroll = gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ui->manage_scroll)));
    g_free(ui->focus_id); ui->focus_id = g_strdup(id); ui->focus_action = action; ui->manage_frames = 0;
    ui->manage_tick = gtk_widget_add_tick_callback(ui->manage_rows, restore_manage, ui, NULL);
}
static gboolean move_command(QuickCommandsUI *ui, const char *id, gint target, int action) {
    gint index = command_index(ui, id);
    if (!ui->config_valid || index < 0 || target < 0 || target >= (gint)ui->commands->len) return FALSE;
    if (index == target) return TRUE;
    gpointer item = g_ptr_array_steal_index(ui->commands, index); g_ptr_array_insert(ui->commands, target, item);
    if (!save_commands(ui)) {
        g_ptr_array_steal_index(ui->commands, target); g_ptr_array_insert(ui->commands, index, item); return FALSE;
    }
    preserve_manage(ui, id, action); refresh_commands(ui); refresh_manage(ui); return TRUE;
}

typedef struct { QuickCommandsUI *ui; char *id; GtkWidget *name, *command, *enter, *error; } Editor;
static void editor_free(gpointer data) { Editor *e = data; g_free(e->id); g_free(e); }
static void editor_destroyed(GtkWidget *dialog, gpointer data) {
    QuickCommandsUI *ui = data; if (ui->editor == dialog) ui->editor = NULL;
}
static gboolean nonblank(const char *s) {
    for (; *s; s = g_utf8_next_char(s)) if (!g_unichar_isspace(g_utf8_get_char(s))) return TRUE;
    return FALSE;
}
static void on_editor_response(GtkDialog *dialog, int response, gpointer data) {
    Editor *e = data; QuickCommandsUI *ui = e->ui;
    if (response != GTK_RESPONSE_ACCEPT || ui->closing) { gtk_window_destroy(GTK_WINDOW(dialog)); return; }
    const char *name = gtk_editable_get_text(GTK_EDITABLE(e->name)), *command = gtk_editable_get_text(GTK_EDITABLE(e->command));
    if (!nonblank(name) || !nonblank(command)) { gtk_label_set_text(GTK_LABEL(e->error), "Name and command are required."); return; }
    gint index = e->id ? command_index(ui, e->id) : -1;
    if (e->id && index < 0) { gtk_window_destroy(GTK_WINDOW(dialog)); return; }
    char *id = e->id ? g_strdup(e->id) : g_uuid_string_random();
    QuickCommand *replacement = quick_command_new(id, name, command, gtk_check_button_get_active(GTK_CHECK_BUTTON(e->enter)));
    QuickCommand *old = index >= 0 ? ui->commands->pdata[index] : NULL;
    if (old) ui->commands->pdata[index] = replacement; else g_ptr_array_add(ui->commands, replacement);
    if (!save_commands(ui)) {
        if (old) { ui->commands->pdata[index] = old; quick_command_free(replacement); }
        else g_ptr_array_remove_index(ui->commands, ui->commands->len - 1);
        gtk_label_set_text(GTK_LABEL(e->error), "Cannot save the configuration. Your changes have not been applied; correct the problem and retry.");
        g_free(id); return;
    }
    quick_command_free(old);
    preserve_manage(ui, e->id, EDIT); refresh_commands(ui); refresh_manage(ui); g_free(id);
    gtk_window_destroy(GTK_WINDOW(dialog));
}
static void show_editor(QuickCommandsUI *ui, const char *id) {
    if (!ui->config_valid || ui->closing || (id && command_index(ui, id) < 0)) return;
    if (ui->editor) { gtk_window_present(GTK_WINDOW(ui->editor)); return; }
    GtkWidget *parent = ui->manage_dialog ? ui->manage_dialog : GTK_WIDGET(ui->window);
    GtkWidget *dialog = gtk_dialog_new_with_buttons(id ? "Edit Command" : "Add Command", GTK_WINDOW(parent), GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, NULL);
    ui->editor = dialog; g_signal_connect(dialog, "destroy", G_CALLBACK(editor_destroyed), ui);
    gtk_widget_add_css_class(dialog, "devterm-dialog");
    gtk_widget_add_css_class(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT), "suggested-action");
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    Editor *e = g_new0(Editor, 1); e->ui = ui; e->id = g_strdup(id);
    g_object_set_data_full(G_OBJECT(dialog), "editor", e, editor_free);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_box_set_spacing(GTK_BOX(box), 8);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
    GtkWidget *label = gtk_label_new("Name"); gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_box_append(GTK_BOX(box), label);
    e->name = gtk_entry_new(); gtk_box_append(GTK_BOX(box), e->name);
    gtk_accessible_update_property(GTK_ACCESSIBLE(e->name), GTK_ACCESSIBLE_PROPERTY_LABEL, "Name", -1);
    label = gtk_label_new("Command"); gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_box_append(GTK_BOX(box), label);
    e->command = gtk_entry_new(); gtk_widget_set_size_request(e->command, 420, -1); gtk_box_append(GTK_BOX(box), e->command);
    gtk_accessible_update_property(GTK_ACCESSIBLE(e->command), GTK_ACCESSIBLE_PROPERTY_LABEL, "Command", -1);
    e->enter = gtk_check_button_new_with_label("Send Enter after command");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(e->enter), TRUE); gtk_box_append(GTK_BOX(box), e->enter);
    label = gtk_label_new("Input goes to the foreground program in the selected terminal (including SSH, vim or top).");
    gtk_label_set_wrap(GTK_LABEL(label), TRUE); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_add_css_class(label, "muted"); gtk_box_append(GTK_BOX(box), label);
    e->error = gtk_label_new(""); gtk_widget_add_css_class(e->error, "error"); gtk_label_set_wrap(GTK_LABEL(e->error), TRUE);
    gtk_box_append(GTK_BOX(box), e->error);
    gint index = id ? command_index(ui, id) : -1;
    if (index >= 0) {
        QuickCommand *item = ui->commands->pdata[index];
        gtk_editable_set_text(GTK_EDITABLE(e->name), item->name); gtk_editable_set_text(GTK_EDITABLE(e->command), item->command);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(e->enter), item->send_enter);
    }
    g_signal_connect(dialog, "response", G_CALLBACK(on_editor_response), e); gtk_window_present(GTK_WINDOW(dialog));
}

typedef struct { QuickCommandsUI *ui; char *id; int action; } RowAction;
static void row_action_free(gpointer data) { RowAction *a = data; g_free(a->id); g_free(a); }
static void delete_response(gboolean accepted, gpointer data) {
    RowAction *a = data; QuickCommandsUI *ui = a->ui; ui->confirmation = NULL;
    if (ui->closing) return;
    gint index = command_index(ui, a->id);
    if (!accepted || index < 0) { preserve_manage(ui, a->id, EDIT); return; }
    QuickCommand *item = g_ptr_array_steal_index(ui->commands, index);
    if (!save_commands(ui)) { g_ptr_array_insert(ui->commands, index, item); preserve_manage(ui, a->id, EDIT); return; }
    const char *next_id = ui->commands->len ? ((QuickCommand *)ui->commands->pdata[MIN((guint)index, ui->commands->len - 1)])->id : NULL;
    preserve_manage(ui, next_id, EDIT); refresh_commands(ui); refresh_manage(ui); quick_command_free(item);
}
static void request_delete(QuickCommandsUI *ui, const char *id) {
    gint index = command_index(ui, id); if (!ui->config_valid || index < 0 || ui->closing) return;
    if (ui->confirmation) { gtk_window_present(GTK_WINDOW(ui->confirmation)); return; }
    RowAction *a = g_new0(RowAction, 1); a->ui = ui; a->id = g_strdup(id);
    char *detail = g_strdup_printf("Delete Quick Command “%s”?\n\nThis removes the button and its saved command.", ((QuickCommand *)ui->commands->pdata[index])->name);
    ui->confirmation = confirm_dialog_show(ui->manage_dialog ? GTK_WINDOW(ui->manage_dialog) : ui->window,
        "Delete Quick Command?", detail, "Delete", delete_response, a, row_action_free); g_free(detail);
}
static void on_row_action(GtkButton *button, gpointer data) {
    (void)button; RowAction *a = data;
    if (a->ui->closing) return;
    if (a->action == EDIT) show_editor(a->ui, a->id);
    else if (a->action == DELETE) request_delete(a->ui, a->id);
    else move_command(a->ui, a->id, command_index(a->ui, a->id) + (a->action == MOVE_UP ? -1 : 1), a->action);
}
static GtkWidget *row_button(QuickCommandsUI *ui, const char *id, int action) {
    const char *name = action == MOVE_UP ? "Move Up" : action == MOVE_DOWN ? "Move Down" : action == EDIT ? "Edit" : "Delete";
    GtkWidget *button = action >= MOVE_UP ? gtk_button_new_from_icon_name(action == MOVE_UP ? "go-up-symbolic" : "go-down-symbolic") : gtk_button_new_with_label(name);
    gtk_widget_set_tooltip_text(button, name);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    if (action == DELETE) gtk_widget_add_css_class(button, "danger");
    RowAction *a = g_new0(RowAction, 1); a->ui = ui; a->id = g_strdup(id); a->action = action;
    g_object_set_data_full(G_OBJECT(button), "row-action", a, row_action_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_row_action), a); return button;
}
static void refresh_manage(QuickCommandsUI *ui) {
    if (!ui->manage_rows) return;
    GHashTableIter iter; gpointer id, widget;
    g_hash_table_iter_init(&iter, ui->rows);
    while (g_hash_table_iter_next(&iter, &id, &widget)) {
        if (command_index(ui, id) >= 0) continue;
        g_hash_table_iter_remove(&iter); gtk_box_remove(GTK_BOX(ui->manage_rows), widget);
    }
    GtkWidget *previous = NULL;
    for (guint i = 0; i < ui->commands->len; i++) {
        QuickCommand *item = ui->commands->pdata[i]; GtkWidget *row = g_hash_table_lookup(ui->rows, item->id);
        if (!row) {
            row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_widget_add_css_class(row, "command-row");
            GtkWidget *label = gtk_label_new(item->name); gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(label), 32); gtk_widget_set_hexpand(label, TRUE); gtk_widget_set_halign(label, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(row), label);
            gtk_box_append(GTK_BOX(row), row_button(ui, item->id, MOVE_UP)); gtk_box_append(GTK_BOX(row), row_button(ui, item->id, MOVE_DOWN));
            gtk_box_append(GTK_BOX(row), row_button(ui, item->id, EDIT)); gtk_box_append(GTK_BOX(row), row_button(ui, item->id, DELETE));
            gtk_box_append(GTK_BOX(ui->manage_rows), row); g_hash_table_insert(ui->rows, g_strdup(item->id), row);
        }
        GtkWidget *label = gtk_widget_get_first_child(row); gtk_label_set_text(GTK_LABEL(label), item->name);
        gtk_widget_set_tooltip_text(label, item->command);
        GtkWidget *up = gtk_widget_get_next_sibling(label), *down = gtk_widget_get_next_sibling(up);
        gtk_widget_set_sensitive(up, i > 0); gtk_widget_set_sensitive(down, i + 1 < ui->commands->len);
        gtk_box_reorder_child_after(GTK_BOX(ui->manage_rows), row, previous); previous = row;
    }
}
static void on_manage_destroy(GtkWidget *widget, gpointer data) {
    QuickCommandsUI *ui = data;
    if (ui->manage_dialog == widget) {
        if (ui->manage_tick) { gtk_widget_remove_tick_callback(ui->manage_rows, ui->manage_tick); ui->manage_tick = 0; }
        g_object_unref(ui->manage_rows);
        ui->manage_dialog = ui->manage_rows = ui->manage_scroll = NULL;
        g_hash_table_remove_all(ui->rows); g_clear_pointer(&ui->focus_id, g_free);
    }
}
static void on_manage_response(GtkDialog *dialog, int response, gpointer data) {
    (void)response; (void)data; gtk_window_destroy(GTK_WINDOW(dialog));
}
static void on_manage(GtkButton *button, gpointer data) {
    (void)button; QuickCommandsUI *ui = data;
    if (ui->closing || !ui->config_valid) return;
    if (ui->manage_dialog) { gtk_window_present(GTK_WINDOW(ui->manage_dialog)); return; }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Manage Quick Commands", ui->window, GTK_DIALOG_MODAL, "Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_widget_add_css_class(dialog, "devterm-dialog"); gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    ui->manage_dialog = dialog; gtk_window_set_default_size(GTK_WINDOW(dialog), 650, 350);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_box_set_spacing(GTK_BOX(area), 8);
    gtk_widget_set_margin_start(area, 12); gtk_widget_set_margin_end(area, 12); gtk_widget_set_margin_top(area, 12); gtk_widget_set_margin_bottom(area, 12);
    GtkWidget *add = gtk_button_new_with_label("Add Command"); g_signal_connect(add, "clicked", G_CALLBACK(on_add), ui); gtk_box_append(GTK_BOX(area), add);
    ui->manage_scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(ui->manage_scroll, TRUE);
    ui->manage_rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); g_object_ref_sink(ui->manage_rows);
    gtk_widget_add_css_class(ui->manage_rows, "settings-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ui->manage_scroll), ui->manage_rows); gtk_box_append(GTK_BOX(area), ui->manage_scroll);
    refresh_manage(ui); g_signal_connect(dialog, "response", G_CALLBACK(on_manage_response), ui);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_manage_destroy), ui); gtk_window_present(GTK_WINDOW(dialog));
}

static void context_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
    QuickCommandsUI *ui = data; char *id = g_variant_dup_string(parameter, NULL); close_menu(ui);
    if (g_str_equal(g_action_get_name(G_ACTION(action)), "edit")) show_editor(ui, id); else request_delete(ui, id);
    g_free(id);
}
static void show_context(QuickCommandsUI *ui, GtkWidget *button, double x, double y) {
    if (ui->closing || ui->dragging || !ui->config_valid) return;
    const char *id = g_object_get_data(G_OBJECT(button), "command-id"); if (command_index(ui, id) < 0) return;
    close_menu(ui); GMenu *model = g_menu_new();
    GMenuItem *item = g_menu_item_new("Edit", NULL); g_menu_item_set_action_and_target(item, "quick.edit", "s", id);
    g_menu_append_item(model, item); g_object_unref(item);
    item = g_menu_item_new("Delete", NULL); g_menu_item_set_action_and_target(item, "quick.delete", "s", id);
    g_menu_append_item(model, item); g_object_unref(item);
    ui->menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(model)); g_object_unref(model);
    ui->menu_button = button; gtk_widget_set_has_tooltip(button, FALSE);
    g_signal_connect(ui->menu, "closed", G_CALLBACK(menu_closed), ui);
    gtk_widget_set_parent(ui->menu, ui->quick_bar);
    graphene_point_t point = GRAPHENE_POINT_INIT(x, y), anchor;
    if (!gtk_widget_compute_point(button, ui->quick_bar, &point, &anchor)) anchor = GRAPHENE_POINT_INIT(0, 0);
    GdkRectangle rect = {(int)anchor.x, (int)anchor.y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(ui->menu), &rect); gtk_popover_popup(GTK_POPOVER(ui->menu));
}
static void right_pressed(GtkGestureClick *gesture, int n, double x, double y, gpointer data) {
    (void)n; gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    show_context(data, gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)), x, y);
}
static void left_pressed(GtkGestureClick *gesture, int n, double x, double y, gpointer data) {
    (void)n; (void)x; (void)y; QuickCommandsUI *ui = data;
    GtkWidget *button = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    if (!ui->dragging) g_object_set_data(G_OBJECT(button), "suppress-click", NULL);
    /* Keep Escape/other keys away from the PTY even while the native drag
     * grab is starting/ending. A normal click returns focus via run(). */
    gtk_widget_grab_focus(button);
}
static gboolean context_key(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state, gpointer data) {
    (void)code; QuickCommandsUI *ui = data;
    GtkWidget *button = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(keys));
    if (key == GDK_KEY_Escape) {
        g_object_set_data(G_OBJECT(button), "suppress-click", GINT_TO_POINTER(TRUE));
        if (ui->drag_source) gtk_drag_source_drag_cancel(ui->drag_source);
        return TRUE;
    }
    if (key == GDK_KEY_Menu || (key == GDK_KEY_F10 && (state & GDK_SHIFT_MASK))) {
        show_context(data, button, gtk_widget_get_width(button) / 2., gtk_widget_get_height(button)); return TRUE;
    }
    if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter || key == GDK_KEY_space)
        g_object_set_data(G_OBJECT(button), "suppress-click", NULL);
    return FALSE;
}
static void clear_drop(QuickCommandsUI *ui) {
    if (ui->drop_button) {
        gtk_widget_remove_css_class(ui->drop_button, "drop-before"); gtk_widget_remove_css_class(ui->drop_button, "drop-after");
        ui->drop_button = NULL;
    }
}
static gint drop_position(QuickCommandsUI *ui, double x, double y) {
    clear_drop(ui);
    if (x < 0 || y < 0 || x > gtk_widget_get_width(ui->quick_scroll) || y > gtk_widget_get_height(ui->quick_scroll)) return -1;
    double best = G_MAXDOUBLE; gint result = -1;
    for (guint i = 0; i < ui->commands->len; i++) {
        QuickCommand *item = ui->commands->pdata[i]; GtkWidget *button = g_hash_table_lookup(ui->buttons, item->id);
        graphene_rect_t bounds;
        if (!button || !gtk_widget_compute_bounds(button, ui->quick_scroll, &bounds)) continue;
        double left = bounds.origin.x, top = bounds.origin.y, right = left + bounds.size.width, bottom = top + bounds.size.height;
        double dx = x < left ? left - x : x > right ? x - right : 0;
        double dy = y < top ? top - y : y > bottom ? y - bottom : 0;
        double distance = dx * dx + dy * dy;
        if (distance >= best) continue;
        best = distance; ui->drop_button = button;
        ui->drop_after = x >= left + bounds.size.width / 2.;
        if (gtk_widget_get_direction(ui->quick_bar) == GTK_TEXT_DIR_RTL) ui->drop_after = !ui->drop_after;
        if (y > bottom) ui->drop_after = TRUE;
        else if (y < top) ui->drop_after = FALSE;
        result = i + (ui->drop_after ? 1 : 0);
    }
    if (ui->drop_button) gtk_widget_add_css_class(ui->drop_button, ui->drop_after ? "drop-after" : "drop-before");
    return result;
}
static gboolean drag_scroll(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)widget; QuickCommandsUI *ui = data;
    gint64 now = gdk_frame_clock_get_frame_time(clock);
    double seconds = ui->last_frame ? MIN((now - ui->last_frame) / 1000000., .05) : 0;
    ui->last_frame = now;
    if (!ui->drop_inside) return G_SOURCE_CONTINUE;
    double height = gtk_widget_get_height(ui->quick_scroll), edge = MIN(24., height / 3.);
    double speed = ui->drop_y < edge ? -320 * (1 - ui->drop_y / edge) :
        ui->drop_y > height - edge ? 320 * (1 - (height - ui->drop_y) / edge) : 0;
    if (speed) {
        GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ui->quick_scroll));
        gtk_adjustment_set_value(adjustment, gtk_adjustment_get_value(adjustment) + speed * seconds);
        drop_position(ui, ui->drop_x, ui->drop_y);
    }
    return G_SOURCE_CONTINUE;
}
static GdkContentProvider *drag_prepare(GtkDragSource *source, double x, double y, gpointer data) {
    QuickCommandsUI *ui = data;
    if (ui->closing || !ui->config_valid) return NULL;
    GtkWidget *button = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
    gtk_widget_grab_focus(button);
    const char *id = g_object_get_data(G_OBJECT(button), "command-id"); if (command_index(ui, id) < 0) return NULL;
    close_menu(ui); g_clear_object(&ui->drag_payload); g_free(ui->drag_id); ui->drag_id = g_strdup(id);
    ui->drag_payload = g_object_new(G_TYPE_OBJECT, NULL);
    GdkPaintable *paintable = gtk_widget_paintable_new(button); gtk_drag_source_set_icon(source, paintable, (int)x, (int)y); g_object_unref(paintable);
    return gdk_content_provider_new_typed(G_TYPE_OBJECT, ui->drag_payload);
}
static void drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer data) {
    (void)drag; QuickCommandsUI *ui = data;
    ui->dragging = TRUE; ui->drag_source = g_object_ref(source);
    GtkWidget *button = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
    g_object_set_data(G_OBJECT(button), "suppress-click", GINT_TO_POINTER(TRUE)); gtk_widget_add_css_class(button, "drag-source");
    ui->last_frame = 0; ui->drag_tick = gtk_widget_add_tick_callback(ui->quick_scroll, drag_scroll, ui, NULL);
}
static void drag_end(GtkDragSource *source, GdkDrag *drag, gboolean delete_data, gpointer data) {
    (void)drag; (void)delete_data; QuickCommandsUI *ui = data;
    if (ui->drag_tick) { gtk_widget_remove_tick_callback(ui->quick_scroll, ui->drag_tick); ui->drag_tick = 0; }
    gtk_widget_remove_css_class(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source)), "drag-source");
    clear_drop(ui); ui->dragging = ui->drop_inside = FALSE;
    g_clear_object(&ui->drag_source); g_clear_object(&ui->drag_payload); g_clear_pointer(&ui->drag_id, g_free);
    /* suppress-click stays set until the next primary press (or keyboard
     * activation), including cancelled/outside drops and late releases. */
}
static gboolean drag_cancel(GtkDragSource *source, GdkDrag *drag, GdkDragCancelReason reason, gpointer data) {
    (void)source; (void)drag; (void)reason; (void)data; return TRUE;
}
static gboolean drop_accept(GtkDropTarget *target, GdkDrop *drop, gpointer data) {
    (void)target; QuickCommandsUI *ui = data;
    return ui->dragging && ui->drag_source && gdk_drop_get_drag(drop) == gtk_drag_source_get_drag(ui->drag_source);
}
static GdkDragAction drop_motion(GtkDropTarget *target, double x, double y, gpointer data) {
    (void)target; QuickCommandsUI *ui = data;
    if (!ui->dragging) return 0;
    ui->drop_inside = TRUE; ui->drop_x = x; ui->drop_y = y;
    return drop_position(ui, x, y) >= 0 ? GDK_ACTION_MOVE : 0;
}
static void drop_leave(GtkDropTarget *target, gpointer data) {
    (void)target; QuickCommandsUI *ui = data; ui->drop_inside = FALSE; clear_drop(ui);
}
static gboolean dropped(GtkDropTarget *target, const GValue *value, double x, double y, gpointer data) {
    (void)target; QuickCommandsUI *ui = data;
    if (!ui->dragging || !G_VALUE_HOLDS_OBJECT(value) || g_value_get_object(value) != ui->drag_payload) return FALSE;
    gint slot = drop_position(ui, x, y), index = command_index(ui, ui->drag_id);
    clear_drop(ui); ui->drop_inside = FALSE;
    if (slot < 0 || index < 0) return FALSE;
    if (index < slot) slot--;
    return move_command(ui, ui->drag_id, slot, EDIT);
}
static void install_button_controls(QuickCommandsUI *ui, GtkWidget *button) {
    GtkGesture *right = gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right), GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(right), GTK_PHASE_CAPTURE);
    g_signal_connect(right, "pressed", G_CALLBACK(right_pressed), ui); gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(right));
    GtkGesture *left = gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(left), GDK_BUTTON_PRIMARY);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(left), GTK_PHASE_CAPTURE);
    g_signal_connect(left, "pressed", G_CALLBACK(left_pressed), ui); gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(left));
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(context_key), ui); gtk_widget_add_controller(button, keys);
    GtkDragSource *source = gtk_drag_source_new(); gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(source), GTK_PHASE_CAPTURE);
    g_signal_connect(source, "prepare", G_CALLBACK(drag_prepare), ui); g_signal_connect(source, "drag-begin", G_CALLBACK(drag_begin), ui);
    g_signal_connect(source, "drag-end", G_CALLBACK(drag_end), ui); g_signal_connect(source, "drag-cancel", G_CALLBACK(drag_cancel), ui);
    gtk_widget_add_controller(button, GTK_EVENT_CONTROLLER(source));
}
QuickCommandsUI *quick_commands_ui_new(GtkWindow *parent, GtkFlowBox *bar, GtkScrolledWindow *scroll,
    GtkWidget *add, GtkWidget *manage, QuickCommandsRun run, QuickCommandsStatus report, gpointer data) {
    QuickCommandsUI *ui = g_new0(QuickCommandsUI, 1); ui->window = parent;
    ui->quick_bar = g_object_ref(GTK_WIDGET(bar)); ui->quick_scroll = g_object_ref(GTK_WIDGET(scroll));
    ui->command_add = g_object_ref(add); ui->command_manage = g_object_ref(manage);
    ui->run = run; ui->status = report; ui->data = data;
    ui->buttons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    ui->rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    ui->config_path = commands_config_path(); GError *error = NULL;
    ui->commands = commands_load(ui->config_path, &error); ui->config_valid = ui->commands != NULL;
    if (!ui->config_valid) {
        ui->commands = g_ptr_array_new_with_free_func(quick_command_free);
        ui->load_error = g_strdup_printf("Quick Commands configuration error: %s. Fix the file before editing commands.", error->message);
        g_clear_error(&error);
    }
    g_signal_connect(add, "clicked", G_CALLBACK(on_add), ui); g_signal_connect(manage, "clicked", G_CALLBACK(on_manage), ui);
    GSimpleActionGroup *group = g_simple_action_group_new();
    const GActionEntry actions[] = {{.name="edit", .activate=context_action, .parameter_type="s"}, {.name="delete", .activate=context_action, .parameter_type="s"}};
    g_action_map_add_action_entries(G_ACTION_MAP(group), actions, G_N_ELEMENTS(actions), ui);
    gtk_widget_insert_action_group(ui->quick_bar, "quick", G_ACTION_GROUP(group)); g_object_unref(group);
    gtk_flow_box_set_sort_func(bar, button_order, ui, NULL);
    ui->drop_target = gtk_drop_target_new(G_TYPE_OBJECT, GDK_ACTION_MOVE);
    g_signal_connect(ui->drop_target, "accept", G_CALLBACK(drop_accept), ui);
    g_signal_connect(ui->drop_target, "enter", G_CALLBACK(drop_motion), ui); g_signal_connect(ui->drop_target, "motion", G_CALLBACK(drop_motion), ui);
    g_signal_connect(ui->drop_target, "leave", G_CALLBACK(drop_leave), ui); g_signal_connect(ui->drop_target, "drop", G_CALLBACK(dropped), ui);
    gtk_widget_add_controller(ui->quick_scroll, GTK_EVENT_CONTROLLER(ui->drop_target));
    refresh_commands(ui); return ui;
}
const char *quick_commands_ui_load_error(QuickCommandsUI *ui) { return ui->load_error; }
void quick_commands_ui_free(QuickCommandsUI *ui) {
    if (!ui) return;
    ui->closing = TRUE;
    if (ui->editor) gtk_window_destroy(GTK_WINDOW(ui->editor));
    if (ui->confirmation) gtk_window_destroy(GTK_WINDOW(ui->confirmation));
    if (ui->manage_dialog) gtk_window_destroy(GTK_WINDOW(ui->manage_dialog));
    close_menu(ui);
    if (ui->drag_source) gtk_drag_source_drag_cancel(ui->drag_source);
    if (ui->drag_source) {
        /* A backend may finish the drag asynchronously after parent teardown. */
        g_signal_handlers_disconnect_by_data(ui->drag_source, ui);
        g_clear_object(&ui->drag_source);
    }
    if (ui->drag_tick) gtk_widget_remove_tick_callback(ui->quick_scroll, ui->drag_tick);
    clear_drop(ui); g_clear_object(&ui->drag_payload);
    gtk_widget_remove_controller(ui->quick_scroll, GTK_EVENT_CONTROLLER(ui->drop_target));
    g_signal_handlers_disconnect_by_data(ui->command_add, ui); g_signal_handlers_disconnect_by_data(ui->command_manage, ui);
    gtk_widget_insert_action_group(ui->quick_bar, "quick", NULL); gtk_flow_box_set_sort_func(GTK_FLOW_BOX(ui->quick_bar), NULL, NULL, NULL);
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(ui->quick_bar))) gtk_flow_box_remove(GTK_FLOW_BOX(ui->quick_bar), child);
    g_hash_table_unref(ui->buttons); g_hash_table_unref(ui->rows); g_ptr_array_unref(ui->commands);
    g_object_unref(ui->quick_bar); g_object_unref(ui->quick_scroll); g_object_unref(ui->command_add); g_object_unref(ui->command_manage);
    g_free(ui->config_path); g_free(ui->load_error); g_free(ui->drag_id); g_free(ui->focus_id); g_free(ui);
}
