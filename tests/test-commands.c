#include "../src/commands.h"
#include <gio/gio.h>
#include <glib/gstdio.h>

static char *temp_path(void) {
    GError *error = NULL;
    char *dir = g_dir_make_tmp("devterm-test-XXXXXX", &error);
    g_assert_no_error(error);
    char *path = g_build_filename(dir, "nested", "commands.json", NULL);
    g_free(dir);
    return path;
}
static void cleanup(const char *path) {
    char *dir = g_path_get_dirname(path);
    char *root = g_path_get_dirname(dir);
    g_remove(path); g_rmdir(dir); g_rmdir(root);
    g_free(dir); g_free(root);
}
static void test_roundtrip(void) {
    char *path = temp_path();
    GPtrArray *commands = g_ptr_array_new_with_free_func(quick_command_free);
    g_ptr_array_add(commands, quick_command_new("first", "Kiểm tra mạng", "printf 'đường dẫn' | cat > /tmp/x && echo ✓", TRUE));
    g_ptr_array_add(commands, quick_command_new("second", "Only type", "cd '/tmp/a b'", FALSE));
    GError *error = NULL;
    g_assert_true(commands_save(path, commands, &error));
    g_assert_no_error(error);
    GPtrArray *loaded = commands_load(path, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(loaded->len, ==, 2);
    QuickCommand *a = g_ptr_array_index(loaded, 0), *b = g_ptr_array_index(loaded, 1);
    g_assert_cmpstr(a->id, ==, "first");
    g_assert_cmpstr(a->name, ==, "Kiểm tra mạng");
    g_assert_cmpstr(a->command, ==, "printf 'đường dẫn' | cat > /tmp/x && echo ✓");
    g_assert_true(a->send_enter);
    g_assert_cmpstr(b->id, ==, "second");
    g_assert_cmpstr(b->command, ==, "cd '/tmp/a b'");
    g_assert_false(b->send_enter);
    g_ptr_array_unref(loaded); g_ptr_array_unref(commands);
    cleanup(path); g_free(path);
}
static void test_invalid(void) {
    const char *bad[] = {
        "{bad json", "[]", "{\"version\":2,\"commands\":[]}",
        "{\"version\":1,\"commands\":[{\"id\":\"a\",\"name\":\"\",\"command\":\"x\",\"send_enter\":true}]}",
        "{\"version\":1,\"commands\":[{\"id\":\"a\",\"name\":\"   \",\"command\":\"x\",\"send_enter\":true}]}",
        "{\"version\":1,\"commands\":[{\"id\":\"a\",\"name\":\"A\",\"command\":\"x\",\"send_enter\":true},{\"id\":\"a\",\"name\":\"B\",\"command\":\"y\",\"send_enter\":false}]}"
    };
    char *path = temp_path();
    char *dir = g_path_get_dirname(path);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
    for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
        g_assert_true(g_file_set_contents(path, bad[i], -1, NULL));
        GError *error = NULL;
        GPtrArray *loaded = commands_load(path, &error);
        g_assert_null(loaded); g_assert_nonnull(error); g_clear_error(&error);
        char *contents = NULL;
        g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
        g_assert_cmpstr(contents, ==, bad[i]);
        g_free(contents);
    }
    g_free(dir); cleanup(path); g_free(path);
}
static void test_write_failure(void) {
    char *path = temp_path();
    GPtrArray *commands = g_ptr_array_new_with_free_func(quick_command_free);
    g_ptr_array_add(commands, quick_command_new("a", "A", "echo a", TRUE));
    GError *error = NULL;
    g_assert_true(commands_save(path, commands, &error));
    g_assert_no_error(error);
    char *before = NULL;
    g_assert_true(g_file_get_contents(path, &before, NULL, NULL));
    char *dir = g_path_get_dirname(path);
    g_assert_cmpint(g_chmod(dir, 0500), ==, 0);
    g_ptr_array_add(commands, quick_command_new("b", "B", "echo b", TRUE));
    g_assert_false(commands_save(path, commands, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    char *after = NULL;
    g_assert_true(g_file_get_contents(path, &after, NULL, NULL));
    g_assert_cmpstr(after, ==, before);
    g_assert_cmpint(g_chmod(dir, 0700), ==, 0);
    g_free(before); g_free(after); g_free(dir);
    g_ptr_array_unref(commands); cleanup(path); g_free(path);
}
static void test_config_path(void) {
    char *old = g_strdup(g_getenv("XDG_CONFIG_HOME"));
    g_setenv("XDG_CONFIG_HOME", "/tmp/devterm-xdg-test", TRUE);
    char *path = commands_config_path();
    g_assert_cmpstr(path, ==, "/tmp/devterm-xdg-test/devterm/commands.json");
    g_free(path);
    g_setenv("XDG_CONFIG_HOME", "", TRUE);
    path = commands_config_path();
    char *fallback = g_build_filename(g_get_home_dir(), ".config", "devterm", "commands.json", NULL);
    g_assert_cmpstr(path, ==, fallback);
    g_free(path); g_free(fallback);
    if (old) g_setenv("XDG_CONFIG_HOME", old, TRUE);
    else g_unsetenv("XDG_CONFIG_HOME");
    g_free(old);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/commands/roundtrip", test_roundtrip);
    g_test_add_func("/commands/invalid", test_invalid);
    g_test_add_func("/commands/xdg-path", test_config_path);
    g_test_add_func("/commands/write-failure", test_write_failure);
    return g_test_run();
}
