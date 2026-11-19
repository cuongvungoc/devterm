#include "../src/shortcuts.h"
#include <glib/gstdio.h>

static char *root, *path;
static void test_round_trip(void) {
    ShortcutBindings *b = shortcut_bindings_new(); GError *error = NULL;
    g_assert_true(shortcut_bindings_set(b, SC_RECONNECT, "<Alt>F9", &error));
    g_assert_true(shortcut_bindings_set(b, SC_COPY_ALL, NULL, &error));
    g_assert_true(shortcut_bindings_set(b, SC_PANE_LEFT, "<Control><Alt>Left", &error));
    g_assert_true(shortcuts_save(path, b, &error)); g_assert_no_error(error);
    ShortcutBindings *copy = shortcuts_load(path, &error); g_assert_no_error(error); g_assert_nonnull(copy);
    for (int i = 0; i < SC_COUNT; i++) g_assert_cmpstr(copy->keys[i], ==, b->keys[i]);
    char *text; g_assert_true(g_file_get_contents(path, &text, NULL, &error));
    g_assert_nonnull(strstr(text, "\"copy-all\" : null")); g_assert_null(strstr(text, "new-tab")); g_free(text);
    shortcut_bindings_free(copy); shortcut_bindings_free(b);
}
static void test_invalid(void) {
    const char *bad[] = {"{", "[]", "{}", "{\"version\":2,\"bindings\":{}}", "{\"version\":1,\"bindings\":[]}",
        "{\"version\":1,\"bindings\":{\"typo\":null}}", "{\"version\":1,\"bindings\":{\"copy-all\":false}}",
        "{\"version\":1,\"bindings\":{\"copy-all\":\"\"}}", "{\"version\":1,\"bindings\":{\"copy-all\":\"a\"}}",
        "{\"version\":1,\"bindings\":{\"copy-all\":\"<Control><Shift>R\"}}"};
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        g_assert_true(g_file_set_contents(path, bad[i], -1, NULL)); GError *error = NULL;
        g_assert_null(shortcuts_load(path, &error)); g_assert_nonnull(error); g_clear_error(&error);
        char *text; g_file_get_contents(path, &text, NULL, NULL); g_assert_cmpstr(text, ==, bad[i]); g_free(text);
    }
}
static void test_swaps_and_validation(void) {
    const char *json = "{\"version\":1,\"bindings\":{\"reconnect-pane\":\"<Control><Shift>d\",\"disconnect-pane\":\"<Control><Shift>r\"}}";
    g_file_set_contents(path, json, -1, NULL); GError *error = NULL;
    ShortcutBindings *b = shortcuts_load(path, &error); g_assert_no_error(error); g_assert_nonnull(b);
    char *key = shortcut_normalize("<Shift><Ctrl>R", &error); g_assert_no_error(error);
    g_assert_cmpstr(b->keys[SC_DISCONNECT], ==, key); g_free(key);
    g_assert_false(shortcut_bindings_set(b, SC_COPY_ALL, b->keys[SC_DISCONNECT], &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS); g_clear_error(&error);
    g_assert_nonnull(b->keys[SC_COPY_ALL]);
    const char *bad[] = {"<Shift>a", "Escape", "Return", "Tab", "abc", "<Control>Shift_L"};
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) { g_assert_null(shortcut_normalize(bad[i], &error)); g_assert_nonnull(error); g_clear_error(&error); }
    shortcut_bindings_free(b);
}
static void test_defaults_and_paths(void) {
    g_remove(path); GError *error = NULL; ShortcutBindings *b = shortcuts_load(path, &error); g_assert_no_error(error);
    g_assert_true(shortcut_bindings_validate(b, &error)); g_assert_no_error(error);
    for (int i = 0; i < SC_COUNT; i++) {
        g_assert_nonnull(shortcut_definition(i)->id);
        for (int j = i+1; j < SC_COUNT; j++) g_assert_cmpstr(shortcut_definition(i)->id, !=, shortcut_definition(j)->id);
    }
    shortcut_bindings_free(b);
    g_setenv("XDG_CONFIG_HOME", root, TRUE); char *actual = shortcuts_config_path(); g_assert_cmpstr(actual, ==, path); g_free(actual);
    g_setenv("XDG_CONFIG_HOME", "", TRUE); actual = shortcuts_config_path();
    char *expected = g_build_filename(g_get_home_dir(), ".config", "devterm", "shortcuts.json", NULL);
    g_assert_cmpstr(actual, ==, expected); g_free(actual); g_free(expected);
}
static void test_write_failure(void) {
    ShortcutBindings *b = shortcut_bindings_new(); GError *error = NULL;
    char *blocked = g_build_filename(root, "blocked", NULL), *file = g_build_filename(blocked, "shortcuts.json", NULL);
    g_file_set_contents(blocked, "not a directory", -1, NULL);
    g_assert_false(shortcuts_save(file, b, &error)); g_assert_nonnull(error); g_clear_error(&error);
    char *text; g_file_get_contents(blocked, &text, NULL, NULL); g_assert_cmpstr(text, ==, "not a directory"); g_free(text);
    g_remove(blocked); g_free(blocked); g_free(file); shortcut_bindings_free(b);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL); root = g_dir_make_tmp("devterm-shortcuts-XXXXXX", NULL);
    char *dir = g_build_filename(root, "devterm", NULL); g_mkdir(dir, 0700); path = g_build_filename(dir, "shortcuts.json", NULL);
    g_test_add_func("/shortcuts/round-trip-overrides", test_round_trip);
    g_test_add_func("/shortcuts/invalid-preserved", test_invalid);
    g_test_add_func("/shortcuts/swaps-conflicts-validation", test_swaps_and_validation);
    g_test_add_func("/shortcuts/defaults-xdg", test_defaults_and_paths);
    g_test_add_func("/shortcuts/write-failure", test_write_failure);
    int result = g_test_run(); g_remove(path); g_rmdir(dir); g_rmdir(root); g_free(dir); g_free(path); g_free(root); return result;
}
