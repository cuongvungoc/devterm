#include "dialog-layout.h"

/* A height-for-width container chooses its layout during GTK measurement,
 * before the first snapshot. No idle callback changes already painted rows.
 * The dead band keeps a manually resized row stable near the breakpoint. */
typedef struct {
    GtkWidget parent;
    GtkWidget *first, *second;
    int narrow, wide_threshold, first_percent;
    gboolean allocated, wide;
} DialogPair;
typedef GtkWidgetClass DialogPairClass;
G_DEFINE_TYPE(DialogPair, dialog_pair, GTK_TYPE_WIDGET)
static gboolean labels_wide(DialogPair *self, int width) {
    int first, second;
    gtk_widget_measure(self->first, GTK_ORIENTATION_HORIZONTAL, -1, &first, NULL, NULL, NULL);
    gtk_widget_measure(self->second, GTK_ORIENTATION_HORIZONTAL, -1, &second, NULL, NULL, NULL);
    return width >= MAX(first + second + 16, self->allocated && self->wide ? self->narrow : self->wide_threshold);
}
static GtkSizeRequestMode labels_request_mode(GtkWidget *widget) {
    (void)widget; return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}
static void labels_measure(GtkWidget *widget, GtkOrientation orientation, int width,
                           int *minimum, int *natural, int *min_baseline, int *nat_baseline) {
    DialogPair *self = (DialogPair *)widget;
    int a, b, an, bn;
    gtk_widget_measure(self->first, orientation, -1, &a, &an, NULL, NULL);
    gtk_widget_measure(self->second, orientation, -1, &b, &bn, NULL, NULL);
    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        *minimum = MAX(a, b); *natural = MAX(*minimum, an + bn + 16);
    } else if (labels_wide(self, width)) {
        *minimum = MAX(a, b); *natural = MAX(an, bn);
    } else { *minimum = a + b + 2; *natural = an + bn + 2; }
    *min_baseline = *nat_baseline = -1;
}
static void labels_allocate(GtkWidget *widget, int width, int height, int baseline) {
    (void)baseline; DialogPair *self = (DialogPair *)widget;
    gboolean wide = labels_wide(self, width), changed = self->allocated && self->wide != wide;
    self->wide = wide; self->allocated = TRUE;
    if (wide) {
        int first_min, second_min;
        gtk_widget_measure(self->first, GTK_ORIENTATION_HORIZONTAL, -1, &first_min, NULL, NULL, NULL);
        gtk_widget_measure(self->second, GTK_ORIENTATION_HORIZONTAL, -1, &second_min, NULL, NULL, NULL);
        int first = CLAMP((width - 16) * self->first_percent / 100, first_min, width - 16 - second_min);
        int second = width - first - 16;
        gboolean rtl = gtk_widget_get_direction(widget) == GTK_TEXT_DIR_RTL;
        GtkAllocation a = {rtl ? width - first : 0, 0, first, height};
        GtkAllocation b = {rtl ? 0 : first + 16, 0, second, height};
        gtk_widget_size_allocate(self->first, &a, -1); gtk_widget_size_allocate(self->second, &b, -1);
    } else {
        int first; gtk_widget_measure(self->first, GTK_ORIENTATION_VERTICAL, width, &first, NULL, NULL, NULL);
        first = MIN(first, height);
        GtkAllocation a = {0, 0, width, first}, b = {0, first + 2, width, MAX(0, height - first - 2)};
        gtk_widget_size_allocate(self->first, &a, -1); gtk_widget_size_allocate(self->second, &b, -1);
    }
    /* Invalidate height requests cached on the other side of the dead band. */
    if (changed) gtk_widget_queue_resize(widget);
}
static void labels_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    DialogPair *self = (DialogPair *)widget;
    gtk_widget_snapshot_child(widget, self->first, snapshot); gtk_widget_snapshot_child(widget, self->second, snapshot);
}
static void labels_dispose(GObject *object) {
    DialogPair *self = (DialogPair *)object;
    g_clear_pointer(&self->first, gtk_widget_unparent); g_clear_pointer(&self->second, gtk_widget_unparent);
    G_OBJECT_CLASS(dialog_pair_parent_class)->dispose(object);
}
static void dialog_pair_class_init(DialogPairClass *klass) {
    klass->get_request_mode = labels_request_mode; klass->measure = labels_measure;
    klass->size_allocate = labels_allocate; klass->snapshot = labels_snapshot;
    G_OBJECT_CLASS(klass)->dispose = labels_dispose;
    gtk_widget_class_set_css_name(klass, "dialog-pair");
    gtk_widget_class_set_accessible_role(klass, GTK_ACCESSIBLE_ROLE_GROUP);
}
static void dialog_pair_init(DialogPair *self) { (void)self; }
GtkWidget *dialog_layout_pair(GtkWidget *name, GtkWidget *endpoint, int narrow, int wide, int first_percent) {
    DialogPair *self = g_object_new(dialog_pair_get_type(), NULL);
    self->first = name; self->second = endpoint;
    self->narrow = narrow; self->wide_threshold = wide; self->first_percent = first_percent;
    gtk_widget_set_parent(name, GTK_WIDGET(self)); gtk_widget_set_parent(endpoint, GTK_WIDGET(self));
    return GTK_WIDGET(self);
}

typedef struct { int width, height; } Size;
typedef struct {
    char *key;
    int surface_width, surface_height;
    guint settle_source;
    gboolean closed, restoring;
    gboolean surface_maximized, surface_fullscreen;
} Layout;
static GHashTable *sizes(GdkDisplay *display) {
    GHashTable *table = g_object_get_data(G_OBJECT(display), "devterm-dialog-sizes");
    if (!table) {
        table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_object_set_data_full(G_OBJECT(display), "devterm-dialog-sizes", table, (GDestroyNotify)g_hash_table_unref);
    }
    return table;
}
static void layout_free(gpointer data) { Layout *layout = data; g_free(layout->key); g_free(layout); }
static void remember_bounds(GtkWindow *window, Layout *layout) {
    if (!layout->restoring && !layout->surface_maximized && !layout->surface_fullscreen &&
        !gtk_window_is_maximized(window) && !gtk_window_is_fullscreen(window) && layout->surface_width > 0 && layout->surface_height > 0) {
        /* GtkWindow defaults use native bounds, including CSD margins. Saving
         * widget allocation would shrink the dialog on every reopen. */
        Size *size = g_new(Size, 1); *size = (Size){layout->surface_width, layout->surface_height};
        g_hash_table_replace(sizes(gtk_widget_get_display(GTK_WIDGET(window))), g_strdup(layout->key), size);
    }
}
static gboolean remember_size(gpointer data) {
    GtkWindow *window = data; Layout *layout = g_object_get_data(G_OBJECT(window), "devterm-dialog-layout"); layout->settle_source = 0;
    if (!layout->closed && gtk_widget_get_mapped(GTK_WIDGET(window))) remember_bounds(window, layout);
    return G_SOURCE_REMOVE;
}
static void unmapped(GtkWidget *widget, gpointer data) {
    (void)data; Layout *layout = g_object_get_data(G_OBJECT(widget), "devterm-dialog-layout");
    /* Closing immediately after a resize must retain the last native bounds,
     * even when the debounce has not fired. Do not save maximize transitions. */
    if (!layout->closed) remember_bounds(GTK_WINDOW(widget), layout);
    if (layout->settle_source) { guint source = layout->settle_source; layout->settle_source = 0; g_source_remove(source); }
}
static void surface_layout(GdkSurface *surface, int width, int height, gpointer data) {
    (void)surface; GtkWindow *window = data; Layout *layout = g_object_get_data(G_OBJECT(window), "devterm-dialog-layout");
    gboolean maximized = gtk_window_is_maximized(window), fullscreen = gtk_window_is_fullscreen(window);
    if (layout->closed || (layout->surface_width == width && layout->surface_height == height &&
        layout->surface_maximized == maximized && layout->surface_fullscreen == fullscreen)) return;
    layout->surface_maximized = maximized; layout->surface_fullscreen = fullscreen;
    layout->surface_width = width; layout->surface_height = height; layout->restoring = FALSE;
    /* Remember settled native bounds, not intermediate configure events while
     * mapping, maximizing or restoring a window. */
    if (layout->settle_source) g_source_remove(layout->settle_source);
    /* The destroy handler cancels this source. A strong reference here would
     * defer GtkWidget::destroy and leave controllers pointing at a closed window. */
    layout->settle_source = g_timeout_add(150, remember_size, window);
}
static void realized(GtkWidget *widget, gpointer data) {
    (void)data; GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    g_signal_connect_object(surface, "layout", G_CALLBACK(surface_layout), widget, G_CONNECT_AFTER);
}
static void destroyed(GtkWidget *widget, gpointer data) {
    (void)data; Layout *layout = g_object_get_data(G_OBJECT(widget), "devterm-dialog-layout"); layout->closed = TRUE;
    if (layout->settle_source) { guint source = layout->settle_source; layout->settle_source = 0; g_source_remove(source); }
}
static void maximize_clicked(GtkButton *button, gpointer data) {
    (void)button; GtkWindow *window = data;
    if (gtk_window_is_maximized(window)) gtk_window_unmaximize(window); else gtk_window_maximize(window);
}
static void maximized_changed(GtkWindow *window, GParamSpec *pspec, gpointer data) {
    (void)pspec; GtkButton *button = data; gboolean maximized = gtk_window_is_maximized(window);
    gtk_button_set_icon_name(button, maximized ? "view-restore-symbolic" : "view-fullscreen-symbolic");
    const char *label = maximized ? "Restore dialog size" : "Maximize dialog";
    gtk_widget_set_tooltip_text(GTK_WIDGET(button), label);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
}
void dialog_layout_apply(GtkWindow *window, const char *key, int width, int height) {
    Layout *layout = g_object_get_data(G_OBJECT(window), "devterm-dialog-layout");
    if (!layout) {
        layout = g_new0(Layout, 1); g_object_set_data_full(G_OBJECT(window), "devterm-dialog-layout", layout, layout_free);
        g_signal_connect(window, "realize", G_CALLBACK(realized), NULL); g_signal_connect(window, "destroy", G_CALLBACK(destroyed), NULL);
        g_signal_connect(window, "unmap", G_CALLBACK(unmapped), NULL);
        GtkWidget *header = gtk_header_bar_new(); gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(header), ":close");
        GtkWidget *maximize = gtk_button_new(); maximized_changed(window, NULL, maximize);
        gtk_header_bar_pack_end(GTK_HEADER_BAR(header), maximize);
        g_signal_connect_object(maximize, "clicked", G_CALLBACK(maximize_clicked), window, 0);
        g_signal_connect_object(window, "notify::maximized", G_CALLBACK(maximized_changed), maximize, 0);
        gtk_window_set_titlebar(window, header); gtk_window_set_resizable(window, TRUE);
        gtk_widget_add_css_class(GTK_WIDGET(window), "list-dialog");
    }
    if (g_strcmp0(layout->key, key) == 0) return;
    g_free(layout->key); layout->key = g_strdup(key); layout->restoring = TRUE;
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(window));
    Size *saved = g_hash_table_lookup(sizes(display), key); if (saved) { width = saved->width; height = saved->height; }
    GtkWindow *parent = gtk_window_get_transient_for(window);
    GdkSurface *surface = parent ? gtk_native_get_surface(GTK_NATIVE(parent)) : NULL;
    GdkMonitor *monitor = surface ? gdk_display_get_monitor_at_surface(display, surface) : NULL;
    GListModel *monitors = gdk_display_get_monitors(display);
    GdkMonitor *fallback = !monitor && g_list_model_get_n_items(monitors) ? g_list_model_get_item(monitors, 0) : NULL;
    if (monitor || fallback) {
        GdkRectangle bounds; gdk_monitor_get_geometry(monitor ? monitor : fallback, &bounds);
        width = MIN(width, MAX(1, bounds.width - 64));
        height = MIN(height, MAX(1, bounds.height - 96));
    }
    g_clear_object(&fallback); gtk_window_set_default_size(window, width, height);
}
GtkWidget *dialog_layout_help(const char *summary, const char *detail) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8), *label = gtk_label_new(summary);
    gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(label, summary);
    gtk_widget_set_hexpand(label, TRUE); gtk_widget_add_css_class(label, "muted"); gtk_box_append(GTK_BOX(box), label);
    GtkWidget *button = gtk_menu_button_new(); gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "help-about-symbolic");
    gtk_widget_set_valign(button, GTK_ALIGN_START); gtk_widget_set_tooltip_text(button, "More information");
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, "More information", -1);
    GtkWidget *popover = gtk_popover_new(), *text = gtk_label_new(detail); gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(text), 56); gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_widget_set_margin_start(text, 12); gtk_widget_set_margin_end(text, 12);
    gtk_widget_set_margin_top(text, 12); gtk_widget_set_margin_bottom(text, 12);
    gtk_popover_set_child(GTK_POPOVER(popover), text); gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
    gtk_box_append(GTK_BOX(box), button); return box;
}
