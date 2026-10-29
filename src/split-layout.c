#include "split-layout.h"

typedef struct LayoutNode LayoutNode;
struct LayoutNode {
    LayoutNode *parent, *first, *second;
    TerminalPane *pane;
    GtkWidget *widget;
    guint balance_tick;
};
struct SplitLayout {
    GtkWidget *widget;
    LayoutNode *tree;
    TerminalPane *active;
    gboolean track_focus;
    LayoutStatus status;
    gpointer user_data;
};

static void set_active(SplitLayout *layout, TerminalPane *pane) {
    if (layout->active) pane_set_active(layout->active, FALSE);
    layout->active = pane;
    pane_set_active(pane, TRUE);
}
static void selected(TerminalPane *pane, const char *message, gpointer data) {
    (void)message;
    SplitLayout *layout = data;
    if (layout->track_focus) set_active(layout, pane);
}
static void pane_status(TerminalPane *pane, const char *message, gpointer data) {
    (void)pane;
    SplitLayout *layout = data;
    if (layout->status) layout->status(layout, message, layout->user_data);
}
static LayoutNode *leaf_new(SplitLayout *layout, const char *directory) {
    LayoutNode *node = g_new0(LayoutNode, 1);
    node->pane = pane_new(directory, selected, pane_status, layout);
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
static void detach(GtkWidget *widget) {
    GtkWidget *parent = gtk_widget_get_parent(widget);
    if (!parent) return;
    GtkRoot *root = gtk_widget_get_root(widget);
    GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
    if (focus && (focus == widget || gtk_widget_is_ancestor(focus, widget))) gtk_root_set_focus(root, NULL);
    if (GTK_IS_PANED(parent)) {
        if (is_start(parent, widget)) gtk_paned_set_start_child(GTK_PANED(parent), NULL);
        else gtk_paned_set_end_child(GTK_PANED(parent), NULL);
    } else gtk_box_remove(GTK_BOX(parent), widget);
}
static gboolean balance_split(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)clock;
    LayoutNode *node = data;
    /* A parent centered in this frame has not allocated its children yet.
     * Initialize outer splits first, each followed by one allocation pass. */
    for (LayoutNode *parent = node->parent; parent; parent = parent->parent)
        if (parent->balance_tick) return G_SOURCE_CONTINUE;
    gboolean position_set;
    int available;
    g_object_get(widget, "position-set", &position_set, "max-position", &available, NULL);
    /* Respect a divider already moved by the user before initialization. */
    if (!position_set) {
        GtkOrientation orientation = gtk_orientable_get_orientation(GTK_ORIENTABLE(widget));
        int size = orientation == GTK_ORIENTATION_HORIZONTAL ? gtk_widget_get_width(widget) : gtk_widget_get_height(widget);
        if (!gtk_widget_get_mapped(widget) || size <= 1 || available == G_MAXINT) return G_SOURCE_CONTINUE;
        /* Both children may shrink, so max-position is the allocated span
         * minus the separator. Wait for that first allocation, then center
         * once; GTK preserves this ratio on subsequent window resizes. */
        gtk_paned_set_position(GTK_PANED(widget), available / 2);
        return G_SOURCE_CONTINUE;
    }
    node->balance_tick = 0;
    return G_SOURCE_REMOVE;
}
static void cancel_balance(LayoutNode *node) {
    if (node->balance_tick) {
        gtk_widget_remove_tick_callback(node->widget, node->balance_tick);
        node->balance_tick = 0;
    }
}
static void preserve_ancestors(LayoutNode *node) {
    for (; node; node = node->parent) {
        /* Unallocated splits still have their own pending 50/50 initializer. */
        if (!node->balance_tick && gtk_widget_get_width(node->widget) > 0 && gtk_widget_get_height(node->widget) > 0)
            gtk_paned_set_position(GTK_PANED(node->widget), gtk_paned_get_position(GTK_PANED(node->widget)));
    }
}
static void free_node(LayoutNode *node) {
    if (!node) return;
    cancel_balance(node);
    detach(node->widget);
    if (node->pane) pane_close(node->pane);
    else {
        free_node(node->first);
        free_node(node->second);
        g_object_unref(node->widget);
    }
    g_free(node);
}

SplitLayout *split_layout_new(const char *directory, LayoutStatus status, gpointer user_data) {
    SplitLayout *layout = g_new0(SplitLayout, 1);
    layout->status = status;
    layout->user_data = user_data;
    layout->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref_sink(layout->widget);
    gtk_widget_set_hexpand(layout->widget, TRUE);
    gtk_widget_set_vexpand(layout->widget, TRUE);
    layout->tree = leaf_new(layout, directory);
    gtk_box_append(GTK_BOX(layout->widget), layout->tree->widget);
    set_active(layout, layout->tree->pane);
    return layout;
}
SplitLayout *split_layout_new_session(const SessionProfile *profile, const char *password, LayoutStatus status, gpointer data) {
    SplitLayout *layout = g_new0(SplitLayout, 1);
    layout->status = status; layout->user_data = data;
    layout->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(layout->widget);
    gtk_widget_set_hexpand(layout->widget, TRUE); gtk_widget_set_vexpand(layout->widget, TRUE);
    LayoutNode *node = g_new0(LayoutNode, 1);
    node->pane = pane_new_session(profile, password, selected, pane_status, layout);
    node->widget = pane_widget(node->pane); layout->tree = node;
    gtk_box_append(GTK_BOX(layout->widget), node->widget); set_active(layout, node->pane);
    return layout;
}
void split_layout_free(SplitLayout *layout) {
    if (!layout) return;
    layout->track_focus = FALSE;
    layout->active = NULL;
    free_node(layout->tree);
    g_object_unref(layout->widget);
    g_free(layout);
}
GtkWidget *split_layout_widget(SplitLayout *layout) { return layout->widget; }
TerminalPane *split_layout_active(SplitLayout *layout) { return layout ? layout->active : NULL; }
void split_layout_focus(SplitLayout *layout) {
    if (!layout) return;
    if (layout->active) pane_focus(layout->active);
    layout->track_focus = TRUE;
}
void split_layout_suspend_focus(SplitLayout *layout) { if (layout) layout->track_focus = FALSE; }
void split_layout_split(SplitLayout *layout, GtkOrientation orientation) {
    if (!layout || !layout->active) return;
    LayoutNode *node = find_pane(layout->tree, layout->active);
    if (!node) return;
    char *directory = pane_dup_working_directory(layout->active);
    LayoutNode *old = g_new0(LayoutNode, 1);
    old->pane = node->pane;
    old->widget = node->widget;
    LayoutNode *fresh = leaf_new(layout, directory);
    g_free(directory);
    GtkWidget *parent = gtk_widget_get_parent(node->widget);
    gboolean start = is_start(parent, node->widget);
    preserve_ancestors(node->parent);
    detach(node->widget);
    node->pane = NULL;
    node->widget = gtk_paned_new(orientation);
    g_object_ref_sink(node->widget);
    node->first = old; node->second = fresh;
    old->parent = node; fresh->parent = node;
    gtk_paned_set_start_child(GTK_PANED(node->widget), old->widget);
    gtk_paned_set_end_child(GTK_PANED(node->widget), fresh->widget);
    gtk_paned_set_resize_start_child(GTK_PANED(node->widget), TRUE);
    gtk_paned_set_resize_end_child(GTK_PANED(node->widget), TRUE);
    attach_at(parent, start, node->widget);
    node->balance_tick = gtk_widget_add_tick_callback(node->widget, balance_split, node, NULL);
    set_active(layout, fresh->pane);
    pane_focus(fresh->pane);
}
void split_layout_close_active(SplitLayout *layout) {
    if (!layout || !layout->active) return;
    LayoutNode *node = find_pane(layout->tree, layout->active);
    if (!node || !node->parent) return;
    LayoutNode *branch = node->parent;
    LayoutNode *other = branch->first == node ? branch->second : branch->first;
    LayoutNode *grand = branch->parent;
    GtkWidget *parent = gtk_widget_get_parent(branch->widget);
    gboolean start = is_start(parent, branch->widget);
    preserve_ancestors(grand);
    cancel_balance(branch);
    pane_set_active(node->pane, FALSE);
    layout->active = NULL;
    detach(other->widget);
    detach(node->widget);
    detach(branch->widget);
    attach_at(parent, start, other->widget);
    other->parent = grand;
    if (grand) {
        if (grand->first == branch) grand->first = other;
        else grand->second = other;
    } else layout->tree = other;
    pane_close(node->pane);
    g_object_unref(branch->widget);
    g_free(node); g_free(branch);
    set_active(layout, first_leaf(other)->pane);
    split_layout_focus(layout);
}
static guint count(LayoutNode *node, gboolean live_only) {
    if (!node) return 0;
    if (node->pane) return live_only ? pane_has_live_session(node->pane) : 1;
    return count(node->first, live_only) + count(node->second, live_only);
}
guint split_layout_pane_count(SplitLayout *layout) { return layout ? count(layout->tree, FALSE) : 0; }
guint split_layout_live_sessions(SplitLayout *layout) { return layout ? count(layout->tree, TRUE) : 0; }
