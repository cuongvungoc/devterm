#pragma once
#include <gtk/gtk.h>

typedef struct Draft Draft;
typedef void (*DraftVisibilityChanged)(gboolean visible, gpointer data);

/* The page wraps terminal_area in a vertical split; text stays in RAM only. */
Draft *draft_new(GtkWidget *terminal_area, const char *title, DraftVisibilityChanged changed, gpointer data);
void draft_free(Draft *draft);
GtkWidget *draft_widget(Draft *draft);
gboolean draft_is_visible(Draft *draft);
void draft_set_visible(Draft *draft, gboolean visible);
gboolean draft_has_text(Draft *draft);
gboolean draft_wants_focus(Draft *draft);
void draft_focus(Draft *draft);
/* Ignore GtkNotebook's temporary default focus while changing pages. */
void draft_track_focus(Draft *draft, gboolean enabled);
