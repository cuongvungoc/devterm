#pragma once
#include <gtk/gtk.h>

GtkCssProvider *ui_style_install(GdkDisplay *display, gboolean dark);
void ui_style_apply(GtkCssProvider *provider, gboolean dark);
void ui_style_uninstall(GtkCssProvider *provider);
GtkWidget *ui_button(const char *icon, const char *label, const char *tooltip);
GtkWidget *ui_icon(const char *name);
