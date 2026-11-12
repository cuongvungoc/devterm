#include "tab-strip.h"
#include <string.h>

typedef struct {
    guint id;
    GtkWidget *page, *header, *label, *close;
    char *title;
} TabEntry;
struct TabStrip {
    GtkWindow *window;
    GtkNotebook *notebook;
    GHashTable *entries;
    GSimpleActionGroup *actions;
    TabStripNotify notify; gpointer data;
    GtkEventController *events;
    GtkWidget *menu, *rename, *name, *error;
    guint rename_id, pressed_id, close_press, finish_idle;
    GPtrArray *original_order;
    double press_x, press_y;
    gboolean dragging, cancelled, closing;
};
static TabEntry *lookup(TabStrip *s, guint id) { return g_hash_table_lookup(s->entries, GUINT_TO_POINTER(id)); }
static void notify(TabStrip *s, guint id, TabStripAction action, const char *title) {
    if (!s->closing && s->notify) s->notify(id, action, title, s->data);
}
static void entry_free(gpointer data) {
    TabEntry *entry = data;
    g_object_unref(entry->page); g_object_unref(entry->header); g_free(entry->title); g_free(entry);
}
static void close_menu(TabStrip *s) {
    GtkWidget *menu = g_steal_pointer(&s->menu);
    if (menu) {
        g_signal_handlers_disconnect_by_data(menu, s);
        gtk_popover_popdown(GTK_POPOVER(menu)); gtk_widget_unparent(menu);
    }
}
static void update_title(TabEntry *entry, const char *title) {
    g_free(entry->title); entry->title = g_strdup(title);
    gtk_label_set_text(GTK_LABEL(entry->label), title);
    gtk_widget_set_tooltip_text(entry->header, title);
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry->header), GTK_ACCESSIBLE_PROPERTY_LABEL, title, -1);
    char *close = g_strdup_printf("Close %s", title);
    gtk_widget_set_tooltip_text(entry->close, close);
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry->close), GTK_ACCESSIBLE_PROPERTY_LABEL, close, -1);
    g_free(close);
}
static void rename_destroyed(GtkWidget *dialog, gpointer data) {
    TabStrip *s = data;
    if (s->rename == dialog) {
        s->rename = s->name = s->error = NULL; s->rename_id = 0;
        notify(s, 0, TAB_STRIP_FOCUS, NULL);
    }
}
static char *clean_name(const char *text) {
    if (!g_utf8_validate(text, -1, NULL)) return NULL;
    for (const char *p = text; *p; p = g_utf8_next_char(p)) {
        gunichar character = g_utf8_get_char(p);
        GUnicodeType type = g_unichar_type(character);
        if (g_unichar_iscntrl(character) || type == G_UNICODE_LINE_SEPARATOR || type == G_UNICODE_PARAGRAPH_SEPARATOR) return NULL;
    }
    const char *start = text, *end = text + strlen(text);
    while (*start && g_unichar_isspace(g_utf8_get_char(start))) start = g_utf8_next_char(start);
    while (end > start) {
        const char *previous = g_utf8_find_prev_char(start, end);
        if (!g_unichar_isspace(g_utf8_get_char(previous))) break;
        end = previous;
    }
    if (start == end || g_utf8_strlen(start, end - start) > 128) return NULL;
    return g_strndup(start, end - start);
}
static void rename_response(GtkDialog *dialog, int response, gpointer data) {
    TabStrip *s = data; TabEntry *entry = lookup(s, s->rename_id);
    if (response == GTK_RESPONSE_ACCEPT && !s->closing && entry) {
        char *title = clean_name(gtk_editable_get_text(GTK_EDITABLE(s->name)));
        if (!title) {
            gtk_label_set_text(GTK_LABEL(s->error), "Enter 1–128 characters. A name cannot be blank or contain line breaks/control characters.");
            gtk_widget_grab_focus(s->name); return;
        }
        update_title(entry, title);
        notify(s, entry->id, TAB_STRIP_RENAMED, title); g_free(title);
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
}
static gboolean rename_key(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType state, gpointer data) {
    (void)keys; (void)code; (void)state; TabStrip *s = data;
    if (key != GDK_KEY_Escape) return FALSE;
    gtk_dialog_response(GTK_DIALOG(s->rename), GTK_RESPONSE_CANCEL); return TRUE;
}
static void show_rename(TabStrip *s, guint id) {
    TabEntry *entry = lookup(s, id); if (!entry || s->closing) return;
    if (s->rename) { gtk_window_present(GTK_WINDOW(s->rename)); return; }
    s->rename_id = id;
    s->rename = gtk_dialog_new_with_buttons("Rename Tab", s->window, GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(s->rename), TRUE);
    gtk_widget_add_css_class(s->rename, "devterm-dialog");
    gtk_dialog_set_default_response(GTK_DIALOG(s->rename), GTK_RESPONSE_ACCEPT);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(s->rename)); gtk_box_set_spacing(GTK_BOX(box), 8);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    GtkWidget *label = gtk_label_new("Tab name"); gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_box_append(GTK_BOX(box), label);
    s->name = gtk_entry_new(); gtk_widget_set_size_request(s->name, 380, -1);
    gtk_entry_set_activates_default(GTK_ENTRY(s->name), TRUE);
    gtk_editable_set_text(GTK_EDITABLE(s->name), entry->title);
    gtk_editable_select_region(GTK_EDITABLE(s->name), 0, -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(s->name), GTK_ACCESSIBLE_PROPERTY_LABEL, "Tab name", -1);
    gtk_box_append(GTK_BOX(box), s->name);
    s->error = gtk_label_new(""); gtk_label_set_wrap(GTK_LABEL(s->error), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(s->error), 50);
    gtk_widget_add_css_class(s->error, "error"); gtk_box_append(GTK_BOX(box), s->error);
    GtkEventController *keys = gtk_event_controller_key_new(); gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(rename_key), s); gtk_widget_add_controller(s->rename, keys);
    g_signal_connect(s->rename, "response", G_CALLBACK(rename_response), s);
    g_signal_connect(s->rename, "destroy", G_CALLBACK(rename_destroyed), s);
    gtk_window_present(GTK_WINDOW(s->rename)); gtk_widget_grab_focus(s->name);
}
static void menu_action(GSimpleAction *action, GVariant *parameter, gpointer data) {
    TabStrip *s = data; guint id = g_variant_get_uint32(parameter); TabEntry *entry = lookup(s, id);
    close_menu(s); if (!entry || s->closing) return;
    const char *name = g_action_get_name(G_ACTION(action));
    if (g_str_equal(name, "rename")) show_rename(s, id);
    else if (g_str_equal(name, "close")) notify(s, id, TAB_STRIP_CLOSE, NULL);
    else {
        gint index = gtk_notebook_page_num(s->notebook, entry->page);
        gint target = index + (g_str_equal(name, "left") ? -1 : 1);
        if (index >= 0 && target >= 0 && target < gtk_notebook_get_n_pages(s->notebook)) gtk_notebook_reorder_child(s->notebook, entry->page, target);
        notify(s, 0, TAB_STRIP_FOCUS, NULL);
    }
}
static void menu_closed(GtkPopover *menu, gpointer data) {
    (void)menu; notify(data, 0, TAB_STRIP_FOCUS, NULL);
}
static void show_menu(TabStrip *s, TabEntry *entry, double x, double y) {
    close_menu(s);
    GSimpleActionGroup *group = s->actions;
    gint index = gtk_notebook_page_num(s->notebook, entry->page);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(group), "left")), index > 0);
    g_simple_action_set_enabled(G_SIMPLE_ACTION(g_action_map_lookup_action(G_ACTION_MAP(group), "right")), index + 1 < gtk_notebook_get_n_pages(s->notebook));
    GMenu *model = g_menu_new();
    const char *labels[] = {"Rename Tab…", "Move Left", "Move Right", "Close Tab"};
    const char *actions[] = {"tab.rename", "tab.left", "tab.right", "tab.close"};
    for (guint i = 0; i < G_N_ELEMENTS(labels); i++) {
        GMenuItem *item = g_menu_item_new(labels[i], NULL); g_menu_item_set_action_and_target(item, actions[i], "u", entry->id);
        g_menu_append_item(model, item); g_object_unref(item);
    }
    s->menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(model)); g_object_unref(model);
    gtk_widget_set_parent(s->menu, GTK_WIDGET(s->notebook));
    graphene_point_t point = GRAPHENE_POINT_INIT(x, y), anchor;
    if (!gtk_widget_compute_point(GTK_WIDGET(s->window), GTK_WIDGET(s->notebook), &point, &anchor)) anchor = GRAPHENE_POINT_INIT(0, 0);
    GdkRectangle rect = {(int)anchor.x, (int)anchor.y, 1, 1}; gtk_popover_set_pointing_to(GTK_POPOVER(s->menu), &rect);
    g_signal_connect(s->menu, "closed", G_CALLBACK(menu_closed), s);
    gtk_popover_popup(GTK_POPOVER(s->menu));
}
static gboolean contains(GtkWidget *widget, GtkWidget *root, double x, double y) {
    graphene_rect_t bounds; graphene_point_t point = GRAPHENE_POINT_INIT(x, y);
    return gtk_widget_get_mapped(widget) && gtk_widget_compute_bounds(widget, root, &bounds) && graphene_rect_contains_point(&bounds, &point);
}
static GtkWidget *tab_hit_widget(TabEntry *entry) {
    /* Include the native tab's CSS padding in pointer hit testing. */
    GtkWidget *parent = gtk_widget_get_parent(entry->header);
    return parent && g_str_equal(gtk_widget_get_css_name(parent), "tab") ? parent : entry->header;
}
static TabEntry *at_position(TabStrip *s, double x, double y) {
    GHashTableIter iter; gpointer data; g_hash_table_iter_init(&iter, s->entries);
    while (g_hash_table_iter_next(&iter, NULL, &data)) {
        TabEntry *entry = data; if (contains(tab_hit_widget(entry), GTK_WIDGET(s->window), x, y)) return entry;
    }
    return NULL;
}
static gboolean in_tab_bar(TabStrip *s, double x, double y) {
    graphene_rect_t notebook;
    if (!gtk_widget_compute_bounds(GTK_WIDGET(s->notebook), GTK_WIDGET(s->window), &notebook) || x < notebook.origin.x || x > notebook.origin.x + notebook.size.width) return FALSE;
    GHashTableIter iter; gpointer data; g_hash_table_iter_init(&iter, s->entries);
    while (g_hash_table_iter_next(&iter, NULL, &data)) {
        TabEntry *entry = data; graphene_rect_t bounds;
        if (gtk_widget_get_mapped(entry->header) && gtk_widget_compute_bounds(entry->header, GTK_WIDGET(s->window), &bounds) &&
            y >= bounds.origin.y - 6 && y <= bounds.origin.y + bounds.size.height + 6) return TRUE;
    }
    return FALSE;
}
static void reset_native_drag(TabStrip *s) {
    /* GtkNotebook commits its internal drag when its gesture is cancelled.
     * Finish that operation first, then restore the saved order if needed. */
    GListModel *controllers = gtk_widget_observe_controllers(GTK_WIDGET(s->notebook));
    for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
        GObject *controller = g_list_model_get_item(controllers, i);
        if (GTK_IS_GESTURE_CLICK(controller)) gtk_event_controller_reset(GTK_EVENT_CONTROLLER(controller));
        g_object_unref(controller);
    }
    g_object_unref(controllers);
}
static gboolean finish_drag(gpointer data) {
    TabStrip *s = data; s->finish_idle = 0;
    if (s->cancelled && s->original_order) {
        reset_native_drag(s);
        gint position = 0;
        for (guint i = 0; i < s->original_order->len; i++) {
            GtkWidget *page = s->original_order->pdata[i];
            if (gtk_notebook_page_num(s->notebook, page) >= 0) gtk_notebook_reorder_child(s->notebook, page, position++);
        }
    }
    s->pressed_id = 0; s->dragging = s->cancelled = FALSE;
    g_clear_pointer(&s->original_order, g_ptr_array_unref);
    notify(s, 0, TAB_STRIP_FOCUS, NULL); return G_SOURCE_REMOVE;
}
static void cancel_drag(TabStrip *s) {
    if (!s->pressed_id) return;
    if (s->finish_idle) { g_source_remove(s->finish_idle); s->finish_idle = 0; }
    s->cancelled = TRUE; finish_drag(s);
}
static void restore_close_press(TabStrip *s) {
    TabEntry *entry = lookup(s, s->close_press);
    if (entry) gtk_notebook_set_tab_reorderable(s->notebook, entry->page, TRUE);
    s->close_press = 0;
}
static gboolean events(GtkEventControllerLegacy *controller, GdkEvent *event, gpointer data) {
    (void)controller; TabStrip *s = data; if (s->closing) return FALSE;
    GdkEventType type = gdk_event_get_event_type(event);
    if (type == GDK_GRAB_BROKEN) { cancel_drag(s); restore_close_press(s); return FALSE; }
    if (type == GDK_KEY_PRESS) {
        guint key = gdk_key_event_get_keyval(event);
        if (s->pressed_id && key == GDK_KEY_Escape) { cancel_drag(s); return TRUE; }
        if (key == GDK_KEY_Menu || (key == GDK_KEY_F10 && (gdk_event_get_modifier_state(event) & GDK_SHIFT_MASK))) {
            GtkWidget *focus = gtk_window_get_focus(s->window);
            TabEntry *entry = NULL; GHashTableIter iter; gpointer value; g_hash_table_iter_init(&iter, s->entries);
            while (g_hash_table_iter_next(&iter, NULL, &value)) {
                TabEntry *candidate = value;
                if (focus && (focus == candidate->header || gtk_widget_is_ancestor(focus, candidate->header))) { entry = candidate; break; }
            }
            if (!entry && focus == GTK_WIDGET(s->notebook)) {
                GtkWidget *page = gtk_notebook_get_nth_page(s->notebook, gtk_notebook_get_current_page(s->notebook));
                g_hash_table_iter_init(&iter, s->entries);
                while (g_hash_table_iter_next(&iter, NULL, &value)) if (((TabEntry *)value)->page == page) { entry = value; break; }
            }
            if (entry) {
                graphene_rect_t bounds;
                if (gtk_widget_compute_bounds(entry->header, GTK_WIDGET(s->window), &bounds)) show_menu(s, entry, bounds.origin.x + bounds.size.width / 2, bounds.origin.y + bounds.size.height);
                return TRUE;
            }
        }
        return FALSE;
    }
    if (gdk_event_get_surface(event) != gtk_native_get_surface(GTK_NATIVE(s->window))) return FALSE;
    double x, y; if (!gdk_event_get_position(event, &x, &y)) return FALSE;
    double offset_x, offset_y; gtk_native_get_surface_transform(GTK_NATIVE(s->window), &offset_x, &offset_y);
    x += offset_x; y += offset_y;
    if (type == GDK_BUTTON_PRESS) {
        TabEntry *entry = at_position(s, x, y); if (!entry) return FALSE;
        guint button = gdk_button_event_get_button(event);
        if (button == GDK_BUTTON_SECONDARY) { cancel_drag(s); show_menu(s, entry, x, y); return TRUE; }
        if (button != GDK_BUTTON_PRIMARY) return FALSE;
        close_menu(s);
        if (s->finish_idle) { g_source_remove(s->finish_idle); s->finish_idle = 0; finish_drag(s); }
        if (s->pressed_id) cancel_drag(s);
        if (contains(entry->close, GTK_WIDGET(s->window), x, y)) {
            s->close_press = entry->id; gtk_notebook_set_tab_reorderable(s->notebook, entry->page, FALSE); return FALSE;
        }
        s->pressed_id = entry->id; s->press_x = x; s->press_y = y;
        s->original_order = g_ptr_array_new_with_free_func(g_object_unref);
        for (int i = 0; i < gtk_notebook_get_n_pages(s->notebook); i++)
            g_ptr_array_add(s->original_order, g_object_ref(gtk_notebook_get_nth_page(s->notebook, i)));
    } else if (type == GDK_MOTION_NOTIFY && s->pressed_id) {
        if (gtk_drag_check_threshold(GTK_WIDGET(s->window), (int)s->press_x, (int)s->press_y, (int)x, (int)y)) s->dragging = TRUE;
    } else if (type == GDK_BUTTON_RELEASE && gdk_button_event_get_button(event) == GDK_BUTTON_PRIMARY) {
        restore_close_press(s);
        if (s->pressed_id && !s->finish_idle) {
            s->cancelled = s->dragging && !in_tab_bar(s, x, y);
            /* Let GtkNotebook process the release/commit before applying a
             * rollback and restoring the selected terminal/Draft focus. */
            s->finish_idle = g_idle_add(finish_drag, s);
        }
    }
    return FALSE;
}
static void close_clicked(GtkButton *button, gpointer data) {
    TabStrip *s = data; guint id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "tab-id"));
    if (lookup(s, id)) notify(s, id, TAB_STRIP_CLOSE, NULL);
}
TabStrip *tab_strip_new(GtkWindow *window, GtkNotebook *notebook, TabStripNotify callback, gpointer data) {
    TabStrip *s = g_new0(TabStrip, 1); s->window = window; s->notebook = g_object_ref(notebook); s->notify = callback; s->data = data;
    s->entries = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, entry_free);
    GSimpleActionGroup *group = s->actions = g_simple_action_group_new();
    const GActionEntry actions[] = {
        {.name="rename", .activate=menu_action, .parameter_type="u"}, {.name="left", .activate=menu_action, .parameter_type="u"},
        {.name="right", .activate=menu_action, .parameter_type="u"}, {.name="close", .activate=menu_action, .parameter_type="u"}
    };
    g_action_map_add_action_entries(G_ACTION_MAP(group), actions, G_N_ELEMENTS(actions), s);
    gtk_widget_insert_action_group(GTK_WIDGET(notebook), "tab", G_ACTION_GROUP(group));
    s->events = gtk_event_controller_legacy_new(); gtk_event_controller_set_propagation_phase(s->events, GTK_PHASE_CAPTURE);
    g_signal_connect(s->events, "event", G_CALLBACK(events), s); gtk_widget_add_controller(GTK_WIDGET(window), s->events);
    return s;
}
int tab_strip_append(TabStrip *s, guint id, GtkWidget *page, const char *title, const char *icon) {
    TabEntry *entry = g_new0(TabEntry, 1); entry->id = id; entry->page = g_object_ref(page);
    entry->header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); g_object_ref_sink(entry->header);
    gtk_box_append(GTK_BOX(entry->header), gtk_image_new_from_icon_name(icon));
    entry->label = gtk_label_new(title); gtk_label_set_ellipsize(GTK_LABEL(entry->label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(entry->label), 22); gtk_label_set_width_chars(GTK_LABEL(entry->label), 12);
    gtk_box_append(GTK_BOX(entry->header), entry->label);
    entry->close = gtk_button_new_from_icon_name("window-close-symbolic"); gtk_widget_set_focus_on_click(entry->close, FALSE);
    gtk_widget_add_css_class(entry->close, "flat"); gtk_widget_add_css_class(entry->close, "tab-close");
    g_object_set_data(G_OBJECT(entry->close), "tab-id", GUINT_TO_POINTER(id)); g_signal_connect(entry->close, "clicked", G_CALLBACK(close_clicked), s);
    gtk_box_append(GTK_BOX(entry->header), entry->close); update_title(entry, title);
    g_hash_table_insert(s->entries, GUINT_TO_POINTER(id), entry);
    int position = gtk_notebook_append_page(s->notebook, page, entry->header);
    gtk_notebook_set_tab_reorderable(s->notebook, page, TRUE); gtk_notebook_set_tab_detachable(s->notebook, page, FALSE);
    return position;
}
void tab_strip_remove(TabStrip *s, guint id) {
    if (!s) return;
    if (s->pressed_id) cancel_drag(s);
    restore_close_press(s); close_menu(s);
    if (s->rename && s->rename_id == id) gtk_window_destroy(GTK_WINDOW(s->rename));
    TabEntry *entry = lookup(s, id);
    if (entry) g_signal_handlers_disconnect_by_data(entry->close, s);
    g_hash_table_remove(s->entries, GUINT_TO_POINTER(id));
}
gboolean tab_strip_busy(TabStrip *s) { return s && (s->pressed_id || s->rename || (s->menu && gtk_widget_get_visible(s->menu))); }
void tab_strip_free(TabStrip *s) {
    if (!s) return;
    s->closing = TRUE; cancel_drag(s); restore_close_press(s); close_menu(s);
    if (s->rename) gtk_window_destroy(GTK_WINDOW(s->rename));
    gtk_widget_remove_controller(GTK_WIDGET(s->window), s->events);
    gtk_widget_insert_action_group(GTK_WIDGET(s->notebook), "tab", NULL);
    GHashTableIter iter; gpointer value; g_hash_table_iter_init(&iter, s->entries);
    while (g_hash_table_iter_next(&iter, NULL, &value)) g_signal_handlers_disconnect_by_data(((TabEntry *)value)->close, s);
    g_hash_table_unref(s->entries); g_object_unref(s->actions); g_object_unref(s->notebook); g_free(s);
}
