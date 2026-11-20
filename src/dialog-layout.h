#pragma once
#include <gtk/gtk.h>

/* Logical pixels, clamped to the parent's monitor. Size memory belongs to the
 * display and lasts only for this process; no user configuration is written. */
void dialog_layout_apply(GtkWindow *window, const char *key, int width, int height);
/* Responsive pair measured before painting. Thresholds use allocated content
 * width, independent of window decorations; first_percent controls the wide column. */
GtkWidget *dialog_layout_pair(GtkWidget *first, GtkWidget *second, int narrow, int wide, int first_percent);
GtkWidget *dialog_layout_help(const char *summary, const char *detail);
