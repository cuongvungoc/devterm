#include "confirm-dialog.h"

typedef struct {
    GtkWidget *window;
    gboolean accepted;
    ConfirmResponse response;
    gpointer data;
    GDestroyNotify destroy_data;
} Confirmation;
static void closed(GtkWidget *window, gpointer data) {
    (void)window; Confirmation *c = data;
    c->response(c->accepted, c->data);
    if (c->destroy_data) c->destroy_data(c->data);
    g_free(c);
}
static void cancel_clicked(GtkButton *button, gpointer data) { (void)button; gtk_window_destroy(GTK_WINDOW(data)); }
static void accept_clicked(GtkButton *button, gpointer data) {
    (void)button; Confirmation *c = data; c->accepted = TRUE; gtk_window_destroy(GTK_WINDOW(c->window));
}
static gboolean on_key(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
    (void)controller; (void)keycode; (void)state;
    if (keyval != GDK_KEY_Escape) return FALSE;
    gtk_window_destroy(GTK_WINDOW(data)); return TRUE;
}
GtkWidget *confirm_dialog_show(GtkWindow *parent, const char *title, const char *detail,
    const char *accept_label, ConfirmResponse response, gpointer data, GDestroyNotify destroy_data) {
    Confirmation *c = g_new0(Confirmation, 1);
    c->window = gtk_window_new(); c->response = response; c->data = data; c->destroy_data = destroy_data;
    gtk_widget_add_css_class(c->window, "devterm-dialog");
    gtk_window_set_title(GTK_WINDOW(c->window), title); gtk_window_set_transient_for(GTK_WINDOW(c->window), parent);
    gtk_window_set_modal(GTK_WINDOW(c->window), TRUE); gtk_window_set_destroy_with_parent(GTK_WINDOW(c->window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(c->window), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(box, 20); gtk_widget_set_margin_end(box, 20);
    gtk_widget_set_margin_top(box, 20); gtk_widget_set_margin_bottom(box, 20);
    gtk_window_set_child(GTK_WINDOW(c->window), box);
    GtkWidget *label = gtk_label_new(detail); gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 65); gtk_widget_set_size_request(label, 400, -1);
    gtk_label_set_xalign(GTK_LABEL(label), 0); gtk_box_append(GTK_BOX(box), label);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel"), *accept = gtk_button_new_with_label(accept_label);
    gtk_widget_add_css_class(accept, "destructive-action");
    gtk_box_append(GTK_BOX(buttons), cancel); gtk_box_append(GTK_BOX(buttons), accept); gtk_box_append(GTK_BOX(box), buttons);
    gtk_window_set_default_widget(GTK_WINDOW(c->window), cancel);
    g_signal_connect(cancel, "clicked", G_CALLBACK(cancel_clicked), c->window);
    g_signal_connect(accept, "clicked", G_CALLBACK(accept_clicked), c);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), c->window); gtk_widget_add_controller(c->window, keys);
    g_signal_connect(c->window, "destroy", G_CALLBACK(closed), c);
    gtk_window_present(GTK_WINDOW(c->window)); gtk_widget_grab_focus(cancel);
    return c->window;
}
