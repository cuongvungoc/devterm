#pragma once
#include "pane.h"

typedef struct SplitLayout SplitLayout;
typedef void (*LayoutStatus)(SplitLayout *layout, const char *message, gpointer user_data);

SplitLayout *split_layout_new(const char *directory, LayoutStatus status, gpointer user_data);
SplitLayout *split_layout_new_session(const SessionProfile *profile, const char *password, LayoutStatus status, gpointer user_data);
void split_layout_free(SplitLayout *layout);
GtkWidget *split_layout_widget(SplitLayout *layout);
TerminalPane *split_layout_active(SplitLayout *layout);
void split_layout_focus(SplitLayout *layout);
/* Freeze selection while GtkNotebook changes pages and assigns default focus. */
void split_layout_suspend_focus(SplitLayout *layout);
void split_layout_split(SplitLayout *layout, GtkOrientation orientation);
/* The caller closes the tab instead when it contains only one pane. */
void split_layout_close_active(SplitLayout *layout);
guint split_layout_pane_count(SplitLayout *layout);
guint split_layout_live_sessions(SplitLayout *layout);
