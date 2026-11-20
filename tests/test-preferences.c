#include "../src/preferences.h"
#include <glib/gstdio.h>

static char *root, *path;
static void test_round_trip(void) {
    Preferences *preferences = preferences_load(path, NULL);
    g_assert_nonnull(preferences); g_assert_true(preferences->confirm_close_sessions);
    preferences->confirm_close_sessions = FALSE;
    g_assert_true(preferences_save(path, preferences, NULL)); g_free(preferences);
    preferences = preferences_load(path, NULL); g_assert_nonnull(preferences);
    g_assert_false(preferences->confirm_close_sessions);
    GStatBuf info; g_assert_cmpint(g_stat(path, &info), ==, 0); g_assert_cmpint(info.st_mode & 0777, ==, 0600);
    preferences->confirm_close_sessions = TRUE;
    g_assert_true(preferences_save(path, preferences, NULL)); g_free(preferences);
    preferences = preferences_load(path, NULL); g_assert_true(preferences->confirm_close_sessions); g_free(preferences);
    g_remove(path);
}
static void test_invalid_is_preserved(void) {
    const char *invalid[] = {"", "{broken", "null", "[]", "{}", "{\"version\":2,\"confirm_close_sessions\":false}",
        "{\"version\":1,\"confirm_close_sessions\":\"false\"}", "{\"version\":1.0,\"confirm_close_sessions\":true}"};
    Preferences preferences = {TRUE};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_assert_true(g_file_set_contents(path, invalid[i], -1, NULL)); GError *error = NULL;
        g_assert_null(preferences_load(path, &error)); g_assert_nonnull(error); g_clear_error(&error);
        g_assert_false(preferences_save(path, &preferences, &error)); g_assert_nonnull(error); g_clear_error(&error);
        char *text = NULL; g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
        g_assert_cmpstr(text, ==, invalid[i]); g_free(text);
    }
    g_remove(path);
}
static void test_external_change_and_write_failure(void) {
    Preferences original = {FALSE}, changed = {TRUE};
    g_assert_true(preferences_save(path, &original, NULL));
    g_assert_true(g_file_set_contents(path, "externally damaged", -1, NULL));
    g_assert_false(preferences_save(path, &changed, NULL));
    char *text = NULL; g_file_get_contents(path, &text, NULL, NULL); g_assert_cmpstr(text, ==, "externally damaged"); g_free(text); g_remove(path);
    char *file = g_build_filename(root, "blocked", NULL), *child = g_build_filename(file, "preferences.json", NULL);
    g_assert_true(g_file_set_contents(file, "regular file", -1, NULL)); GError *error = NULL;
    g_assert_false(preferences_save(child, &changed, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_true(changed.confirm_close_sessions); g_remove(file); g_free(child); g_free(file);
}
static void test_xdg_path(void) {
    g_setenv("XDG_CONFIG_HOME", root, TRUE); char *actual = preferences_config_path();
    char *expected = g_build_filename(root, "devterm", "preferences.json", NULL); g_assert_cmpstr(actual, ==, expected); g_free(actual); g_free(expected);
    g_setenv("XDG_CONFIG_HOME", "", TRUE); actual = preferences_config_path();
    expected = g_build_filename(g_get_home_dir(), ".config", "devterm", "preferences.json", NULL);
    g_assert_cmpstr(actual, ==, expected); g_free(actual);
    g_unsetenv("XDG_CONFIG_HOME"); actual = preferences_config_path(); g_assert_cmpstr(actual, ==, expected);
    g_free(actual); g_free(expected); g_setenv("XDG_CONFIG_HOME", root, TRUE);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL); root = g_dir_make_tmp("devterm-preferences-XXXXXX", NULL);
    char *directory = g_build_filename(root, "Đường — 日本語", NULL); g_mkdir(directory, 0700);
    path = g_build_filename(directory, "preferences.json", NULL);
    g_test_add_func("/preferences/default-round-trip", test_round_trip);
    g_test_add_func("/preferences/invalid-preserved", test_invalid_is_preserved);
    g_test_add_func("/preferences/external-change-write-failure", test_external_change_and_write_failure);
    g_test_add_func("/preferences/xdg", test_xdg_path);
    int result = g_test_run(); g_rmdir(directory); g_rmdir(root); g_free(path); g_free(directory); g_free(root); return result;
}
