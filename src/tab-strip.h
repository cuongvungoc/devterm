#pragma once
#include <gtk/gtk.h>

typedef struct TabStrip TabStrip;
typedef enum { TAB_STRIP_RENAMED, TAB_STRIP_CLOSE, TAB_STRIP_FOCUS } TabStripAction;
typedef void (*TabStripNotify)(guint id, TabStripAction action, const char *title, gpointer data);
/* The window must remain valid until tab_strip_free(). Pages retain their
 * original widgets/PTYs; tab IDs identify actions independently of order. */
TabStrip *tab_strip_new(GtkWindow *window, GtkNotebook *notebook, TabStripNotify notify, gpointer data);
int tab_strip_append(TabStrip *strip, guint id, GtkWidget *page, const char *title, const char *icon);
void tab_strip_remove(TabStrip *strip, guint id);
/* Prevent notebook's temporary focus from replacing the remembered pane/Draft. */
gboolean tab_strip_busy(TabStrip *strip);
void tab_strip_free(TabStrip *strip);
