#pragma once
#include <gio/gio.h>
#include <json-glib/json-glib.h>

typedef enum { SESSION_SSH, SESSION_TELNET, SESSION_SERIAL } SessionType;
typedef struct {
    char *id, *name, *host, *username, *identity, *device;
    char *login_prompt, *password_prompt, *ready_prompt;
    SessionType type;
    int port, baud, data_bits, stop_bits, timeout;
    char *parity, *flow;
    gboolean password_auth, remember, auto_login, send_initial_enter;
} SessionProfile;
SessionProfile *session_profile_new(void);
SessionProfile *session_profile_copy(const SessionProfile *profile);
void session_profile_free(gpointer profile);
gboolean session_profile_validate(const SessionProfile *profile, GError **error);
char *sessions_config_path(void);
GPtrArray *sessions_load(const char *path, GError **error);
gboolean sessions_save(const char *path, GPtrArray *profiles, GError **error);
char *session_endpoint(const SessionProfile *profile);
gboolean session_needs_password(const SessionProfile *profile);
void session_secret_free(char *password);
/* Linux anonymous RAM descriptor; no secret appears in argv/env/files. */
int session_launch_fd(const SessionProfile *profile, const char *password, GError **error);
SessionProfile *session_read_launch(int fd, char **password, GError **error);
