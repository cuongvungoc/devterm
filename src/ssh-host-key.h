#pragma once
#include "sessions.h"

#define SSH_HOST_KEY_NOTICE "DEVTERM-HOST-KEY-CHANGED-1\n"
#define SSH_HOST_KEY_NOTICE_LIMIT 32768

typedef struct SshHostKeyReview SshHostKeyReview;
struct SshHostKeyReview {
    gint refs;
    char *endpoint, *path, *old_fingerprints, *fingerprint, *algorithm, *backup;
    /* Immutable snapshot: confirmation must apply to exactly these bytes. */
    char *original, *replacement, *etag;
};
SshHostKeyReview *ssh_host_key_ref(SshHostKeyReview *review);
void ssh_host_key_unref(SshHostKeyReview *review);
/* Blocking operations, for worker threads/tests only. Never authenticate. */
SshHostKeyReview *ssh_host_key_prepare(const SessionProfile *profile, const char *diagnostics,
                                      GCancellable *cancel, GError **error);
gboolean ssh_host_key_replace(SshHostKeyReview *review, GCancellable *cancel, GError **error);
void ssh_host_key_prepare_async(const SessionProfile *profile, const char *diagnostics, GCancellable *cancel,
                               GAsyncReadyCallback callback, gpointer data);
SshHostKeyReview *ssh_host_key_prepare_finish(GAsyncResult *result, GError **error);
void ssh_host_key_replace_async(SshHostKeyReview *review, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data);
gboolean ssh_host_key_replace_finish(GAsyncResult *result, GError **error);
