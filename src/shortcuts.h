#pragma once
#include <gtk/gtk.h>

typedef enum {
    SC_RECONNECT, SC_DISCONNECT, SC_CLOSE_PANE, SC_SPLIT_RIGHT, SC_SPLIT_DOWN,
    SC_NEXT_PANE, SC_PREVIOUS_PANE, SC_PANE_LEFT, SC_PANE_RIGHT, SC_PANE_UP, SC_PANE_DOWN,
    SC_NEW_TAB, SC_CLOSE_TAB, SC_NEXT_TAB, SC_PREVIOUS_TAB, SC_RENAME_TAB, SC_TAB_LEFT, SC_TAB_RIGHT,
    SC_COPY, SC_PASTE, SC_COPY_ALL, SC_EXPORT, SC_FIND, SC_SESSIONS, SC_DRAFT,
    SC_QUICK_MANAGE, SC_QUICK_ADD, SC_SETTINGS, SC_COUNT
} ShortcutId;
typedef struct {
    const char *id, *label, *group, *default_key;
    gboolean terminal_only;
} ShortcutDefinition;
typedef struct { char *keys[SC_COUNT]; } ShortcutBindings;
const ShortcutDefinition *shortcut_definition(ShortcutId id);
ShortcutBindings *shortcut_bindings_new(void);
ShortcutBindings *shortcut_bindings_copy(const ShortcutBindings *bindings);
void shortcut_bindings_free(ShortcutBindings *bindings);
/* NULL/empty means deliberately unassigned. Invalid keys return an error. */
char *shortcut_normalize(const char *key, GError **error);
gboolean shortcut_bindings_set(ShortcutBindings *bindings, ShortcutId id, const char *key, GError **error);
int shortcut_bindings_conflict(const ShortcutBindings *bindings, ShortcutId except, const char *normalized);
gboolean shortcut_bindings_validate(const ShortcutBindings *bindings, GError **error);
char *shortcuts_config_path(void);
/* On failure, return NULL; never partially apply a file. */
ShortcutBindings *shortcuts_load(const char *path, GError **error);
gboolean shortcuts_save(const char *path, const ShortcutBindings *bindings, GError **error);
char *shortcut_label(const char *key);
char *shortcut_from_event(GdkEvent *event, GError **error);
int shortcut_match_event(const ShortcutBindings *bindings, GdkEvent *event);
