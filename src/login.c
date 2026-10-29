#define _GNU_SOURCE
#include "login.h"
#include <string.h>
struct LoginMachine { GRegex *user, *password, *ready; GString *text; int stage, escape; };
LoginMachine *login_machine_new(const SessionProfile *p) {
    LoginMachine *m = g_new0(LoginMachine, 1);
    if (*p->login_prompt) m->user = g_regex_new(p->login_prompt, G_REGEX_RAW | G_REGEX_MULTILINE, 0, NULL);
    m->password = g_regex_new(p->password_prompt, G_REGEX_RAW | G_REGEX_MULTILINE, 0, NULL);
    m->ready = g_regex_new(p->ready_prompt, G_REGEX_RAW | G_REGEX_MULTILINE, 0, NULL);
    m->stage = m->user ? 0 : 1; m->text = g_string_new(NULL); return m;
}
static gboolean matches(GRegex *r, GString *s) {
    if (!r) return FALSE;
    GMatchInfo *info = NULL; gboolean ok = FALSE;
    if (g_regex_match_full(r, s->str, s->len, 0, 0, &info, NULL)) {
        do { int end; g_match_info_fetch_pos(info, 0, NULL, &end); if (end == (int)s->len) { ok = TRUE; break; } }
        while (g_match_info_next(info, NULL));
    }
    g_match_info_free(info); return ok;
}
LoginAction login_machine_feed(LoginMachine *m, const char *data, gsize size) {
    if (m->stage >= 3) return m->stage == 3 ? LOGIN_READY : LOGIN_FAILED;
    /* Strip ANSI CSI/OSC across chunk boundaries; never put authentication
     * transcript into VTE, even if a device incorrectly echoes the password. */
    for (gsize i = 0; i < size; i++) {
        guchar c = data[i];
        if (m->escape == 1) { m->escape = c == '[' ? 2 : c == ']' ? 3 : 0; continue; }
        if (m->escape == 2) { if (c >= 0x40 && c <= 0x7e) m->escape = 0; continue; }
        if (m->escape == 3) { if (c == 7) m->escape = 0; else if (c == 27) m->escape = 4; continue; }
        if (m->escape == 4) { m->escape = c == '\\' ? 0 : 3; continue; }
        if (c == 27) { m->escape = 1; continue; }
        if (c == 8 || c == 127) { if (m->text->len) g_string_truncate(m->text, m->text->len - 1); }
        else if (c >= 32 || c == '\r' || c == '\n' || c == '\t') g_string_append_c(m->text, c);
    }
    if (m->text->len > 8192) g_string_erase(m->text, 0, m->text->len - 8192);
    LoginAction action = LOGIN_WAIT;
    if (m->stage == 0 && matches(m->user, m->text)) { m->stage = 1; action = LOGIN_SEND_USER; }
    else if (m->stage == 1 && matches(m->password, m->text)) { m->stage = 2; action = LOGIN_SEND_PASSWORD; }
    else if (m->stage == 2 && (matches(m->user, m->text) || matches(m->password, m->text))) { m->stage = 4; action = LOGIN_FAILED; }
    else if (m->stage == 2 && matches(m->ready, m->text)) { m->stage = 3; action = LOGIN_READY; }
    if (action != LOGIN_WAIT) g_string_truncate(m->text, 0);
    return action;
}
void login_machine_free(LoginMachine *m) {
    if (!m) return;
    if (m->user) g_regex_unref(m->user);
    g_regex_unref(m->password); g_regex_unref(m->ready);
    /* May contain an echoed secret until the state machine advances. */
    explicit_bzero(m->text->str, m->text->allocated_len);
    g_string_free(m->text, TRUE); g_free(m);
}
