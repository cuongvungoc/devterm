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
        "{bad json", "[]", "{\"version\":3,\"commands\":[]}",
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

static void test_sequence_roundtrip(void) {
    char *path = temp_path(); GError *error = NULL;
    GPtrArray *items = g_ptr_array_new_with_free_func(quick_command_free);
    g_ptr_array_add(items, quick_command_new("a", "Single", "cd '/tmp/a b'", FALSE));
    QuickCommand *seq = quick_sequence_new("s", "Chuỗi 日本語");
    g_ptr_array_add(seq->steps, quick_command_step_new("  printf 'Đường | 東京' | cat && pwd  ", 375));
    g_ptr_array_add(seq->steps, quick_command_step_new("cd '/tmp/a b'", 1200));
    g_ptr_array_add(seq->steps, quick_command_step_new("pwd", 50000)); /* Last delay is normalized away. */
    g_ptr_array_add(items, seq); g_ptr_array_add(items, quick_command_new("b", "After", "echo done", TRUE));
    g_assert_true(commands_save(path, items, &error)); g_assert_no_error(error);
    GPtrArray *loaded = commands_load(path, &error); g_assert_no_error(error); g_assert_cmpuint(loaded->len, ==, 3);
    QuickCommand *copy = loaded->pdata[1]; g_assert_cmpint(copy->kind, ==, QUICK_SEQUENCE);
    g_assert_cmpstr(copy->id, ==, "s"); g_assert_cmpstr(copy->name, ==, "Chuỗi 日本語");
    for (guint i = 0; i < 3; i++) {
        QuickCommandStep *original = seq->steps->pdata[i], *step = copy->steps->pdata[i];
        g_assert_cmpstr(step->command, ==, original->command);
        g_assert_cmpuint(step->delay_ms, ==, i == 2 ? 0 : original->delay_ms);
    }
    g_assert_cmpstr(((QuickCommand *)loaded->pdata[2])->id, ==, "b");
    g_assert_false(((QuickCommand *)loaded->pdata[0])->send_enter);
    QuickCommand *snapshot = quick_command_copy(seq); g_ptr_array_unref(items);
    g_assert_cmpstr(((QuickCommandStep *)snapshot->steps->pdata[0])->command, ==, "  printf 'Đường | 東京' | cat && pwd  ");
    quick_command_free(snapshot); g_ptr_array_unref(loaded); cleanup(path); g_free(path);
}
static void test_migrate_v1(void) {
    char *path = temp_path(), *dir = g_path_get_dirname(path); g_mkdir_with_parents(dir, 0700);
    const char *legacy = "{\"version\":1, \"commands\":[{\"id\":\"stable\",\"name\":\"Đường\",\"command\":\"printf 'a'\\npwd\",\"send_enter\":false}]}\n";
    g_file_set_contents(path, legacy, -1, NULL); GError *error = NULL;
    GPtrArray *items = commands_load(path, &error); g_assert_no_error(error);
    char *text; g_file_get_contents(path, &text, NULL, NULL); g_assert_cmpstr(text, ==, legacy); g_free(text);
    /* A failed backup/upgrade must leave the original legacy bytes intact. */
    g_assert_cmpint(g_chmod(dir, 0500), ==, 0);
    g_assert_false(commands_save(path, items, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_file_get_contents(path, &text, NULL, NULL); g_assert_cmpstr(text, ==, legacy); g_free(text);
    g_assert_cmpint(g_chmod(dir, 0700), ==, 0);
    g_assert_true(commands_save(path, items, &error)); g_assert_no_error(error); g_ptr_array_unref(items);
    items = commands_load(path, &error); g_assert_no_error(error);
    QuickCommand *item = items->pdata[0]; g_assert_cmpstr(item->id, ==, "stable");
    g_assert_cmpstr(item->command, ==, "printf 'a'\npwd"); g_assert_false(item->send_enter); g_ptr_array_unref(items);
    GDir *directory = g_dir_open(dir, 0, NULL); const char *name; guint backups = 0;
    while ((name = g_dir_read_name(directory))) if (g_str_has_prefix(name, "commands.json.v1-")) {
        char *backup = g_build_filename(dir, name, NULL); g_file_get_contents(backup, &text, NULL, NULL);
        g_assert_cmpstr(text, ==, legacy); backups++; g_free(text); g_remove(backup); g_free(backup);
    }
    g_dir_close(directory); g_assert_cmpuint(backups, ==, 1); g_free(dir); cleanup(path); g_free(path);
}
static void test_invalid_sequences(void) {
    const char *entries[] = {
        "{\"kind\":\"sequence\",\"steps\":[]}",
        "{\"kind\":\"sequence\",\"steps\":[{\"command\":\"\",\"delay_ms\":0}]}",
        "{\"kind\":\"sequence\",\"steps\":[{\"command\":\"a\\nb\",\"delay_ms\":0}]}",
        "{\"kind\":\"sequence\",\"steps\":[{\"command\":\"x\",\"delay_ms\":-1}]}",
        "{\"kind\":\"sequence\",\"steps\":[{\"command\":\"x\",\"delay_ms\":60001}]}",
        "{\"kind\":\"sequence\",\"steps\":[{\"command\":\"x\",\"delay_ms\":1.5}]}",
        "{\"kind\":\"sequence\",\"steps\":[{\"command\":\"x\",\"delay_ms\":\"5\"}]}",
        "{\"kind\":\"sequence\",\"steps\":[null]}",
        "{\"kind\":\"unknown\"}", "{\"command\":\"pwd\",\"send_enter\":true}"
    };
    char *path = temp_path(), *dir = g_path_get_dirname(path); g_mkdir_with_parents(dir, 0700);
    GPtrArray *valid = g_ptr_array_new_with_free_func(quick_command_free); g_ptr_array_add(valid, quick_command_new("new", "New", "pwd", TRUE));
    for (guint i = 0; i < G_N_ELEMENTS(entries); i++) {
        char *entry = g_strdup_printf("{\"id\":\"s\",\"name\":\"S\",%s", entries[i] + 1);
        char *json = g_strdup_printf("{\"version\":2,\"commands\":[%s]}", entry); g_free(entry);
        g_file_set_contents(path, json, -1, NULL); GError *error = NULL;
        g_assert_null(commands_load(path, &error)); g_assert_nonnull(error); g_clear_error(&error);
        g_assert_false(commands_save(path, valid, &error)); g_assert_nonnull(error); g_clear_error(&error);
        char *after; g_file_get_contents(path, &after, NULL, NULL); g_assert_cmpstr(after, ==, json); g_free(after); g_free(json);
    }
    g_ptr_array_unref(valid); g_free(dir); cleanup(path); g_free(path);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/commands/roundtrip", test_roundtrip);
    g_test_add_func("/commands/invalid", test_invalid);
    g_test_add_func("/commands/xdg-path", test_config_path);
    g_test_add_func("/commands/write-failure", test_write_failure);
    g_test_add_func("/commands/sequence-roundtrip", test_sequence_roundtrip);
    g_test_add_func("/commands/migrate-v1-backup", test_migrate_v1);
    g_test_add_func("/commands/invalid-sequences-preserved", test_invalid_sequences);
    return g_test_run();
}
