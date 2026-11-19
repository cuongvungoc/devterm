#pragma once
#include <glib.h>

#define QUICK_SEQUENCE_MAX_STEPS 256
#define QUICK_SEQUENCE_MAX_DELAY_MS 60000

typedef enum { QUICK_COMMAND, QUICK_SEQUENCE } QuickCommandKind;
typedef struct { char *command; guint delay_ms; } QuickCommandStep;
typedef struct {
    char *id;
    char *name;
    char *command; /* Single command only; NULL for a sequence. */
    gboolean send_enter;
    QuickCommandKind kind;
    GPtrArray *steps; /* QuickCommandStep*, sequence only. */
} QuickCommand;

QuickCommand *quick_command_new(const char *id, const char *name, const char *command, gboolean send_enter);
QuickCommand *quick_sequence_new(const char *id, const char *name);
QuickCommandStep *quick_command_step_new(const char *command, guint delay_ms);
QuickCommand *quick_command_copy(const QuickCommand *command);
gboolean quick_command_validate(const QuickCommand *command, GError **error);
char *quick_command_describe(const QuickCommand *command);
void quick_command_free(gpointer command);
char *commands_config_path(void);
GPtrArray *commands_load(const char *path, GError **error);
/* Writes v2 atomically. Before upgrading an existing v1 file, backs up its exact
 * bytes to commands.json.v1-<uuid>.bak. Invalid existing files are preserved. */
gboolean commands_save(const char *path, GPtrArray *commands, GError **error);
