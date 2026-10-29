#define _GNU_SOURCE
#include "sessions.h"
#include <errno.h>
#include <glib/gstdio.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

void session_secret_free(char *s) { if (s) { explicit_bzero(s, strlen(s)); g_free(s); } }
SessionProfile *session_profile_new(void) {
    SessionProfile *p = g_new0(SessionProfile, 1);
    p->id = g_uuid_string_random(); p->name = g_strdup(""); p->host = g_strdup("");
    p->username = g_strdup(g_get_user_name()); p->identity = g_strdup(""); p->device = g_strdup("");
    p->login_prompt = g_strdup("(?i)(login|username)\\s*:\\s*$");
    p->password_prompt = g_strdup("(?i)password\\s*:\\s*$");
    p->ready_prompt = g_strdup("[^\\r\\n]+[>#$]\\s*$");
    p->port = 22; p->baud = 115200; p->data_bits = 8; p->stop_bits = 1; p->timeout = 30;
    p->parity = g_strdup("none"); p->flow = g_strdup("none"); p->password_auth = TRUE;
    return p;
}
SessionProfile *session_profile_copy(const SessionProfile *p) {
    SessionProfile *n = g_memdup2(p, sizeof *p);
#define COPY(field) n->field = g_strdup(p->field)
    COPY(id); COPY(name); COPY(host); COPY(username); COPY(identity); COPY(device);
    COPY(login_prompt); COPY(password_prompt); COPY(ready_prompt); COPY(parity); COPY(flow);
#undef COPY
    return n;
}
void session_profile_free(gpointer data) {
    SessionProfile *p = data; if (!p) return;
    g_free(p->id); g_free(p->name); g_free(p->host); g_free(p->username); g_free(p->identity); g_free(p->device);
    g_free(p->login_prompt); g_free(p->password_prompt); g_free(p->ready_prompt); g_free(p->parity); g_free(p->flow); g_free(p);
}
static gboolean plain(const char *s, gsize max) {
    if (!s || strlen(s) > max || !g_utf8_validate(s, -1, NULL)) return FALSE;
    for (const char *c = s; *c; c++) if ((guchar)*c < 32 || *c == 127) return FALSE;
    return TRUE;
}
static gboolean valid_prompt(const char *s, gboolean empty) {
    if (!plain(s, 512) || (!empty && !*s)) return FALSE;
    if (!*s) return TRUE;
    GError *error = NULL; GRegex *regex = g_regex_new(s, G_REGEX_OPTIMIZE, 0, &error);
    if (!regex) { g_clear_error(&error); return FALSE; }
    gboolean ok = !g_regex_match(regex, "", 0, NULL); g_regex_unref(regex); return ok;
}
gboolean session_profile_validate(const SessionProfile *p, GError **error) {
    if (!p || !p->id || !g_uuid_string_is_valid(p->id) || !plain(p->name, 128)) goto invalid;
    { char *name = g_strdup(p->name); gboolean empty = !*g_strstrip(name); g_free(name); if (empty) goto invalid; }
    if (p->type < SESSION_SSH || p->type > SESSION_SERIAL || !plain(p->username, 128) ||
        !plain(p->host, 253) || !plain(p->identity, 4096) || !plain(p->device, 4096) ||
        !plain(p->parity, 8) || !plain(p->flow, 8)) goto invalid;
    if (p->type != SESSION_SERIAL) {
        if (p->port < 1 || p->port > 65535 || !*p->host || p->host[0] == '-' ||
            (!g_hostname_is_ip_address(p->host) && !g_regex_match_simple("^[A-Za-z0-9][A-Za-z0-9._-]*$", p->host, 0, 0))) goto invalid;
    }
    if (p->type == SESSION_SSH && (!*p->username ||
        !g_regex_match_simple("^[A-Za-z0-9_][A-Za-z0-9_.@\\\\-]*$", p->username, 0, 0))) goto invalid;
    if (*p->identity && !g_path_is_absolute(p->identity)) goto invalid;
    if (p->type == SESSION_SERIAL && (!g_path_is_absolute(p->device) || !g_str_has_prefix(p->device, "/dev/") ||
        p->baud < 50 || p->baud > 4000000 || p->data_bits < 5 || p->data_bits > 8 || p->stop_bits < 1 || p->stop_bits > 2 ||
        (!g_str_equal(p->parity, "none") && !g_str_equal(p->parity, "even") && !g_str_equal(p->parity, "odd")) ||
        (!g_str_equal(p->flow, "none") && !g_str_equal(p->flow, "hard") && !g_str_equal(p->flow, "soft")))) goto invalid;
    if (p->timeout < 5 || p->timeout > 120) goto invalid;
    if (p->type != SESSION_SSH && p->auto_login && (!valid_prompt(p->login_prompt, TRUE) ||
        !valid_prompt(p->password_prompt, FALSE) || !valid_prompt(p->ready_prompt, FALSE) ||
        (*p->login_prompt && !*p->username))) goto invalid;
    return TRUE;
invalid:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
        "Invalid session: check name, host/port, username, absolute paths, serial settings and nonempty valid prompts (timeout 5–120 seconds)");
    return FALSE;
}
gboolean session_needs_password(const SessionProfile *p) { return p->type == SESSION_SSH ? p->password_auth : p->auto_login; }
char *session_endpoint(const SessionProfile *p) {
    char *s = g_strdup_printf("%d|%s|%d|%s|%s", p->type, p->host, p->port, p->username, p->device);
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, s, -1); g_free(s); return hash;
}
static JsonNode *profile_node(const SessionProfile *p) {
    JsonObject *o = json_object_new();
#define STRING(f) json_object_set_string_member(o, #f, p->f)
#define INT(f) json_object_set_int_member(o, #f, p->f)
#define BOOL(f) json_object_set_boolean_member(o, #f, p->f)
    STRING(id); STRING(name); STRING(host); STRING(username); STRING(identity); STRING(device);
    STRING(login_prompt); STRING(password_prompt); STRING(ready_prompt); STRING(parity); STRING(flow);
    INT(type); INT(port); INT(baud); INT(data_bits); INT(stop_bits); INT(timeout);
    BOOL(password_auth); BOOL(remember); BOOL(auto_login); BOOL(send_initial_enter);
#undef STRING
#undef INT
#undef BOOL
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, o); return node;
}
static SessionProfile *parse_profile(JsonNode *node, GError **error) {
    if (!JSON_NODE_HOLDS_OBJECT(node)) goto invalid;
    JsonObject *o = json_node_get_object(node);
    if (json_object_has_member(o, "password")) goto invalid;
    SessionProfile *p = session_profile_new();
#define STRING(f) { JsonNode *n = json_object_get_member(o, #f); if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING) goto bad; g_free(p->f); p->f = g_strdup(json_node_get_string(n)); }
#define INT(f) { JsonNode *n = json_object_get_member(o, #f); if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64 || json_node_get_int(n) < 0 || json_node_get_int(n) > G_MAXINT) goto bad; p->f = json_node_get_int(n); }
#define BOOL(f) { JsonNode *n = json_object_get_member(o, #f); if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_BOOLEAN) goto bad; p->f = json_node_get_boolean(n); }
    STRING(id); STRING(name); STRING(host); STRING(username); STRING(identity); STRING(device);
    STRING(login_prompt); STRING(password_prompt); STRING(ready_prompt); STRING(parity); STRING(flow);
    INT(type); INT(port); INT(baud); INT(data_bits); INT(stop_bits); INT(timeout);
    BOOL(password_auth); BOOL(remember); BOOL(auto_login); BOOL(send_initial_enter);
#undef STRING
#undef INT
#undef BOOL
    if (session_profile_validate(p, error)) return p;
    session_profile_free(p); return NULL;
bad: session_profile_free(p);
invalid: g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid session profile format"); return NULL;
}
char *sessions_config_path(void) {
    const char *base = g_getenv("XDG_CONFIG_HOME");
    return base && *base ? g_build_filename(base, "devterm", "sessions.json", NULL) :
        g_build_filename(g_get_home_dir(), ".config", "devterm", "sessions.json", NULL);
}
GPtrArray *sessions_load(const char *path, GError **error) {
    GPtrArray *profiles = g_ptr_array_new_with_free_func(session_profile_free);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) return profiles;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, error)) goto fail;
    JsonNode *node = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(node)) goto invalid;
    JsonObject *root = json_node_get_object(node);
    JsonNode *version = json_object_get_member(root, "version"), *list = json_object_get_member(root, "sessions");
    if (!version || !JSON_NODE_HOLDS_VALUE(version) || json_node_get_value_type(version) != G_TYPE_INT64 ||
        json_node_get_int(version) != 1 || !list || !JSON_NODE_HOLDS_ARRAY(list)) goto invalid;
    JsonArray *array = json_node_get_array(list);
    for (guint i = 0; i < json_array_get_length(array); i++) {
        SessionProfile *p = parse_profile(json_array_get_element(array, i), error); if (!p) goto fail;
        for (guint j = 0; j < profiles->len; j++)
            if (g_str_equal(p->id, ((SessionProfile *)profiles->pdata[j])->id)) { session_profile_free(p); goto invalid; }
        g_ptr_array_add(profiles, p);
    }
    g_object_unref(parser); return profiles;
invalid: g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid sessions.json (expected version 1 and unique profiles)");
fail: g_object_unref(parser); g_ptr_array_unref(profiles); return NULL;
}
gboolean sessions_save(const char *path, GPtrArray *profiles, GError **error) {
    JsonObject *root = json_object_new(); JsonArray *array = json_array_new();
    json_object_set_int_member(root, "version", 1); json_object_set_array_member(root, "sessions", array);
    for (guint i = 0; i < profiles->len; i++) {
        if (!session_profile_validate(profiles->pdata[i], error)) { json_object_unref(root); return FALSE; }
        json_array_add_element(array, profile_node(profiles->pdata[i]));
    }
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, root);
    char *text = json_to_string(node, TRUE); json_node_free(node);
    char *dir = g_path_get_dirname(path);
    gboolean ok = g_mkdir_with_parents(dir, 0700) == 0;
    if (!ok) g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Cannot create session configuration directory: %s", g_strerror(errno));
    /* CONSISTENT always writes a sibling temporary file, including for new files. */
    if (ok) ok = g_file_set_contents_full(path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
    g_free(text); g_free(dir); return ok;
}
static gboolean transfer(int fd, void *data, gsize size, gboolean writing) {
    char *p = data;
    while (size) {
        ssize_t n = writing ? write(fd, p, size) : read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return FALSE;
        size -= n; p += n;
    }
    return TRUE;
}
int session_launch_fd(const SessionProfile *p, const char *password, GError **error) {
    if (!session_profile_validate(p, error)) return -1;
    if (password && (!plain(password, 1023) || !*password)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Password must be 1–1023 UTF-8 bytes without control characters"); return -1;
    }
    JsonNode *node = profile_node(p); char *text = json_to_string(node, FALSE); json_node_free(node);
    guint32 sizes[] = {GUINT32_TO_BE(strlen(text)), GUINT32_TO_BE(password ? strlen(password) : 0)};
    int fd = memfd_create("devterm-session", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    gboolean ok = fd >= 0 && transfer(fd, sizes, sizeof sizes, TRUE) && transfer(fd, text, strlen(text), TRUE) &&
        (!password || transfer(fd, (void *)password, strlen(password), TRUE)) && lseek(fd, 0, SEEK_SET) == 0 &&
        fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) == 0;
    g_free(text);
    if (ok) return fd;
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot prepare session: %s", g_strerror(errno));
    if (fd >= 0) close(fd);
    return -1;
}
SessionProfile *session_read_launch(int fd, char **password, GError **error) {
    *password = NULL; guint32 sizes[2]; SessionProfile *p = NULL;
    if (!transfer(fd, sizes, sizeof sizes, FALSE)) goto invalid;
    gsize size = GUINT32_FROM_BE(sizes[0]), secret_size = GUINT32_FROM_BE(sizes[1]);
    if (!size || size > 65536 || secret_size > 1023) goto invalid;
    char *text = g_malloc0(size + 1), *secret = g_malloc0(secret_size + 1);
    if (!transfer(fd, text, size, FALSE) || !transfer(fd, secret, secret_size, FALSE)) { g_free(text); session_secret_free(secret); goto invalid; }
    JsonNode *node = json_from_string(text, error); g_free(text);
    if (node) { p = parse_profile(node, error); json_node_free(node); }
    if (!p) { session_secret_free(secret); return NULL; }
    if ((secret_size && (!plain(secret, 1023) || strlen(secret) != secret_size)) || (session_needs_password(p) && !secret_size)) {
        session_profile_free(p); session_secret_free(secret); goto invalid;
    }
    *password = secret_size ? secret : NULL; if (!secret_size) g_free(secret); return p;
invalid: g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid session launch data or missing password"); return NULL;
}
