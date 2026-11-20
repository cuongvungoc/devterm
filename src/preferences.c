#include "preferences.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <errno.h>

char *preferences_config_path(void) {
    const char *base = g_getenv("XDG_CONFIG_HOME");
    return base && *base ? g_build_filename(base, "devterm", "preferences.json", NULL) :
        g_build_filename(g_get_home_dir(), ".config", "devterm", "preferences.json", NULL);
}
Preferences *preferences_load(const char *path, GError **error) {
    char *data; gsize length; GError *local = NULL;
    Preferences *preferences = g_new0(Preferences, 1); preferences->confirm_close_sessions = TRUE;
    if (!g_file_get_contents(path, &data, &length, &local)) {
        if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) { g_clear_error(&local); return preferences; }
        g_propagate_error(error, local); g_free(preferences); return NULL;
    }
    JsonParser *parser = json_parser_new();
    gboolean valid = json_parser_load_from_data(parser, data, length, error); g_free(data);
    if (valid) {
        JsonNode *root = json_parser_get_root(parser), *version = NULL, *confirm = NULL, *appearance = NULL;
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *object = json_node_get_object(root);
            version = json_object_get_member(object, "version"); confirm = json_object_get_member(object, "confirm_close_sessions");
            appearance = json_object_get_member(object, "appearance");
        }
        valid = version && JSON_NODE_HOLDS_VALUE(version) && json_node_get_value_type(version) == G_TYPE_INT64 &&
            json_node_get_int(version) == 1 && confirm && JSON_NODE_HOLDS_VALUE(confirm) && json_node_get_value_type(confirm) == G_TYPE_BOOLEAN;
        if (valid && appearance) {
            valid = JSON_NODE_HOLDS_VALUE(appearance) && json_node_get_value_type(appearance) == G_TYPE_STRING;
            const char *name = valid ? json_node_get_string(appearance) : NULL;
            valid = g_strcmp0(name, "light") == 0 || g_strcmp0(name, "dark") == 0;
            if (valid && g_str_equal(name, "light")) preferences->appearance = PREFERENCES_APPEARANCE_LIGHT;
        }
        if (valid) preferences->confirm_close_sessions = json_node_get_boolean(confirm);
        else g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "Invalid preferences.json (expected version 1, boolean confirm_close_sessions and optional appearance: light or dark)");
    }
    g_object_unref(parser);
    if (!valid) g_clear_pointer(&preferences, g_free);
    return preferences;
}
gboolean preferences_save(const char *path, const Preferences *preferences, GError **error) {
    if (preferences->appearance != PREFERENCES_APPEARANCE_DARK && preferences->appearance != PREFERENCES_APPEARANCE_LIGHT) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid appearance preference"); return FALSE;
    }
    /* Revalidate at save time too; never replace a file that became invalid. */
    Preferences *disk = preferences_load(path, error); if (!disk) return FALSE; g_free(disk);
    char *directory = g_path_get_dirname(path);
    if (g_mkdir_with_parents(directory, 0700) < 0) {
        int code = errno; g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(code), "Cannot create preferences directory: %s", g_strerror(code));
        g_free(directory); return FALSE;
    }
    g_free(directory);
    char *json = g_strdup_printf("{\n  \"version\": 1,\n  \"confirm_close_sessions\": %s,\n  \"appearance\": \"%s\"\n}\n",
        preferences->confirm_close_sessions ? "true" : "false",
        preferences->appearance == PREFERENCES_APPEARANCE_LIGHT ? "light" : "dark");
    gboolean saved = g_file_set_contents_full(path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
    g_free(json); return saved;
}
