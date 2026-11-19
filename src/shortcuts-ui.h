#pragma once
#include "shortcuts.h"
#define TYPE_SHORTCUT_SETTINGS (shortcut_settings_get_type())
G_DECLARE_FINAL_TYPE(ShortcutSettings, shortcut_settings, SHORTCUT, SETTINGS, GObject)
/* Application-owned settings, shared by all windows. Emits "changed" after save. */
ShortcutSettings *shortcut_settings_get(GtkApplication *application);
const ShortcutBindings *shortcut_settings_bindings(ShortcutSettings *settings);
const char *shortcut_settings_error(ShortcutSettings *settings);
gboolean shortcut_settings_apply(ShortcutSettings *settings, const ShortcutBindings *bindings, gboolean recover, GError **error);
void shortcut_settings_show(ShortcutSettings *settings, GtkWindow *parent);
void shortcut_settings_tooltip(ShortcutSettings *settings, GtkWidget *widget, ShortcutId id, const char *description);
