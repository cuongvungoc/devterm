#pragma once
#include <gio/gio.h>

typedef struct { gboolean confirm_close_sessions; } Preferences;
char *preferences_config_path(void);
/* Missing file uses defaults. Invalid files return NULL and remain untouched. */
Preferences *preferences_load(const char *path, GError **error);
gboolean preferences_save(const char *path, const Preferences *preferences, GError **error);
