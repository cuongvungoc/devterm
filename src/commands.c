#include "commands.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <unistd.h>

QuickCommand *quick_command_new(const char *id, const char *name, const char *command, gboolean send_enter) {
    QuickCommand *item = g_new0(QuickCommand, 1);
    item->id = g_strdup(id);
    item->name = g_strdup(name);
    item->command = g_strdup(command);
    item->send_enter = send_enter;
    return item;
}

void quick_command_free(gpointer data) {
    QuickCommand *item = data;
    if (!item) return;
    g_free(item->id);
    g_free(item->name);
    g_free(item->command);
    g_free(item);
}

char *commands_config_path(void) {
    const char *base = g_getenv("XDG_CONFIG_HOME");
    char *fallback = NULL;
    if (!base || !*base) {
        fallback = g_build_filename(g_get_home_dir(), ".config", NULL);
        base = fallback;
    }
    char *path = g_build_filename(base, "devterm", "commands.json", NULL);
    g_free(fallback);
    return path;
}

static gboolean valid_text(const char *s) {
    if (!s || !g_utf8_validate(s, -1, NULL)) return FALSE;
    for (const char *p = s; *p; p = g_utf8_next_char(p))
        if (!g_unichar_isspace(g_utf8_get_char(p))) return TRUE;
    return FALSE;
}

static gboolean string_member(JsonObject *object, const char *key, const char **value) {
    if (!json_object_has_member(object, key)) return FALSE;
    JsonNode *node = json_object_get_member(object, key);
    if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) return FALSE;
    *value = json_node_get_string(node);
    return valid_text(*value);
}

GPtrArray *commands_load(const char *path, GError **error) {
    GPtrArray *items = g_ptr_array_new_with_free_func(quick_command_free);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) return items;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, error)) goto fail;
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) goto invalid;
    JsonObject *object = json_node_get_object(root);
    JsonNode *version = json_object_get_member(object, "version");
    JsonNode *commands = json_object_get_member(object, "commands");
    if (!version || !JSON_NODE_HOLDS_VALUE(version) || json_node_get_value_type(version) != G_TYPE_INT64 ||
        json_node_get_int(version) != 1 || !commands || !JSON_NODE_HOLDS_ARRAY(commands)) goto invalid;
    JsonArray *array = json_node_get_array(commands);
    GHashTable *ids = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < json_array_get_length(array); i++) {
        JsonNode *node = json_array_get_element(array, i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) { g_hash_table_unref(ids); goto invalid; }
        JsonObject *entry = json_node_get_object(node);
        const char *id, *name, *command;
        JsonNode *send = json_object_get_member(entry, "send_enter");
        if (!string_member(entry, "id", &id) || !string_member(entry, "name", &name) ||
            !string_member(entry, "command", &command) || !send || !JSON_NODE_HOLDS_VALUE(send) ||
            json_node_get_value_type(send) != G_TYPE_BOOLEAN || g_hash_table_contains(ids, id)) {
            g_hash_table_unref(ids);
            goto invalid;
        }
        g_hash_table_add(ids, (gpointer)id);
        g_ptr_array_add(items, quick_command_new(id, name, command, json_node_get_boolean(send)));
    }
    g_hash_table_unref(ids);
    g_object_unref(parser);
    return items;
invalid:
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid Quick Commands configuration in %s (expected version 1 and valid, unique commands)", path);
fail:
    g_object_unref(parser);
    g_ptr_array_unref(items);
    return NULL;
}

gboolean commands_save(const char *path, GPtrArray *items, GError **error) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "version"); json_builder_add_int_value(builder, 1);
    json_builder_set_member_name(builder, "commands"); json_builder_begin_array(builder);
    for (guint i = 0; i < items->len; i++) {
        QuickCommand *item = g_ptr_array_index(items, i);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id"); json_builder_add_string_value(builder, item->id);
        json_builder_set_member_name(builder, "name"); json_builder_add_string_value(builder, item->name);
        json_builder_set_member_name(builder, "command"); json_builder_add_string_value(builder, item->command);
        json_builder_set_member_name(builder, "send_enter"); json_builder_add_boolean_value(builder, item->send_enter);
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder); json_builder_end_object(builder);
    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    json_generator_set_pretty(generator, TRUE);
    char *data = json_generator_to_data(generator, NULL);
    char *dir = g_path_get_dirname(path);
    gboolean ok = g_mkdir_with_parents(dir, 0700) == 0;
    if (!ok) g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot create %s: %s", dir, g_strerror(errno));
    if (ok) {
        char *temp = g_build_filename(dir, ".commands.json.XXXXXX", NULL);
        int fd = g_mkstemp(temp);
        if (fd < 0) {
            ok = FALSE;
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot create temporary configuration file: %s", g_strerror(errno));
        } else {
            gsize size = strlen(data), offset = 0;
            while (offset < size) {
                ssize_t written = write(fd, data + offset, size - offset);
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) { if (written == 0) errno = EIO; ok = FALSE; break; }
                offset += (gsize)written;
            }
            if (ok && fsync(fd) != 0) ok = FALSE;
            int saved_errno = errno;
            if (close(fd) != 0 && ok) { ok = FALSE; saved_errno = errno; }
            if (ok && g_rename(temp, path) != 0) { ok = FALSE; saved_errno = errno; }
            if (!ok) {
                g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno), "Cannot save %s: %s", path, g_strerror(saved_errno));
                g_unlink(temp);
            }
        }
        g_free(temp);
    }
    g_free(dir); g_free(data); json_node_free(root);
    g_object_unref(generator); g_object_unref(builder);
    return ok;
}
