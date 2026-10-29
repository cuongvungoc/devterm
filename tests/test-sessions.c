#define _GNU_SOURCE
#include "../src/sessions.h"
#include "../src/login.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static SessionProfile *profile(void) {
    SessionProfile *p = session_profile_new(); g_free(p->name); p->name = g_strdup("Router Đà Nẵng 日本語");
    g_free(p->host); p->host = g_strdup("127.0.0.1"); return p;
}
static void test_roundtrip(void) {
    GError *error = NULL; char *dir = g_dir_make_tmp("devterm-profiles-XXXXXX", &error); g_assert_no_error(error);
    char *path = g_build_filename(dir, "sessions.json", NULL), *backup = g_build_filename(dir, "previous", NULL);
    GPtrArray *array = g_ptr_array_new_with_free_func(session_profile_free); SessionProfile *first = profile(), *second = profile();
    second->type = SESSION_TELNET; second->port = 2323; second->auto_login = TRUE; second->remember = TRUE;
    g_ptr_array_add(array, first); g_ptr_array_add(array, second);
    g_assert_true(g_file_set_contents(path, "old contents", -1, NULL)); g_assert_cmpint(link(path, backup), ==, 0);
    g_assert_true(sessions_save(path, array, &error)); g_assert_no_error(error);
    GPtrArray *loaded = sessions_load(path, &error); g_assert_no_error(error); g_assert_cmpuint(loaded->len, ==, 2);
    SessionProfile *a = loaded->pdata[0], *b = loaded->pdata[1];
    g_assert_cmpstr(a->name, ==, first->name); g_assert_cmpstr(a->id, ==, first->id);
    g_assert_cmpstr(b->id, ==, second->id); g_assert_true(b->auto_login); g_assert_true(b->remember); g_assert_cmpint(b->port, ==, 2323);
    char *text = NULL; g_assert_true(g_file_get_contents(backup, &text, NULL, NULL)); g_assert_cmpstr(text, ==, "old contents"); g_free(text);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL)); g_assert_null(strstr(text, "\"password\"")); g_free(text);
    g_ptr_array_unref(loaded); g_ptr_array_unref(array); g_unlink(path); g_unlink(backup); g_rmdir(dir); g_free(path); g_free(backup); g_free(dir);
}
static void test_invalid(void) {
    SessionProfile *p = profile(); GError *error = NULL;
    g_free(p->host); p->host = g_strdup("-oProxyCommand=oops");
    g_assert_false(session_profile_validate(p, &error)); g_clear_error(&error);
    g_free(p->host); p->host = g_strdup("localhost"); p->type = SESSION_TELNET; p->auto_login = TRUE;
    g_free(p->ready_prompt); p->ready_prompt = g_strdup("(");
    g_assert_false(session_profile_validate(p, &error)); g_clear_error(&error);
    g_free(p->ready_prompt); p->ready_prompt = g_strdup(".*");
    g_assert_false(session_profile_validate(p, &error)); g_clear_error(&error); session_profile_free(p);
    char *dir = g_dir_make_tmp("devterm-invalid-profiles-XXXXXX", NULL), *path = g_build_filename(dir, "sessions.json", NULL);
    const char *invalid[] = {"{", "[]", "{\"version\":true,\"sessions\":[]}", "{\"version\":999,\"sessions\":[]}", "{\"version\":1,\"sessions\":[{}]}", NULL};
    for (int i = 0; invalid[i]; i++) {
        g_assert_true(g_file_set_contents(path, invalid[i], -1, NULL));
        g_assert_null(sessions_load(path, &error)); g_assert_nonnull(error); g_clear_error(&error);
        char *after = NULL; g_file_get_contents(path, &after, NULL, NULL); g_assert_cmpstr(after, ==, invalid[i]); g_free(after);
    }
    g_unlink(path); g_rmdir(dir); g_free(path); g_free(dir);
}
static void test_endpoint_binding(void) {
    SessionProfile *p = profile(), *copy = session_profile_copy(p);
    char *a = session_endpoint(p), *b = session_endpoint(copy); g_assert_cmpstr(a, ==, b); g_free(b);
    g_free(copy->name); copy->name = g_strdup("Another display name"); b = session_endpoint(copy); g_assert_cmpstr(a, ==, b); g_free(b);
    copy->port++; b = session_endpoint(copy); g_assert_cmpstr(a, !=, b);
    g_free(a); g_free(b); session_profile_free(copy); session_profile_free(p);
}
static void test_secret_descriptor(void) {
    SessionProfile *p = profile(); GError *error = NULL; const char *password = "dummy-Đường-密碼";
    int fd = session_launch_fd(p, password, &error); g_assert_no_error(error); g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(fcntl(fd, F_GET_SEALS) & F_SEAL_WRITE, ==, F_SEAL_WRITE);
    g_assert_cmpint(write(fd, "overwrite", 9), ==, -1);
    char *secret = NULL; SessionProfile *loaded = session_read_launch(fd, &secret, &error);
    g_assert_no_error(error); g_assert_nonnull(loaded); g_assert_cmpstr(secret, ==, password); g_assert_cmpstr(loaded->id, ==, p->id);
    session_secret_free(secret); close(fd); session_profile_free(loaded);
    g_assert_cmpint(session_launch_fd(p, "bad\rcommand", &error), ==, -1); g_assert_nonnull(error); g_clear_error(&error);
    session_profile_free(p);
}
static void test_login_prompts(void) {
    SessionProfile *p = profile(); p->type = SESSION_TELNET; p->auto_login = TRUE;
    LoginMachine *m = login_machine_new(p);
#define FEED(s) login_machine_feed(m, s, strlen(s))
    g_assert_cmpint(FEED("banner login required\r\n\033[3"), ==, LOGIN_WAIT);
    g_assert_cmpint(FEED("1mUser"), ==, LOGIN_WAIT);
    g_assert_cmpint(FEED("name: \033[0m"), ==, LOGIN_SEND_USER);
    g_assert_cmpint(FEED("admin\r\nPass"), ==, LOGIN_WAIT);
    g_assert_cmpint(FEED("word: "), ==, LOGIN_SEND_PASSWORD);
    g_assert_cmpint(FEED("echoed-dummy-password\r\nrouter# "), ==, LOGIN_READY); login_machine_free(m);
    m = login_machine_new(p);
    g_assert_cmpint(FEED("login: "), ==, LOGIN_SEND_USER); g_assert_cmpint(FEED("Password: "), ==, LOGIN_SEND_PASSWORD);
    g_assert_cmpint(FEED("Login incorrect\r\nlogin: "), ==, LOGIN_FAILED); login_machine_free(m);
    g_free(p->login_prompt); p->login_prompt = g_strdup(""); m = login_machine_new(p);
    g_assert_cmpint(FEED("Password: "), ==, LOGIN_SEND_PASSWORD);
    g_assert_cmpint(FEED("Password: "), ==, LOGIN_FAILED); login_machine_free(m);
#undef FEED
    session_profile_free(p);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sessions/roundtrip-atomic", test_roundtrip);
    g_test_add_func("/sessions/invalid", test_invalid);
    g_test_add_func("/sessions/endpoint-binding", test_endpoint_binding);
    g_test_add_func("/sessions/secret-descriptor", test_secret_descriptor);
    g_test_add_func("/sessions/login-prompts", test_login_prompts);
    return g_test_run();
}
