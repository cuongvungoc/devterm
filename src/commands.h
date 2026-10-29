#pragma once
#include <glib.h>

typedef struct {
    char *id;
    char *name;
    char *command;
    gboolean send_enter;
} QuickCommand;

QuickCommand *quick_command_new(const char *id, const char *name, const char *command, gboolean send_enter);
void quick_command_free(gpointer command);
char *commands_config_path(void);
GPtrArray *commands_load(const char *path, GError **error);
gboolean commands_save(const char *path, GPtrArray *commands, GError **error);
