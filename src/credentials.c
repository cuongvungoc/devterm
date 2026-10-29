#include "credentials.h"
#include <libsecret/secret.h>
static const SecretSchema schema = {
    "com.example.DevTerm.Session", SECRET_SCHEMA_NONE,
    {{"profile", SECRET_SCHEMA_ATTRIBUTE_STRING}, {"endpoint", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL, 0}},
    0, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
typedef struct { char *id, *endpoint, *name, *password; int operation; } Credential;
static void credential_free(gpointer data) {
    Credential *c = data; g_free(c->id); g_free(c->endpoint); g_free(c->name); session_secret_free(c->password); g_free(c);
}
static void worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
    (void)source; Credential *c = data; GError *error = NULL;
    if (c->operation == 0) {
        char *secret = secret_password_lookup_sync(&schema, cancel, &error, "profile", c->id, "endpoint", c->endpoint, NULL);
        if (error) g_task_return_error(task, error);
        else { char *password = g_strdup(secret); secret_password_free(secret); g_task_return_pointer(task, password, (GDestroyNotify)session_secret_free); }
    } else {
        gboolean ok = c->operation == 1 ?
            secret_password_store_sync(&schema, SECRET_COLLECTION_DEFAULT, c->name, c->password, cancel, &error, "profile", c->id, "endpoint", c->endpoint, NULL) :
            secret_password_clear_sync(&schema, cancel, &error, "profile", c->id, "endpoint", c->endpoint, NULL);
        if (error) g_task_return_error(task, error);
        else g_task_return_boolean(task, ok || c->operation == 2);
    }
}
static void queued_worker(gpointer data, gpointer unused) {
    (void)unused; GTask *task = data;
    worker(task, NULL, g_task_get_task_data(task), g_task_get_cancellable(task));
    g_object_unref(task);
}
static void start(const SessionProfile *p, const char *password, int operation, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
    Credential *c = g_new0(Credential, 1); c->id = g_strdup(p->id); c->endpoint = session_endpoint(p);
    c->name = g_strdup_printf("DevTerm: %s", p->name); c->password = g_strdup(password); c->operation = operation;
    GTask *task = g_task_new(NULL, cancel, callback, data); g_task_set_task_data(task, c, credential_free);
    /* Preserve store/edit/delete order: a late password store must not undo
     * a later deletion or replace a newer password. GTK remains asynchronous. */
    static gsize initialized;
    static GThreadPool *queue;
    if (g_once_init_enter(&initialized)) {
        queue = g_thread_pool_new(queued_worker, NULL, 1, FALSE, NULL);
        g_once_init_leave(&initialized, 1);
    }
    g_thread_pool_push(queue, task, NULL);
}
void credential_lookup(const SessionProfile *p, GCancellable *c, GAsyncReadyCallback cb, gpointer data) { start(p, NULL, 0, c, cb, data); }
char *credential_lookup_finish(GAsyncResult *result, GError **error) { return g_task_propagate_pointer(G_TASK(result), error); }
void credential_store(const SessionProfile *p, const char *s, GCancellable *c, GAsyncReadyCallback cb, gpointer data) { start(p, s, 1, c, cb, data); }
void credential_clear(const SessionProfile *p, GCancellable *c, GAsyncReadyCallback cb, gpointer data) { start(p, NULL, 2, c, cb, data); }
gboolean credential_change_finish(GAsyncResult *result, GError **error) { return g_task_propagate_boolean(G_TASK(result), error); }
