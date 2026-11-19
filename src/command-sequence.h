#pragma once
#include "commands.h"
#include "pane.h"

typedef struct CommandSequences CommandSequences;
/* All methods and pane cancellation signals run on the GTK thread. */
CommandSequences *command_sequences_new(void);
void command_sequences_free(CommandSequences *sequences);
/* Snapshots a sequence and locks its pane/session, without waiting for command
 * completion. Single commands use the same busy check. Focus moves once only. */
gboolean command_sequences_send(CommandSequences *sequences, TerminalPane *pane,
    const QuickCommand *command, GError **error);
gboolean command_sequences_busy(CommandSequences *sequences, TerminalPane *pane);
