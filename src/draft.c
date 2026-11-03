#include "draft.h"
#include "confirm-dialog.h"

struct Draft {
    int refs;
    GtkWidget *page, *panel, *view, *copy, *clear, *hide;
    GtkTextBuffer *buffer;
    GtkEventController *editor_focus, *terminal_focus;
    GtkWidget *clear_dialog;
    guint balance_tick;
    double terminal_ratio;
    gboolean closed, editor_selected, track_focus;
    DraftVisibilityChanged changed;
    gpointer data;
};

static Draft *draft_ref(Draft *d) { d->refs++; return d; }
static void draft_unref(Draft *d) {
    if (--d->refs) return;
    g_object_unref(d->page);
    g_free(d);
}
GtkWidget *draft_widget(Draft *d) { return d->page; }
gboolean draft_is_visible(Draft *d) { return d && !d->closed && gtk_widget_get_visible(d->panel); }
gboolean draft_has_text(Draft *d) { return d && !d->closed && gtk_text_buffer_get_char_count(d->buffer) > 0; }
gboolean draft_wants_focus(Draft *d) { return draft_is_visible(d) && d->editor_selected; }
void draft_focus(Draft *d) {
    if (draft_is_visible(d)) {
        d->editor_selected = TRUE;
        gtk_widget_grab_focus(d->view);
    }
}
static void editor_enter(GtkEventControllerFocus *controller, gpointer data) {
    (void)controller; Draft *d = data; if (d->track_focus) d->editor_selected = TRUE;
}
static void terminal_enter(GtkEventControllerFocus *controller, gpointer data) {
    (void)controller; Draft *d = data; if (d->track_focus) d->editor_selected = FALSE;
}
void draft_track_focus(Draft *d, gboolean enabled) { if (d) d->track_focus = enabled; }
static void cancel_balance(Draft *d) {
    if (d->balance_tick) { gtk_widget_remove_tick_callback(d->page, d->balance_tick); d->balance_tick = 0; }
}
static gboolean balance(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
    (void)clock; Draft *d = data;
    gboolean position_set; int available;
    g_object_get(widget, "position-set", &position_set, "max-position", &available, NULL);
    /* Honor a divider moved before initialization, and wait for allocation
     * after revealing the end child (the hidden page used the full height). */
    if (position_set) { d->balance_tick = 0; return G_SOURCE_REMOVE; }
    if (!gtk_widget_get_mapped(widget) || !gtk_widget_get_mapped(d->panel) ||
        gtk_widget_get_height(d->panel) <= 0 || available <= 1 || available == G_MAXINT) return G_SOURCE_CONTINUE;
    gtk_paned_set_position(GTK_PANED(widget), (int)(available * d->terminal_ratio + 0.5));
    return G_SOURCE_CONTINUE;
}
void draft_set_visible(Draft *d, gboolean visible) {
    if (!d || d->closed || draft_is_visible(d) == visible) return;
    if (!visible) {
        if (!d->balance_tick && gtk_widget_get_mapped(d->page)) {
            int first = gtk_widget_get_height(gtk_paned_get_start_child(GTK_PANED(d->page)));
            int second = gtk_widget_get_height(d->panel);
            if (first + second > 0) d->terminal_ratio = (double)first / (first + second);
        }
        cancel_balance(d);
        if (d->clear_dialog) gtk_window_destroy(GTK_WINDOW(d->clear_dialog));
        d->editor_selected = FALSE;
    }
    gtk_widget_set_visible(d->panel, visible);
    if (visible) {
        gtk_paned_set_position(GTK_PANED(d->page), -1);
        d->balance_tick = gtk_widget_add_tick_callback(d->page, balance, d, NULL);
        draft_focus(d);
    }
    if (d->changed) d->changed(visible, d->data);
}
static void hide_clicked(GtkButton *button, gpointer data) { (void)button; draft_set_visible(data, FALSE); }
static void buffer_changed(GtkTextBuffer *buffer, gpointer data) {
    (void)buffer; Draft *d = data;
    gboolean has_text = draft_has_text(d);
    gtk_widget_set_sensitive(d->copy, has_text);
    gtk_widget_set_sensitive(d->clear, has_text && !d->clear_dialog);
}
static void copy_clicked(GtkButton *button, gpointer data) {
    (void)button; Draft *d = data;
    GtkTextIter start, end;
    if (!gtk_text_buffer_get_selection_bounds(d->buffer, &start, &end)) gtk_text_buffer_get_bounds(d->buffer, &start, &end);
    char *text = gtk_text_buffer_get_text(d->buffer, &start, &end, TRUE);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(d->view), text);
    g_free(text); draft_focus(d);
}
static void clear_response(gboolean accepted, gpointer data) {
    Draft *d = data; d->clear_dialog = NULL;
    if (!d->closed) {
        if (accepted) {
            GtkTextIter start, end; gtk_text_buffer_get_bounds(d->buffer, &start, &end);
            /* set_text() clears undo history; a single user action is undoable. */
            gtk_text_buffer_begin_user_action(d->buffer);
            gtk_text_buffer_delete(d->buffer, &start, &end);
            gtk_text_buffer_end_user_action(d->buffer);
        }
        buffer_changed(d->buffer, d); draft_focus(d);
    }
}
static void clear_clicked(GtkButton *button, gpointer data) {
    (void)button; Draft *d = data;
    if (d->clear_dialog || !draft_has_text(d)) return;
    GtkRoot *root = gtk_widget_get_root(d->page);
    if (!GTK_IS_WINDOW(root)) return;
    d->clear_dialog = confirm_dialog_show(GTK_WINDOW(root), "Clear this draft?",
        "This removes all text from this tab's draft. You can restore it with Undo (Ctrl+Z).",
        "Clear Draft", clear_response, draft_ref(d), (GDestroyNotify)draft_unref);
    buffer_changed(d->buffer, d);
}
Draft *draft_new(GtkWidget *terminal_area, const char *title, DraftVisibilityChanged changed, gpointer data) {
    Draft *d = g_new0(Draft, 1); d->refs = 1; d->terminal_ratio = 0.7; d->track_focus = TRUE; d->changed = changed; d->data = data;
    d->page = gtk_paned_new(GTK_ORIENTATION_VERTICAL); g_object_ref_sink(d->page);
    gtk_widget_set_hexpand(d->page, TRUE); gtk_widget_set_vexpand(d->page, TRUE);
    gtk_paned_set_resize_start_child(GTK_PANED(d->page), TRUE); gtk_paned_set_resize_end_child(GTK_PANED(d->page), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(d->page), TRUE); gtk_paned_set_shrink_end_child(GTK_PANED(d->page), TRUE);
    gtk_paned_set_wide_handle(GTK_PANED(d->page), TRUE);
    gtk_paned_set_start_child(GTK_PANED(d->page), terminal_area);
    d->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(d->panel, "draft-panel");
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(header, "draft-header");
    gtk_widget_set_margin_start(header, 8); gtk_widget_set_margin_end(header, 8); gtk_widget_set_margin_top(header, 4);
    char *caption = g_strdup_printf("Draft — %s", title);
    GtkWidget *label = gtk_label_new(caption); g_free(caption);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_tooltip_text(label, "Scratch space for this tab. Copy text to use it elsewhere.");
    gtk_widget_add_css_class(label, "section-title");
    gtk_widget_set_hexpand(label, TRUE); gtk_box_append(GTK_BOX(header), label);
    d->copy = gtk_button_new_with_label("Copy"); gtk_widget_set_tooltip_text(d->copy, "Copy selected text, or the entire draft when nothing is selected");
    d->clear = gtk_button_new_with_label("Clear");
    gtk_widget_add_css_class(d->clear, "danger");
    d->hide = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_tooltip_text(d->hide, "Hide Draft — keep text");
    gtk_accessible_update_property(GTK_ACCESSIBLE(d->hide), GTK_ACCESSIBLE_PROPERTY_LABEL, "Hide Draft", -1);
    gtk_widget_set_focus_on_click(d->copy, FALSE); gtk_widget_set_focus_on_click(d->clear, FALSE); gtk_widget_set_focus_on_click(d->hide, FALSE);
    gtk_box_append(GTK_BOX(header), d->copy); gtk_box_append(GTK_BOX(header), d->clear); gtk_box_append(GTK_BOX(header), d->hide);
    gtk_box_append(GTK_BOX(d->panel), header);
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    d->view = gtk_text_view_new(); d->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->view));
    gtk_widget_add_css_class(d->view, "draft-editor");
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(d->view), TRUE); gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(d->view), 8); gtk_text_view_set_right_margin(GTK_TEXT_VIEW(d->view), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(d->view), 6); gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(d->view), 6);
    gtk_text_buffer_set_enable_undo(d->buffer, TRUE);
    gtk_accessible_update_property(GTK_ACCESSIBLE(d->view), GTK_ACCESSIBLE_PROPERTY_LABEL, "Draft text", -1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), d->view); gtk_box_append(GTK_BOX(d->panel), scroll);
    gtk_paned_set_end_child(GTK_PANED(d->page), d->panel); gtk_widget_set_visible(d->panel, FALSE);
    d->editor_focus = gtk_event_controller_focus_new(); g_signal_connect(d->editor_focus, "enter", G_CALLBACK(editor_enter), d);
    gtk_widget_add_controller(d->view, d->editor_focus);
    d->terminal_focus = gtk_event_controller_focus_new(); g_signal_connect(d->terminal_focus, "enter", G_CALLBACK(terminal_enter), d);
    gtk_widget_add_controller(terminal_area, d->terminal_focus);
    g_signal_connect(d->copy, "clicked", G_CALLBACK(copy_clicked), d); g_signal_connect(d->clear, "clicked", G_CALLBACK(clear_clicked), d);
    g_signal_connect(d->hide, "clicked", G_CALLBACK(hide_clicked), d); g_signal_connect(d->buffer, "changed", G_CALLBACK(buffer_changed), d);
    buffer_changed(d->buffer, d); return d;
}
void draft_free(Draft *d) {
    if (!d || d->closed) return;
    d->closed = TRUE; d->changed = NULL; d->data = NULL;
    cancel_balance(d); if (d->clear_dialog) gtk_window_destroy(GTK_WINDOW(d->clear_dialog));
    g_signal_handlers_disconnect_by_data(d->editor_focus, d); g_signal_handlers_disconnect_by_data(d->terminal_focus, d);
    g_signal_handlers_disconnect_by_data(d->buffer, d); g_signal_handlers_disconnect_by_data(d->copy, d);
    g_signal_handlers_disconnect_by_data(d->clear, d); g_signal_handlers_disconnect_by_data(d->hide, d);
    draft_unref(d);
}
