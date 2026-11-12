#include "../src/quick-commands-ui.h"
#include <glib/gstdio.h>
#include <string.h>

static char *root, *path;
static GtkWidget *window, *bar, *scroll, *add, *manage;
static QuickCommandsUI *ui;
static guint runs;
static char *last_status;
static void iterate(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void settle(void) {
    gint64 end = g_get_monotonic_time() + 160 * G_TIME_SPAN_MILLISECOND;
    while (g_get_monotonic_time() < end) iterate();
}
static GtkWidget *top(const char *title) {
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
        GtkWidget *w = g_list_model_get_item(windows, i);
        gboolean found = gtk_widget_get_visible(w) && g_strcmp0(gtk_window_get_title(GTK_WINDOW(w)), title) == 0;
        g_object_unref(w); if (found) return w;
    }
    return NULL;
}
static GtkWidget *find(GtkWidget *w, GType type, const char *label) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(w, type) && (!label ||
        (GTK_IS_BUTTON(w) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(w)), label) == 0) ||
        (GTK_IS_LABEL(w) && g_strcmp0(gtk_label_get_text(GTK_LABEL(w)), label) == 0))) return w;
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *found = find(c, type, label); if (found) return found;
    }
    return NULL;
}
static GtkWidget *field(GtkWidget *dialog, const char *name) {
    GtkWidget *label = find(dialog, GTK_TYPE_LABEL, name); g_assert_nonnull(label); return gtk_widget_get_next_sibling(label);
}
static GtkWidget *quick_button(const char *id) {
    for (GtkWidget *c = gtk_widget_get_first_child(bar); c; c = gtk_widget_get_next_sibling(c)) {
        if (!GTK_IS_FLOW_BOX_CHILD(c)) continue;
        GtkWidget *button = gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(c));
        if (g_strcmp0(g_object_get_data(G_OBJECT(button), "command-id"), id) == 0) return button;
    }
    return NULL;
}
static void click(GtkWidget *w) { g_assert_nonnull(w); g_signal_emit_by_name(w, "clicked"); }
static void action(const char *name, const char *id) { g_assert_true(gtk_widget_activate_action(bar, name, "s", id)); }
static GPtrArray *load(void) { GError *error = NULL; GPtrArray *items = commands_load(path, &error); g_assert_no_error(error); return items; }
static void run(const QuickCommand *command, gpointer data) { (void)command; (void)data; runs++; }
static void report(gpointer data, const char *message) { (void)data; g_free(last_status); last_status = g_strdup(message); }
static void fixture(guint n) {
    GPtrArray *items = g_ptr_array_new_with_free_func(quick_command_free);
    for (guint i = 0; i < n; i++) {
        char *id = g_strdup_printf("id-%u", i), *name = g_strdup_printf("Command %02u", i);
        g_ptr_array_add(items, quick_command_new(id, name, "printf 'Đường 日本語' | cat", TRUE)); g_free(id); g_free(name);
    }
    g_assert_true(commands_save(path, items, NULL)); g_ptr_array_unref(items);
}
static void open_ui(void) {
    window = gtk_window_new(); gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); gtk_window_set_child(GTK_WINDOW(window), box);
    add = gtk_button_new_with_label("Add Command"); manage = gtk_button_new_with_label("Manage");
    gtk_box_append(GTK_BOX(box), add); gtk_box_append(GTK_BOX(box), manage);
    GtkWidget *space = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_widget_set_vexpand(space, TRUE); gtk_box_append(GTK_BOX(box), space);
    scroll = gtk_scrolled_window_new(); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 112); gtk_box_append(GTK_BOX(box), scroll);
    bar = gtk_flow_box_new(); gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(bar), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(bar), 100); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), bar);
    ui = quick_commands_ui_new(GTK_WINDOW(window), GTK_FLOW_BOX(bar), GTK_SCROLLED_WINDOW(scroll), add, manage, run, report, NULL);
    gtk_window_present(GTK_WINDOW(window)); settle();
}
static void close_ui(void) {
    quick_commands_ui_free(ui); ui = NULL; gtk_window_destroy(GTK_WINDOW(window)); settle(); g_clear_pointer(&last_status, g_free);
}
static void test_edit_delete(void) {
    fixture(4); open_ui(); guint before = runs; GtkWidget *button = quick_button("id-1");
    action("quick.edit", "id-1"); GtkWidget *dialog = top("Edit Command"); g_assert_nonnull(dialog);
    g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(field(dialog, "Name"))), ==, "Command 01");
    gtk_editable_set_text(GTK_EDITABLE(field(dialog, "Name")), "Command 02"); /* Duplicate display name. */
    gtk_editable_set_text(GTK_EDITABLE(field(dialog, "Command")), "echo '日本語 Đường' && pwd");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); settle();
    GPtrArray *items = load(); QuickCommand *edited = items->pdata[1];
    g_assert_cmpstr(edited->id, ==, "id-1"); g_assert_cmpstr(edited->name, ==, "Command 02");
    g_assert_cmpstr(edited->command, ==, "echo '日本語 Đường' && pwd"); g_ptr_array_unref(items);
    g_assert_true(quick_button("id-1") == button);
    action("quick.delete", "id-1"); dialog = top("Delete Quick Command?"); g_assert_nonnull(dialog);
    click(find(dialog, GTK_TYPE_BUTTON, "Cancel")); settle(); g_assert_true(quick_button("id-1") == button);
    action("quick.delete", "id-1"); click(find(top("Delete Quick Command?"), GTK_TYPE_BUTTON, "Delete")); settle();
    g_assert_null(quick_button("id-1")); g_assert_nonnull(quick_button("id-2"));
    items = load(); g_assert_cmpuint(items->len, ==, 3); g_assert_cmpstr(((QuickCommand *)items->pdata[1])->id, ==, "id-2"); g_ptr_array_unref(items);
    g_assert_cmpuint(runs, ==, before); close_ui();
}
static GtkWidget *row(const char *name) {
    GtkWidget *label = find(top("Manage Quick Commands"), GTK_TYPE_LABEL, name); g_assert_nonnull(label); return gtk_widget_get_parent(label);
}
static GtkWidget *row_action(GtkWidget *r, guint index) {
    GtkWidget *w = gtk_widget_get_next_sibling(gtk_widget_get_first_child(r));
    for (guint i = 0; i < index; i++) w = gtk_widget_get_next_sibling(w);
    return w;
}
static void test_manage_position(void) {
    fixture(80); open_ui(); click(manage); settle();
    GtkWidget *dialog = top("Manage Quick Commands"), *r = row("Command 45");
    GtkWidget *scroller = find(dialog, GTK_TYPE_SCROLLED_WINDOW, NULL);
    GtkAdjustment *a = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
    graphene_rect_t bounds; g_assert_true(gtk_widget_compute_bounds(r, gtk_widget_get_parent(r), &bounds));
    gtk_adjustment_set_value(a, bounds.origin.y - 60); settle();
    double value = gtk_adjustment_get_value(a); g_assert_cmpfloat(value, >, 1000);
    GtkWidget *down = row_action(r, 1); gtk_widget_grab_focus(down); click(down); settle();
    g_assert_true(row("Command 45") == r); g_assert_true(gtk_window_get_focus(GTK_WINDOW(dialog)) == down);
    g_assert_cmpfloat(gtk_adjustment_get_value(a), >, value - 50);
    GPtrArray *items = load(); g_assert_cmpstr(((QuickCommand *)items->pdata[46])->id, ==, "id-45"); g_ptr_array_unref(items);
    click(row_action(r, 0)); settle(); g_assert_true(row("Command 45") == r);
    click(row_action(r, 2)); GtkWidget *edit = top("Edit Command"); gtk_editable_set_text(GTK_EDITABLE(field(edit, "Name")), "Edited deep command");
    gtk_dialog_response(GTK_DIALOG(edit), GTK_RESPONSE_ACCEPT); settle(); g_assert_cmpfloat(gtk_adjustment_get_value(a), >, value - 50);
    click(row_action(r, 3)); click(find(top("Delete Quick Command?"), GTK_TYPE_BUTTON, "Delete")); settle();
    g_assert_cmpfloat(gtk_adjustment_get_value(a), >, value - 100);
    GtkWidget *next = row("Command 46"); g_assert_true(gtk_widget_is_ancestor(gtk_window_get_focus(GTK_WINDOW(dialog)), next));
    g_assert_false(gtk_widget_get_sensitive(row_action(row("Command 00"), 0)));
    g_assert_false(gtk_widget_get_sensitive(row_action(row("Command 79"), 1))); close_ui();
}
static void test_save_failure(void) {
    fixture(5); open_ui(); click(manage); settle();
    char *backup = g_strconcat(path, ".backup", NULL); g_assert_cmpint(g_rename(path, backup), ==, 0); g_assert_cmpint(g_mkdir(path, 0700), ==, 0);
    GtkWidget *first = quick_button("id-0"), *r = row("Command 02"); click(row_action(r, 1)); settle();
    g_assert_nonnull(strstr(last_status, "Changes were not applied"));
    g_assert_true(gtk_flow_box_child_get_child(gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(bar), 0)) == first);
    click(row_action(r, 3)); click(find(top("Delete Quick Command?"), GTK_TYPE_BUTTON, "Delete")); settle(); g_assert_nonnull(quick_button("id-2"));
    action("quick.edit", "id-2"); GtkWidget *edit = top("Edit Command"); gtk_editable_set_text(GTK_EDITABLE(field(edit, "Name")), "Cannot save");
    gtk_dialog_response(GTK_DIALOG(edit), GTK_RESPONSE_ACCEPT); settle(); g_assert_nonnull(top("Edit Command"));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(quick_button("id-2"))), ==, "Command 02");
    g_assert_cmpint(g_rmdir(path), ==, 0); g_assert_cmpint(g_rename(backup, path), ==, 0); g_free(backup);
    gtk_dialog_response(GTK_DIALOG(edit), GTK_RESPONSE_ACCEPT); settle(); g_assert_null(top("Edit Command"));
    GPtrArray *items = load(); g_assert_cmpstr(((QuickCommand *)items->pdata[2])->name, ==, "Cannot save"); g_ptr_array_unref(items); close_ui();
}
static void test_empty_and_invalid(void) {
    fixture(1); open_ui(); action("quick.delete", "id-0"); click(find(top("Delete Quick Command?"), GTK_TYPE_BUTTON, "Delete")); settle();
    g_assert_null(quick_button("id-0")); click(add); GtkWidget *dialog = top("Add Command");
    gtk_editable_set_text(GTK_EDITABLE(field(dialog, "Name")), "New Unicode 日本語");
    gtk_editable_set_text(GTK_EDITABLE(field(dialog, "Command")), "echo Đường");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); settle(); GPtrArray *items = load(); g_assert_cmpuint(items->len, ==, 1); g_ptr_array_unref(items); close_ui();
    g_assert_true(g_file_set_contents(path, "{broken", -1, NULL)); open_ui();
    g_assert_nonnull(quick_commands_ui_load_error(ui)); g_assert_false(gtk_widget_get_sensitive(add)); g_assert_false(gtk_widget_get_sensitive(manage));
    close_ui(); char *text = NULL; g_file_get_contents(path, &text, NULL, NULL); g_assert_cmpstr(text, ==, "{broken"); g_free(text);
}
static void test_close_pending(void) {
    fixture(30); open_ui(); click(manage); settle(); click(row_action(row("Command 20"), 1));
    action("quick.delete", "id-20"); close_ui(); g_assert_null(top("Delete Quick Command?"));
    open_ui(); action("quick.edit", "id-10"); close_ui(); g_assert_null(top("Edit Command"));
    open_ui(); click(manage); settle();
    /* Simulate parent destruction before the controller's explicit cleanup. */
    GtkWidget *dialog = top("Manage Quick Commands"); click(row_action(row("Command 10"), 1)); gtk_window_destroy(GTK_WINDOW(dialog)); settle();
    close_ui();
    open_ui(); action("quick.edit", "id-10");
    gtk_window_destroy(GTK_WINDOW(window)); quick_commands_ui_free(ui); ui = NULL; settle();
    g_assert_null(top("Edit Command")); g_clear_pointer(&last_status, g_free);
}
static void test_drag_payload(void) {
    fixture(3); open_ui(); GtkWidget *button = quick_button("id-1");
    GListModel *controllers = gtk_widget_observe_controllers(button); GdkContentProvider *provider = NULL;
    for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
        GObject *controller = g_list_model_get_item(controllers, i);
        if (GTK_IS_DRAG_SOURCE(controller)) g_signal_emit_by_name(controller, "prepare", 5., 5., &provider);
        g_object_unref(controller);
    }
    g_object_unref(controllers); g_assert_nonnull(provider);
    GdkContentFormats *formats = gdk_content_provider_ref_formats(provider);
    g_assert_false(gdk_content_formats_contain_gtype(formats, G_TYPE_STRING));
    g_assert_false(gdk_content_formats_contain_mime_type(formats, "text/plain"));
    g_assert_false(gdk_content_formats_contain_mime_type(formats, "text/plain;charset=utf-8"));
    GValue value = G_VALUE_INIT; g_value_init(&value, G_TYPE_STRING); GError *error = NULL;
    g_assert_false(gdk_content_provider_get_value(provider, &value, &error)); g_assert_nonnull(error);
    g_clear_error(&error); g_value_unset(&value); gdk_content_formats_unref(formats); g_object_unref(provider);
    close_ui();
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL); root = g_dir_make_tmp("devterm-quick-ui-XXXXXX", NULL);
    g_setenv("XDG_CONFIG_HOME", root, TRUE); path = commands_config_path(); if (!gtk_init_check()) return 77;
    g_test_add_func("/quick-ui/edit-delete", test_edit_delete);
    g_test_add_func("/quick-ui/manage-position", test_manage_position);
    g_test_add_func("/quick-ui/save-failure", test_save_failure);
    g_test_add_func("/quick-ui/empty-invalid", test_empty_and_invalid);
    g_test_add_func("/quick-ui/close-pending", test_close_pending);
    g_test_add_func("/quick-ui/drag-payload", test_drag_payload);
    int result = g_test_run(); g_remove(path); char *dir = g_path_get_dirname(path); g_rmdir(dir); g_free(dir); g_rmdir(root); g_free(root); g_free(path); return result;
}
