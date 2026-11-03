#include "ui-style.h"

static const char *light_palette =
    "@define-color dt_bg #F2F5F9; @define-color dt_surface #FFFFFF;"
    "@define-color dt_fg #172033; @define-color dt_muted #526176;"
    "@define-color dt_border #CFD8E3; @define-color dt_hover #E8EEF5;"
    "@define-color dt_accent #0F766E; @define-color dt_on_accent #FFFFFF;"
    "@define-color dt_soft #DCF2EF; @define-color dt_danger #B42332;"
    "@define-color dt_disabled #667085;";
static const char *dark_palette =
    "@define-color dt_bg #111722; @define-color dt_surface #1A2331;"
    "@define-color dt_fg #E4EBF3; @define-color dt_muted #A0AEC1;"
    "@define-color dt_border #364356; @define-color dt_hover #29374A;"
    "@define-color dt_accent #5DD6C0; @define-color dt_on_accent #10231F;"
    "@define-color dt_soft #203E40; @define-color dt_danger #FDA4AF;"
    "@define-color dt_disabled #8191A7;";

void ui_style_apply(GtkCssProvider *provider, gboolean dark) {
    GBytes *resource = g_resources_lookup_data("/com/example/DevTerm/style.css", G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
    g_assert_nonnull(resource);
    gsize size; const char *css = g_bytes_get_data(resource, &size);
    char *sheet = g_strconcat(dark ? dark_palette : light_palette, "\n", NULL);
    GString *full = g_string_new(sheet); g_string_append_len(full, css, size); g_free(sheet);
    gtk_css_provider_load_from_data(provider, full->str, full->len);
    g_string_free(full, TRUE); g_bytes_unref(resource);
    /* This setting belongs to this application, never the desktop settings. */
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", dark, NULL);
}
GtkCssProvider *ui_style_install(GdkDisplay *display, gboolean dark) {
    GtkCssProvider *provider = gtk_css_provider_new(); ui_style_apply(provider, dark);
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    return provider;
}
void ui_style_uninstall(GtkCssProvider *provider) {
    if (!provider) return;
    gtk_style_context_remove_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider));
    g_object_unref(provider);
}
GtkWidget *ui_icon(const char *name) {
    GtkWidget *image = gtk_image_new_from_icon_name(name);
    gtk_accessible_update_state(GTK_ACCESSIBLE(image), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
    return image;
}
GtkWidget *ui_button(const char *icon, const char *label, const char *tooltip) {
    GtkWidget *button = gtk_button_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    if (icon) gtk_box_append(GTK_BOX(box), ui_icon(icon));
    if (label) gtk_box_append(GTK_BOX(box), gtk_label_new(label));
    gtk_button_set_child(GTK_BUTTON(button), box); gtk_widget_set_focus_on_click(button, FALSE);
    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label ? label : tooltip, -1);
    return button;
}
