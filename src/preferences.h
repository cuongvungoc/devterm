#pragma once
#include <gio/gio.h>

typedef enum { PREFERENCES_APPEARANCE_DARK, PREFERENCES_APPEARANCE_LIGHT } PreferencesAppearance;
typedef struct {
    gboolean confirm_close_sessions;
    PreferencesAppearance appearance;
} Preferences;
char *preferences_config_path(void);
/* Missing file uses defaults. Invalid files return NULL and remain untouched. */
Preferences *preferences_load(const char *path, GError **error);
gboolean preferences_save(const char *path, const Preferences *preferences, GError **error);
