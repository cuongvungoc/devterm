#include "command-sequence.h"

struct CommandSequences { GHashTable *runs; };
typedef struct {
    CommandSequences *owner;
    TerminalPane *pane;
    QuickCommand *command;
    GCancellable *session;
    gulong cancel_handler;
    guint source, next;
} SequenceRun;
static void run_free(gpointer data) {
    SequenceRun *run = data;
    if (run->source) g_source_remove(run->source);
    if (run->cancel_handler) g_signal_handler_disconnect(run->session, run->cancel_handler);
    g_object_unref(run->session); quick_command_free(run->command); pane_unref(run->pane); g_free(run);
}
CommandSequences *command_sequences_new(void) {
    CommandSequences *sequences = g_new0(CommandSequences, 1);
    sequences->runs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, run_free); return sequences;
}
void command_sequences_free(CommandSequences *sequences) {
    if (!sequences) return;
    g_hash_table_unref(sequences->runs); g_free(sequences);
}
gboolean command_sequences_busy(CommandSequences *sequences, TerminalPane *pane) { return g_hash_table_contains(sequences->runs, pane); }
static void cancelled(GCancellable *session, gpointer data) {
    (void)session; SequenceRun *run = data;
    char *message = g_strdup_printf("Sequence “%s” cancelled: pane session ended or changed; unsent commands discarded.", run->command->name);
    pane_report_status(run->pane, message); g_free(message);
    /* A plain signal handler can disconnect itself. All cancellation originates
     * on the GTK thread, so no blocking g_cancellable_disconnect is needed. */
    g_hash_table_remove(run->owner->runs, run->pane);
}
static gboolean send_next(gpointer data) {
    SequenceRun *run = data; run->source = 0;
    if (g_cancellable_is_cancelled(run->session) || !pane_is_running(run->pane)) {
        cancelled(run->session, run); return G_SOURCE_REMOVE;
    }
    QuickCommandStep *step = run->command->steps->pdata[run->next];
    pane_feed_input(run->pane, step->command, TRUE); run->next++;
    gboolean done = run->next == run->command->steps->len;
    char *message = done ? g_strdup_printf("Sequence “%s”: all %u commands sent.", run->command->name, run->next) :
        g_strdup_printf("Sending sequence “%s”: %u/%u commands sent; waiting %u ms.", run->command->name, run->next, run->command->steps->len, step->delay_ms);
    pane_report_status(run->pane, message); g_free(message);
    if (done) g_hash_table_remove(run->owner->runs, run->pane);
    else run->source = step->delay_ms ? g_timeout_add(step->delay_ms, send_next, run) : g_idle_add(send_next, run);
    return G_SOURCE_REMOVE;
}
gboolean command_sequences_send(CommandSequences *sequences, TerminalPane *pane, const QuickCommand *command, GError **error) {
    if (command_sequences_busy(sequences, pane)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "A sequence is still sending to this pane. Wait until all commands have been sent."); return FALSE;
    }
    if (!pane_is_running(pane)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Select a running pane; wait for reconnect to finish before sending commands."); return FALSE;
    }
    if (!quick_command_validate(command, error)) return FALSE;
    if (command->kind == QUICK_COMMAND) { pane_send(pane, command->command, command->send_enter); return TRUE; }
    SequenceRun *run = g_new0(SequenceRun, 1); run->owner = sequences; run->pane = pane_ref(pane);
    run->command = quick_command_copy(command); run->session = g_object_ref(pane_input_session(pane));
    g_hash_table_insert(sequences->runs, pane, run);
    run->cancel_handler = g_signal_connect(run->session, "cancelled", G_CALLBACK(cancelled), run);
    pane_focus(pane); send_next(run); return TRUE;
}
