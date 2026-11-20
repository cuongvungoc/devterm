/* Geometry and lifecycle checks through the real GTK application controllers. */
#define main devterm_application_main
#include "../src/main.c"
#undef main
#include <glib/gstdio.h>

static GtkApplication *application;
static char *root;
static void settle(void) {
    gint64 end = g_get_monotonic_time() + 220000;
    while (g_get_monotonic_time() < end) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
}
static GtkWidget *find(GtkWidget *widget, GType type, const char *text) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type) && (!text ||
        (GTK_IS_BUTTON(widget) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), text) == 0))) return widget;
    for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *found = find(c, type, text); if (found) return found;
    }
    return NULL;
}
static GtkWidget *with_tooltip(GtkWidget *widget, const char *text) {
    if (g_strcmp0(gtk_widget_get_tooltip_text(widget), text) == 0) return widget;
    for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *found = with_tooltip(c, text); if (found) return found;
    }
    return NULL;
}
static GtkWidget *top(const char *title) {
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; i < g_list_model_get_n_items(windows); i++) {
        GtkWindow *w = g_list_model_get_item(windows, i);
        gboolean found = gtk_widget_get_visible(GTK_WIDGET(w)) && g_strcmp0(gtk_window_get_title(w), title) == 0;
        g_object_unref(w); if (found) return GTK_WIDGET(w);
    }
    return NULL;
}
static GtkWidget *scroll_of(GtkWidget *list) {
    for (GtkWidget *p = gtk_widget_get_parent(list); p; p = gtk_widget_get_parent(p)) if (GTK_IS_SCROLLED_WINDOW(p)) return p;
    g_assert_not_reached();
}
static GtkWidget *step_entry(GtkWidget *widget) {
    if (GTK_IS_ENTRY(widget) && g_strcmp0(gtk_entry_get_placeholder_text(GTK_ENTRY(widget)), "One command line") == 0) return widget;
    for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *found = step_entry(c); if (found) return found;
    }
    return NULL;
}
static guint visible_rows(GtkWidget *list, GtkWidget *scroll) {
    guint result = 0;
    for (GtkWidget *row = gtk_widget_get_first_child(list); row; row = gtk_widget_get_next_sibling(row)) {
        graphene_rect_t b;
        if (gtk_widget_get_visible(row) && gtk_widget_compute_bounds(row, scroll, &b) &&
            b.origin.y >= 0 && b.origin.y + b.size.height <= gtk_widget_get_height(scroll)) result++;
    }
    return result;
}
static void capture(GtkWidget *dialog, const char *name) {
    const char *prefix = g_getenv("DEVTERM_DIALOG_CAPTURE"); if (!prefix) return;
    GdkPaintable *paintable = gtk_widget_paintable_new(dialog); GtkSnapshot *snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), gtk_widget_get_width(dialog), gtk_widget_get_height(dialog));
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot); g_assert_nonnull(node);
    GdkTexture *texture = gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(dialog)), node, NULL);
    char *path = g_strdup_printf("%s-%s.png", prefix, name); g_assert_true(gdk_texture_save_to_png(texture, path));
    g_free(path); g_object_unref(texture); gsk_render_node_unref(node); g_object_unref(paintable);
}
static void assert_list(GtkWidget *dialog, GtkWidget *list, const char *name) {
    settle(); GtkWidget *scroll = scroll_of(list);
    int width = gtk_widget_get_width(dialog), height = gtk_widget_get_height(dialog);
    GdkMonitor *monitor = gdk_display_get_monitor_at_surface(gtk_widget_get_display(dialog), gtk_native_get_surface(GTK_NATIVE(dialog)));
    GdkRectangle screen; gdk_monitor_get_geometry(monitor, &screen);
    g_assert_cmpint(width, <=, screen.width); g_assert_cmpint(height, <=, screen.height);
    g_assert_cmpfloat((double)gtk_widget_get_height(scroll) / height, >=, .55);
    guint rows = visible_rows(list, scroll); g_test_message("%s: %dx%d, list %d px, %u full rows", name, width, height, gtk_widget_get_height(scroll), rows);
    g_assert_cmpuint(rows, >=, screen.height >= 1000 ? 10 : 6);
    GtkWidget *close = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
    if (!close) close = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    graphene_rect_t b; g_assert_true(gtk_widget_compute_bounds(close, dialog, &b));
    g_assert_cmpfloat(b.origin.y, >=, 0); g_assert_cmpfloat(b.origin.y + b.size.height, <=, height);
    capture(dialog, name);
    GtkWidget *info = with_tooltip(dialog, "More information");
    g_assert_true(info != NULL || g_str_equal(name, "quick-manage"));
    if (info) {
        g_assert_true(GTK_IS_MENU_BUTTON(info)); gtk_menu_button_popup(GTK_MENU_BUTTON(info)); settle();
        GtkPopover *popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(info)); g_assert_true(gtk_widget_get_visible(GTK_WIDGET(popover)));
        if (g_getenv("DEVTERM_DIALOG_TEST_WM")) {
            const char *args[] = {"xdotool", "key", "--clearmodifiers", "Escape", NULL}; int status;
            g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL));
            g_assert_cmpint(status, ==, 0); settle();
            g_assert_true(top(gtk_window_get_title(GTK_WINDOW(dialog))) == dialog);
            g_assert_false(gtk_widget_get_visible(GTK_WIDGET(popover)));
        } else { gtk_popover_popdown(popover); settle(); }
    }
}
static void test_lists(void) {
    activate(application, NULL); settle(); GtkWindow *window = gtk_application_get_active_window(application);
    App *app = g_object_get_data(G_OBJECT(window), "app");
    if (g_getenv("DEVTERM_DIALOG_LIGHT")) { g_action_group_activate_action(G_ACTION_GROUP(window), "dark-appearance", NULL); settle(); }
    session_manager_show(app->sessions); settle(); GtkWidget *d = top("Sessions");
    GtkWidget *list = find(d, GTK_TYPE_LIST_BOX, NULL); assert_list(d, list, "sessions");
    if (g_getenv("DEVTERM_DIALOG_TEST_WM")) {
        g_signal_emit_by_name(with_tooltip(d, "Maximize dialog"), "clicked"); settle();
        g_assert_true(gtk_window_is_maximized(GTK_WINDOW(d)));
        g_signal_emit_by_name(with_tooltip(d, "Restore dialog size"), "clicked"); settle();
        g_assert_false(gtk_window_is_maximized(GTK_WINDOW(d)));
        g_test_message("Maximize and Restore confirmed with a window manager");
    }
    /* Resizing reflows profile labels; opening again restores the resized size. */
    gtk_list_box_select_row(GTK_LIST_BOX(list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 3));
    gtk_window_set_default_size(GTK_WINDOW(d), 760, 610); settle();
    int width = gtk_widget_get_width(d), height = gtk_widget_get_height(d);
    GtkWidget *content = gtk_list_box_row_get_child(gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 3));
    GtkWidget *labels = gtk_widget_get_last_child(content);
    graphene_rect_t name_bounds, endpoint_bounds;
    g_assert_true(gtk_widget_compute_bounds(gtk_widget_get_first_child(labels), labels, &name_bounds));
    g_assert_true(gtk_widget_compute_bounds(gtk_widget_get_last_child(labels), labels, &endpoint_bounds));
    g_assert_cmpfloat(endpoint_bounds.origin.y, >, name_bounds.origin.y);
    g_assert_cmpint(gtk_list_box_row_get_index(gtk_list_box_get_selected_row(GTK_LIST_BOX(list))), ==, 3);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CLOSE); settle(); session_manager_show(app->sessions); settle(); d = top("Sessions");
    g_assert_cmpint(gtk_widget_get_width(d), ==, width); g_assert_cmpint(gtk_widget_get_height(d), ==, height);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CLOSE); settle();
    quick_commands_ui_manage(app->quick_commands); settle(); d = top("Manage Quick Commands");
    GtkWidget *label = find(d, GTK_TYPE_BUTTON, "Edit"), *row = gtk_widget_get_parent(label); list = gtk_widget_get_parent(row);
    assert_list(d, list, "quick-manage"); gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CLOSE); settle();
    g_assert_true(gtk_widget_activate_action(app->quick_bar, "quick.edit", "s", "sequence")); settle(); d = top("Edit Command");
    GtkWidget *entry = step_entry(d); list = gtk_widget_get_parent(gtk_widget_get_parent(entry)); assert_list(d, list, "sequence");
    /* Add focuses and reveals the new row; cancel leaves the saved 30 steps. */
    g_signal_emit_by_name(find(d, GTK_TYPE_BUTTON, "Add Command Step"), "clicked"); settle();
    GtkWidget *last = gtk_widget_get_last_child(list), *scroll = scroll_of(list); graphene_rect_t b;
    g_assert_true(gtk_widget_compute_bounds(last, scroll, &b));
    g_assert_cmpfloat(b.origin.y, >=, -1); g_assert_cmpfloat(b.origin.y + b.size.height, <=, gtk_widget_get_height(scroll) + 1);
    g_signal_emit_by_name(with_tooltip(last, "Move step up"), "clicked"); settle();
    g_assert_true(gtk_widget_get_prev_sibling(gtk_widget_get_last_child(list)) == last);
    g_assert_true(gtk_widget_is_ancestor(gtk_window_get_focus(GTK_WINDOW(d)), last));
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); settle();
    show_shortcuts(app); settle(); d = top("Keyboard Shortcuts"); assert_list(d, find(d, GTK_TYPE_LIST_BOX, NULL), "shortcuts");
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); settle();
    char *path = commands_config_path(); GPtrArray *items = commands_load(path, NULL); g_free(path);
    g_assert_cmpuint(((QuickCommand *)items->pdata[0])->steps->len, ==, 30); g_ptr_array_unref(items);
    gtk_window_destroy(window); settle();
}
/* Observe completed GTK frames, including the very first painted frame.
 * An eventual geometry assertion alone misses the opening jump in the report. */
typedef struct {
    GtkWidget *window, *anchors[3];
    graphene_rect_t bounds[3];
    int width, height;
    guint frames;
    gboolean reference;
} FrameWatch;
static void watch_frame(GdkFrameClock *clock, gpointer data) {
    (void)clock; FrameWatch *watch = data;
    int width = gtk_widget_get_width(watch->window), height = gtk_widget_get_height(watch->window);
    g_assert_cmpint(width, >, 0); g_assert_cmpint(height, >, 0);
    if (watch->reference) { g_assert_cmpint(width, ==, watch->width); g_assert_cmpint(height, ==, watch->height); }
    for (guint i = 0; i < G_N_ELEMENTS(watch->anchors); i++) {
        graphene_rect_t bounds;
        g_assert_true(gtk_widget_compute_bounds(watch->anchors[i], watch->window, &bounds));
        if (watch->reference) g_assert_true(graphene_rect_equal(&bounds, &watch->bounds[i]));
        watch->bounds[i] = bounds;
    }
    watch->width = width; watch->height = height; watch->reference = TRUE; watch->frames++;
    if (watch->frames < 6) gtk_widget_queue_draw(watch->window);
}
static void watch_settle(FrameWatch *watch) {
    GdkFrameClock *clock = gtk_widget_get_frame_clock(watch->window);
    g_assert_nonnull(clock); guint before = watch->frames;
    gulong handler = g_signal_connect(clock, "after-paint", G_CALLBACK(watch_frame), watch);
    gtk_widget_queue_draw(watch->window); settle(); g_signal_handler_disconnect(clock, handler);
    g_assert_cmpuint(watch->frames, >, before);
}
static void reset_sizes(GtkWindow *window) {
    GHashTable *sizes = g_object_get_data(G_OBJECT(gtk_widget_get_display(GTK_WIDGET(window))), "devterm-dialog-sizes");
    if (sizes) g_hash_table_remove_all(sizes);
}
static void assert_row_mode(GtkWidget *labels, gboolean wide) {
    graphene_rect_t name, endpoint;
    g_assert_true(gtk_widget_compute_bounds(gtk_widget_get_first_child(labels), labels, &name));
    g_assert_true(gtk_widget_compute_bounds(gtk_widget_get_last_child(labels), labels, &endpoint));
    g_assert_cmpint(endpoint.origin.y == name.origin.y, ==, wide);
}
static void resize_labels(GtkWidget *dialog, GtkWidget *labels, int width) {
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(dialog));
    gtk_window_set_default_size(GTK_WINDOW(dialog), gdk_surface_get_width(surface) + width - gtk_widget_get_width(labels),
                                gdk_surface_get_height(surface)); settle();
}
static void test_opening_frames(void) {
    activate(application, NULL); settle(); GtkWindow *window = gtk_application_get_active_window(application);
    App *app = g_object_get_data(G_OBJECT(window), "app"); reset_sizes(window);
    int saved_width = 0, saved_height = 0;
    for (guint open = 0; open < 4; open++) {
        session_manager_show(app->sessions); GtkWidget *d = top("Sessions");
        GtkWidget *list = find(d, GTK_TYPE_LIST_BOX, NULL);
        GtkWidget *row = GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
        GtkWidget *labels = gtk_widget_get_last_child(gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row)));
        FrameWatch watch = {.window = d, .anchors = {row, gtk_widget_get_first_child(labels), gtk_widget_get_last_child(labels)}};
        watch_settle(&watch); g_assert_cmpuint(watch.frames, >=, 2);
        if (open) {
            g_assert_cmpint(watch.width, ==, saved_width); g_assert_cmpint(watch.height, ==, saved_height);
        }
        gtk_list_box_select_row(GTK_LIST_BOX(list), GTK_LIST_BOX_ROW(row));
        /* Enter and leave the dead band from both sides. No row replacement. */
        resize_labels(d, labels, 740); assert_row_mode(labels, TRUE);
        resize_labels(d, labels, 680); assert_row_mode(labels, TRUE);
        resize_labels(d, labels, 640); assert_row_mode(labels, FALSE);
        resize_labels(d, labels, 680); assert_row_mode(labels, FALSE);
        g_assert_true(gtk_list_box_get_selected_row(GTK_LIST_BOX(list)) == GTK_LIST_BOX_ROW(row));
        if (open % 2) resize_labels(d, labels, 740);
        saved_width = gtk_widget_get_width(d); saved_height = gtk_widget_get_height(d);
        gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CLOSE); settle();
    }
    gtk_window_destroy(window); settle();
}
static void test_shortcut_opening_frames(void) {
    activate(application, NULL); settle(); GtkWindow *window = gtk_application_get_active_window(application);
    App *app = g_object_get_data(G_OBJECT(window), "app"); reset_sizes(window);
    const int widths[] = {900, 880, 920, 760};
    for (guint i = 0; i < G_N_ELEMENTS(widths); i++) {
        show_shortcuts(app); GtkWidget *d = top("Keyboard Shortcuts");
        gtk_window_set_default_size(GTK_WINDOW(d), widths[i], 640); settle();
        int width = gtk_widget_get_width(d), height = gtk_widget_get_height(d);
        gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); settle();
        show_shortcuts(app); d = top("Keyboard Shortcuts");
        GtkWidget *list = find(d, GTK_TYPE_LIST_BOX, NULL), *row = GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
        GtkWidget *labels = gtk_widget_get_first_child(gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row)));
        FrameWatch watch = {.window = d, .anchors = {row, gtk_widget_get_first_child(labels), gtk_widget_get_last_child(labels)}};
        watch_settle(&watch); g_assert_cmpuint(watch.frames, >=, 2);
        g_assert_cmpint(watch.width, ==, width); g_assert_cmpint(watch.height, ==, height);
        resize_labels(d, labels, 460); assert_row_mode(labels, TRUE);
        resize_labels(d, labels, 420); assert_row_mode(labels, TRUE);
        resize_labels(d, labels, 380); assert_row_mode(labels, FALSE);
        resize_labels(d, labels, 420); assert_row_mode(labels, FALSE);
        gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); settle();
    }
    gtk_window_destroy(window); settle();
}
static char *window_geometry(GtkWidget *window) {
    if (!g_getenv("DEVTERM_DIALOG_TEST_WM")) return NULL;
    char *pid = g_strdup_printf("%d", (int)getpid()), *output = NULL; int status;
    const char *args[] = {"xdotool", "search", "--onlyvisible", "--pid", pid, "--name",
                          gtk_window_get_title(GTK_WINDOW(window)), "getwindowgeometry", "--shell", NULL};
    g_assert_true(g_spawn_sync(NULL, (char **)args, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &output, NULL, &status, NULL));
    g_assert_cmpint(status, ==, 0); g_free(pid); return output;
}
static void test_editor_frames(void) {
    activate(application, NULL); settle(); GtkWindow *window = gtk_application_get_active_window(application);
    App *app = g_object_get_data(G_OBJECT(window), "app"); reset_sizes(window);
    int saved_width = 0, saved_height = 0;
    for (guint open = 0; open < 4; open++) {
        if (open == 0) g_signal_emit_by_name(app->command_add, "clicked");
        else g_assert_true(gtk_widget_activate_action(app->quick_bar, "quick.edit", "s", open % 2 ? "sequence" : "command-0"));
        GtkWidget *d = top(open ? "Edit Command" : "Add Command"); g_assert_nonnull(d);
        GtkWidget *kind = find(d, GTK_TYPE_DROP_DOWN, NULL), *stack = step_entry(d);
        while (stack && !(GTK_IS_STACK(stack) && gtk_stack_get_child_by_name(GTK_STACK(stack), "command")))
            stack = gtk_widget_get_parent(stack);
        g_assert_nonnull(stack);
        GtkWidget *name = find(d, GTK_TYPE_ENTRY, NULL), *save = gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
        FrameWatch watch = {.window = d, .anchors = {kind, name, save}};
        watch_settle(&watch);
        if (open) { g_assert_cmpint(watch.width, ==, saved_width); g_assert_cmpint(watch.height, ==, saved_height); }
        GtkWidget *single = gtk_stack_get_child_by_name(GTK_STACK(stack), "command");
        GtkWidget *command = find(single, GTK_TYPE_ENTRY, NULL);
        gtk_editable_set_text(GTK_EDITABLE(command), "printf 'giữ nguyên · 東京'");
        GtkWidget *step = step_entry(d); gtk_editable_set_text(GTK_EDITABLE(step), "echo sequence");
        char *geometry = window_geometry(d);
        for (guint toggle = 0; toggle < 8; toggle++) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(kind), toggle % 2); watch_settle(&watch);
        }
        g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(command)), ==, "printf 'giữ nguyên · 東京'");
        g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(step)), ==, "echo sequence");
        char *after = window_geometry(d); g_assert_cmpstr(after, ==, geometry); g_free(after); g_free(geometry);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(kind), QUICK_COMMAND); watch_settle(&watch);
        if (!open) capture(d, "editor-command");
        gtk_drop_down_set_selected(GTK_DROP_DOWN(kind), QUICK_SEQUENCE); watch_settle(&watch);
        if (!open) capture(d, "editor-sequence");
        /* Even after shrinking in Command mode, Sequence cannot enlarge it. */
        gtk_drop_down_set_selected(GTK_DROP_DOWN(kind), QUICK_COMMAND); settle();
        gtk_window_set_default_size(GTK_WINDOW(d), 760, 480); settle();
        watch.reference = FALSE; watch_settle(&watch);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(kind), QUICK_SEQUENCE); watch_settle(&watch);
        saved_width = gtk_widget_get_width(d); saved_height = gtk_widget_get_height(d);
        gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); settle();
    }
    char *path = commands_config_path(); GPtrArray *items = commands_load(path, NULL); g_free(path);
    g_assert_cmpuint(items->len, ==, 31); g_assert_cmpuint(((QuickCommand *)items->pdata[0])->steps->len, ==, 30);
    g_ptr_array_unref(items); gtk_window_destroy(window); settle();
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL); root = g_dir_make_tmp("devterm-dialog-layout-XXXXXX", NULL); g_setenv("XDG_CONFIG_HOME", root, TRUE);
    if (!gtk_init_check()) return 77;
    if (g_getenv("DEVTERM_DIALOG_LARGE_FONT")) g_object_set(gtk_settings_get_default(), "gtk-font-name", "Sans 13", NULL);
    GPtrArray *items = g_ptr_array_new_with_free_func(quick_command_free); QuickCommand *seq = quick_sequence_new("sequence", "Sequence — 日本語");
    for (guint i = 0; i < 30; i++) { char *text = g_strdup_printf("printf 'Command %u · Đường' | cat", i + 1); g_ptr_array_add(seq->steps, quick_command_step_new(text, 200)); g_free(text); }
    g_ptr_array_add(items, seq);
    for (guint i = 0; i < 30; i++) { char *id = g_strdup_printf("command-%u", i), *name = g_strdup_printf("Command %u", i); g_ptr_array_add(items, quick_command_new(id, name, "pwd", TRUE)); g_free(id); g_free(name); }
    char *path = commands_config_path(); g_assert_true(commands_save(path, items, NULL)); g_free(path); g_ptr_array_unref(items);
    items = g_ptr_array_new_with_free_func(session_profile_free);
    for (guint i = 0; i < 30; i++) { SessionProfile *p = session_profile_new(); g_free(p->name); p->name = g_strdup_printf("Router %02u — 東京", i); g_free(p->host); p->host = g_strdup("192.0.2.1"); g_ptr_array_add(items, p); }
    path = sessions_config_path(); g_assert_true(sessions_save(path, items, NULL)); g_free(path); g_ptr_array_unref(items);
    application = gtk_application_new("com.example.DevTerm.DialogLayoutTests", G_APPLICATION_NON_UNIQUE); g_assert_true(g_application_register(G_APPLICATION(application), NULL, NULL));
    g_test_add_func("/dialogs/lists-reflow-size-memory-focus", test_lists);
    g_test_add_func("/dialogs/opening-frames", test_opening_frames);
    g_test_add_func("/dialogs/editor-frames", test_editor_frames);
    g_test_add_func("/dialogs/shortcut-opening-frames", test_shortcut_opening_frames); int result = g_test_run(); g_object_unref(application);
    char *dir = g_build_filename(root, "devterm", NULL); GDir *files = g_dir_open(dir, 0, NULL); const char *name;
    while ((name = g_dir_read_name(files))) { path = g_build_filename(dir, name, NULL); g_remove(path); g_free(path); }
    g_dir_close(files); g_rmdir(dir); g_rmdir(root); g_free(dir); g_free(root); return result;
}
