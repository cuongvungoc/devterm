#include "commands.h"
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <string.h>

static void step_free(gpointer data) {
    QuickCommandStep *step = data; g_free(step->command); g_free(step);
}
QuickCommandStep *quick_command_step_new(const char *command, guint delay_ms) {
    QuickCommandStep *step = g_new0(QuickCommandStep, 1);
    step->command = g_strdup(command); step->delay_ms = delay_ms; return step;
}
QuickCommand *quick_command_new(const char *id, const char *name, const char *command, gboolean send_enter) {
    QuickCommand *item = g_new0(QuickCommand, 1);
    item->id = g_strdup(id); item->name = g_strdup(name);
    item->command = g_strdup(command); item->send_enter = send_enter; return item;
}
QuickCommand *quick_sequence_new(const char *id, const char *name) {
    QuickCommand *item = quick_command_new(id, name, NULL, TRUE);
    item->kind = QUICK_SEQUENCE; item->steps = g_ptr_array_new_with_free_func(step_free); return item;
}
QuickCommand *quick_command_copy(const QuickCommand *item) {
    if (item->kind == QUICK_COMMAND) return quick_command_new(item->id, item->name, item->command, item->send_enter);
    QuickCommand *copy = quick_sequence_new(item->id, item->name);
    for (guint i = 0; i < item->steps->len; i++) {
        QuickCommandStep *step = item->steps->pdata[i];
        g_ptr_array_add(copy->steps, quick_command_step_new(step->command, step->delay_ms));
    }
    return copy;
}
void quick_command_free(gpointer data) {
    QuickCommand *item = data; if (!item) return;
    g_free(item->id); g_free(item->name); g_free(item->command);
    g_clear_pointer(&item->steps, g_ptr_array_unref); g_free(item);
}
char *commands_config_path(void) {
    const char *base = g_getenv("XDG_CONFIG_HOME");
    char *fallback = (!base || !*base) ? g_build_filename(g_get_home_dir(), ".config", NULL) : NULL;
    char *path = g_build_filename(fallback ? fallback : base, "devterm", "commands.json", NULL);
    g_free(fallback); return path;
}
static gboolean valid_text(const char *s) {
    if (!s || !g_utf8_validate(s, -1, NULL)) return FALSE;
    for (const char *p = s; *p; p = g_utf8_next_char(p)) if (!g_unichar_isspace(g_utf8_get_char(p))) return TRUE;
    return FALSE;
}
static gboolean invalid(GError **error, const char *message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message); return FALSE;
}
gboolean quick_command_validate(const QuickCommand *item, GError **error) {
    if (!item || !valid_text(item->id) || !valid_text(item->name)) return invalid(error, "ID and name are required.");
    if (item->kind == QUICK_COMMAND) return valid_text(item->command) || invalid(error, "Command is required.");
    if (item->kind != QUICK_SEQUENCE || !item->steps || !item->steps->len || item->steps->len > QUICK_SEQUENCE_MAX_STEPS)
        return invalid(error, "A sequence must contain 1–256 commands.");
    for (guint i = 0; i < item->steps->len; i++) {
        QuickCommandStep *step = item->steps->pdata[i];
        if (!step || !valid_text(step->command) || strpbrk(step->command, "\r\n") || step->delay_ms > QUICK_SEQUENCE_MAX_DELAY_MS) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Step %u requires one non-empty command line and a delay from 0 to 60000 ms.", i + 1); return FALSE;
        }
    }
    return TRUE;
}
char *quick_command_describe(const QuickCommand *item) {
    if (item->kind == QUICK_COMMAND) return g_strdup_printf("%s\n%s\n%s", item->name, item->command,
        item->send_enter ? "Send text + Enter to the selected pane" : "Insert text without Enter into the selected pane");
    GString *text = g_string_new(item->name);
    g_string_append_printf(text, "\nSequence · %u commands; Enter after every command", item->steps->len);
    for (guint i = 0; i < item->steps->len; i++) {
        QuickCommandStep *step = item->steps->pdata[i]; g_string_append_printf(text, "\n%u. %s", i + 1, step->command);
        if (i + 1 < item->steps->len) g_string_append_printf(text, "  [delay %u ms]", step->delay_ms);
    }
    g_string_append(text, "\nDelay spaces input; it does not wait for commands to finish."); return g_string_free(text, FALSE);
}
static gboolean string_member(JsonObject *object, const char *key, const char **value) {
    JsonNode *node = json_object_get_member(object, key);
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) return FALSE;
    *value = json_node_get_string(node); return valid_text(*value);
}
static GPtrArray *parse_commands(const char *data, gsize length, gint64 *format, GError **error) {
    JsonParser *parser = json_parser_new();
    GPtrArray *items = g_ptr_array_new_with_free_func(quick_command_free);
    GHashTable *ids = g_hash_table_new(g_str_hash, g_str_equal);
    if (!json_parser_load_from_data(parser, data, length, error)) goto fail;
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) goto malformed;
    JsonObject *object = json_node_get_object(root);
    JsonNode *version = json_object_get_member(object, "version"), *commands = json_object_get_member(object, "commands");
    if (!version || !JSON_NODE_HOLDS_VALUE(version) || json_node_get_value_type(version) != G_TYPE_INT64 ||
        (json_node_get_int(version) != 1 && json_node_get_int(version) != 2) || !commands || !JSON_NODE_HOLDS_ARRAY(commands)) goto malformed;
    *format = json_node_get_int(version); JsonArray *array = json_node_get_array(commands);
    for (guint i = 0; i < json_array_get_length(array); i++) {
        JsonNode *node = json_array_get_element(array, i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) goto malformed;
        JsonObject *entry = json_node_get_object(node); const char *id, *name, *kind = "command";
        if (!string_member(entry, "id", &id) || !string_member(entry, "name", &name) || g_hash_table_contains(ids, id) ||
            (*format == 2 && !string_member(entry, "kind", &kind))) goto malformed;
        QuickCommand *item;
        if (g_str_equal(kind, "command")) {
            const char *command; JsonNode *send = json_object_get_member(entry, "send_enter");
            if (!string_member(entry, "command", &command) || !send || !JSON_NODE_HOLDS_VALUE(send) || json_node_get_value_type(send) != G_TYPE_BOOLEAN) goto malformed;
            item = quick_command_new(id, name, command, json_node_get_boolean(send));
        } else if (g_str_equal(kind, "sequence") && *format == 2) {
            JsonNode *steps = json_object_get_member(entry, "steps");
            if (!steps || !JSON_NODE_HOLDS_ARRAY(steps)) goto malformed;
            JsonArray *list = json_node_get_array(steps);
            if (!json_array_get_length(list) || json_array_get_length(list) > QUICK_SEQUENCE_MAX_STEPS) goto malformed;
            item = quick_sequence_new(id, name);
            g_ptr_array_add(items, item); /* Own partial values on every error path. */
            for (guint j = 0; j < json_array_get_length(list); j++) {
                JsonNode *s = json_array_get_element(list, j); const char *command;
                if (!JSON_NODE_HOLDS_OBJECT(s)) goto malformed;
                JsonObject *step = json_node_get_object(s); JsonNode *delay = json_object_get_member(step, "delay_ms");
                if (!string_member(step, "command", &command) || !delay || !JSON_NODE_HOLDS_VALUE(delay) ||
                    json_node_get_value_type(delay) != G_TYPE_INT64 || json_node_get_int(delay) < 0 ||
                    json_node_get_int(delay) > QUICK_SEQUENCE_MAX_DELAY_MS) goto malformed;
                g_ptr_array_add(item->steps, quick_command_step_new(command, j + 1 < json_array_get_length(list) ? json_node_get_int(delay) : 0));
            }
            if (!quick_command_validate(item, error)) goto fail;
            g_hash_table_add(ids, item->id); continue;
        } else goto malformed;
        g_hash_table_add(ids, item->id); g_ptr_array_add(items, item);
    }
    g_hash_table_unref(ids); g_object_unref(parser); return items;
malformed:
    invalid(error, "Invalid Quick Commands configuration: expected version 1 or 2, unique IDs and valid command/sequence fields.");
fail:
    g_hash_table_unref(ids); g_object_unref(parser); g_ptr_array_unref(items); return NULL;
}
GPtrArray *commands_load(const char *path, GError **error) {
    char *data; gsize length; GError *local = NULL;
    if (!g_file_get_contents(path, &data, &length, &local)) {
        if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) { g_error_free(local); return g_ptr_array_new_with_free_func(quick_command_free); }
        g_propagate_error(error, local); return NULL;
    }
    gint64 format; GPtrArray *items = parse_commands(data, length, &format, error); g_free(data); return items;
}
gboolean commands_save(const char *path, GPtrArray *items, GError **error) {
    GHashTable *ids = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < items->len; i++) {
        QuickCommand *item = items->pdata[i];
        if (!quick_command_validate(item, error)) { g_hash_table_unref(ids); return FALSE; }
        if (g_hash_table_contains(ids, item->id)) { g_hash_table_unref(ids); return invalid(error, "Quick Command IDs must be unique."); }
        g_hash_table_add(ids, item->id);
    }
    g_hash_table_unref(ids);
    char *before = NULL; gsize before_size = 0; gint64 format = 0; GError *local = NULL;
    if (g_file_get_contents(path, &before, &before_size, &local)) {
        GPtrArray *existing = parse_commands(before, before_size, &format, error);
        if (!existing) { g_free(before); return FALSE; } g_ptr_array_unref(existing);
    } else if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) g_clear_error(&local);
    else { g_propagate_error(error, local); return FALSE; }
    if (format == 1) {
        char *uuid = g_uuid_string_random(), *backup = g_strdup_printf("%s.v1-%s.bak", path, uuid); g_free(uuid);
        gboolean ok = g_file_set_contents_full(backup, before, before_size,
            G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
        g_free(backup); if (!ok) { g_free(before); return FALSE; }
    }
    g_free(before);
    JsonBuilder *builder = json_builder_new(); json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "version"); json_builder_add_int_value(builder, 2);
    json_builder_set_member_name(builder, "commands"); json_builder_begin_array(builder);
    for (guint i = 0; i < items->len; i++) {
        QuickCommand *item = items->pdata[i]; json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id"); json_builder_add_string_value(builder, item->id);
        json_builder_set_member_name(builder, "name"); json_builder_add_string_value(builder, item->name);
        json_builder_set_member_name(builder, "kind"); json_builder_add_string_value(builder, item->kind == QUICK_COMMAND ? "command" : "sequence");
        if (item->kind == QUICK_COMMAND) {
            json_builder_set_member_name(builder, "command"); json_builder_add_string_value(builder, item->command);
            json_builder_set_member_name(builder, "send_enter"); json_builder_add_boolean_value(builder, item->send_enter);
        } else {
            json_builder_set_member_name(builder, "steps"); json_builder_begin_array(builder);
            for (guint j = 0; j < item->steps->len; j++) {
                QuickCommandStep *step = item->steps->pdata[j]; json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "command"); json_builder_add_string_value(builder, step->command);
                json_builder_set_member_name(builder, "delay_ms"); json_builder_add_int_value(builder, j + 1 < item->steps->len ? step->delay_ms : 0);
                json_builder_end_object(builder);
            }
            json_builder_end_array(builder);
        }
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder); json_builder_end_object(builder);
    JsonNode *root = json_builder_get_root(builder); JsonGenerator *generator = json_generator_new();
    json_generator_set_root(generator, root); json_generator_set_pretty(generator, TRUE);
    gsize length; char *data = json_generator_to_data(generator, &length), *dir = g_path_get_dirname(path);
    gboolean ok = g_mkdir_with_parents(dir, 0700) == 0;
    if (!ok) g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot create %s: %s", dir, g_strerror(errno));
    if (ok) ok = g_file_set_contents_full(path, data, length, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
    g_free(dir); g_free(data); json_node_free(root); g_object_unref(generator); g_object_unref(builder); return ok;
}
