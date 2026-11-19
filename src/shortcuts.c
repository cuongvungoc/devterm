#include "shortcuts.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <errno.h>

static const ShortcutDefinition definitions[SC_COUNT] = {
    {"reconnect-pane", "Reconnect Current Pane", "Session", "<Control><Shift>r", FALSE},
    {"disconnect-pane", "Disconnect Current Pane", "Session", "<Control><Shift>d", FALSE},
    {"close-pane", "Close Pane", "Pane", "<Control><Shift>x", FALSE},
    {"split-right", "Split Right", "Pane", "<Control><Shift>e", FALSE},
    {"split-down", "Split Down", "Pane", "<Control><Shift>o", FALSE},
    {"next-pane", "Next Pane", "Pane", "<Control><Shift>Page_Down", FALSE},
    {"previous-pane", "Previous Pane", "Pane", "<Control><Shift>Page_Up", FALSE},
    {"pane-left", "Select Pane to the Left", "Pane", NULL, FALSE},
    {"pane-right", "Select Pane to the Right", "Pane", NULL, FALSE},
    {"pane-up", "Select Pane Above", "Pane", NULL, FALSE},
    {"pane-down", "Select Pane Below", "Pane", NULL, FALSE},
    {"new-tab", "New Terminal Tab", "Tab", "<Control><Shift>t", FALSE},
    {"close-tab", "Close Tab", "Tab", "<Control><Shift>w", FALSE},
    {"next-tab", "Next Tab", "Tab", "<Control>Page_Down", FALSE},
    {"previous-tab", "Previous Tab", "Tab", "<Control>Page_Up", FALSE},
    {"rename-tab", "Rename Tab…", "Tab", "<Control><Shift>F2", FALSE},
    {"move-tab-left", "Move Tab Left", "Tab", NULL, FALSE},
    {"move-tab-right", "Move Tab Right", "Tab", NULL, FALSE},
    {"copy", "Copy Terminal Selection", "Text", "<Control><Shift>c", TRUE},
    {"paste", "Paste into Terminal", "Text", "<Control><Shift>v", TRUE},
    {"copy-all", "Copy All Text", "Text", "<Control><Shift>a", FALSE},
    {"export-text", "Export Text…", "Text", "<Control><Shift>s", FALSE},
    {"find-in-pane", "Find in Pane", "Text", "<Control><Shift>f", FALSE},
    {"sessions", "Sessions", "Tools", "<Control><Shift>k", FALSE},
    {"toggle-draft", "Show / Hide Draft", "Tools", "<Control><Shift>m", FALSE},
    {"quick-manage", "Manage Quick Commands", "Tools", "<Control><Shift>p", FALSE},
    {"quick-add", "Add Quick Command", "Tools", NULL, FALSE},
    {"shortcuts", "Keyboard Shortcuts", "Tools", "<Control><Shift>comma", FALSE}
};
const ShortcutDefinition *shortcut_definition(ShortcutId id) { g_return_val_if_fail(id < SC_COUNT, NULL); return &definitions[id]; }
static gboolean invalid(GError **error, const char *message) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message); return FALSE;
}
char *shortcut_normalize(const char *key, GError **error) {
    if (!key || !*key) return NULL;
    guint val; GdkModifierType mods;
    if (!gtk_accelerator_parse(key, &val, &mods) || !val) { invalid(error, "Invalid shortcut syntax."); return NULL; }
    val = gdk_keyval_to_lower(val);
    if (mods & ~gtk_accelerator_get_default_mod_mask()) { invalid(error, "Unsupported shortcut modifier."); return NULL; }
    if (!(mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK | GDK_HYPER_MASK)) &&
        !(val >= GDK_KEY_F1 && val <= GDK_KEY_F35)) {
        invalid(error, "Use Ctrl, Alt or Super with a key, or a function key. Plain typing and Shift-only text keys are reserved for input."); return NULL;
    }
    if (!gtk_accelerator_valid(val, mods) || val == GDK_KEY_Escape) { invalid(error, "This key is reserved for navigation or cancelling."); return NULL; }
    return gtk_accelerator_name(val, mods);
}
ShortcutBindings *shortcut_bindings_new(void) {
    ShortcutBindings *b = g_new0(ShortcutBindings, 1);
    for (int i = 0; i < SC_COUNT; i++) b->keys[i] = shortcut_normalize(definitions[i].default_key, NULL);
    return b;
}
ShortcutBindings *shortcut_bindings_copy(const ShortcutBindings *b) {
    ShortcutBindings *copy = g_new0(ShortcutBindings, 1);
    for (int i = 0; i < SC_COUNT; i++) copy->keys[i] = g_strdup(b->keys[i]);
    return copy;
}
void shortcut_bindings_free(ShortcutBindings *b) {
    if (!b) return;
    for (int i = 0; i < SC_COUNT; i++) g_free(b->keys[i]);
    g_free(b);
}
int shortcut_bindings_conflict(const ShortcutBindings *b, ShortcutId except, const char *key) {
    if (key) for (int i = 0; i < SC_COUNT; i++)
        if (i != (int)except && g_strcmp0(b->keys[i], key) == 0) return i;
    return -1;
}
gboolean shortcut_bindings_set(ShortcutBindings *b, ShortcutId id, const char *key, GError **error) {
    g_return_val_if_fail(id < SC_COUNT, FALSE);
    char *normalized = shortcut_normalize(key, error);
    if (key && *key && !normalized) return FALSE;
    int other = shortcut_bindings_conflict(b, id, normalized);
    if (other >= 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "Shortcut is already used by %s.", definitions[other].label);
        g_free(normalized); return FALSE;
    }
    g_free(b->keys[id]); b->keys[id] = normalized; return TRUE;
}
gboolean shortcut_bindings_validate(const ShortcutBindings *b, GError **error) {
    for (int i = 0; i < SC_COUNT; i++) {
        char *key = shortcut_normalize(b->keys[i], error);
        if (b->keys[i] && !key) return FALSE;
        if (shortcut_bindings_conflict(b, i, key) >= 0) { g_free(key); return invalid(error, "Two actions use the same shortcut. Reassign or remove one binding."); }
        g_free(key);
    }
    return TRUE;
}
char *shortcuts_config_path(void) {
    const char *base = g_getenv("XDG_CONFIG_HOME");
    char *fallback = (!base || !*base) ? g_build_filename(g_get_home_dir(), ".config", NULL) : NULL;
    char *path = g_build_filename(fallback ? fallback : base, "devterm", "shortcuts.json", NULL);
    g_free(fallback); return path;
}
ShortcutBindings *shortcuts_load(const char *path, GError **error) {
    ShortcutBindings *b = shortcut_bindings_new();
    JsonParser *parser = json_parser_new();
    GError *local = NULL;
    if (!json_parser_load_from_file(parser, path, &local)) {
        if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) { g_clear_error(&local); g_object_unref(parser); return b; }
        g_propagate_error(error, local); goto fail;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) goto malformed;
    JsonObject *object = json_node_get_object(root);
    JsonNode *version = json_object_get_member(object, "version"), *bindings = json_object_get_member(object, "bindings");
    if (!version || !JSON_NODE_HOLDS_VALUE(version) || json_node_get_value_type(version) != G_TYPE_INT64 ||
        json_node_get_int(version) != 1 || !bindings || !JSON_NODE_HOLDS_OBJECT(bindings)) goto malformed;
    JsonObject *values = json_node_get_object(bindings);
    /* Overlay all keys before checking conflicts, so swaps are order independent. */
    GList *members = json_object_get_members(values);
    for (GList *p = members; p; p = p->next) {
        int id = -1;
        for (int i = 0; i < SC_COUNT; i++) if (g_str_equal(p->data, definitions[i].id)) { id = i; break; }
        if (id < 0) { g_list_free(members); goto malformed; }
        JsonNode *node = json_object_get_member(values, p->data);
        char *key = NULL;
        if (!JSON_NODE_HOLDS_NULL(node)) {
            if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING || !*json_node_get_string(node)) { g_list_free(members); goto malformed; }
            key = shortcut_normalize(json_node_get_string(node), error);
            if (!key) { g_list_free(members); goto fail; }
        }
        g_free(b->keys[id]); b->keys[id] = key;
    }
    g_list_free(members);
    if (!shortcut_bindings_validate(b, error)) goto fail;
    g_object_unref(parser); return b;
malformed:
    invalid(error, "Invalid shortcuts configuration: expected version 1 and known action IDs mapped to shortcut strings or null.");
fail:
    g_object_unref(parser); shortcut_bindings_free(b); return NULL;
}
gboolean shortcuts_save(const char *path, const ShortcutBindings *b, GError **error) {
    if (!shortcut_bindings_validate(b, error)) return FALSE;
    ShortcutBindings *defaults = shortcut_bindings_new();
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "version"); json_builder_add_int_value(builder, 1);
    json_builder_set_member_name(builder, "bindings"); json_builder_begin_object(builder);
    for (int i = 0; i < SC_COUNT; i++) if (g_strcmp0(b->keys[i], defaults->keys[i]) != 0) {
        json_builder_set_member_name(builder, definitions[i].id);
        if (b->keys[i]) json_builder_add_string_value(builder, b->keys[i]); else json_builder_add_null_value(builder);
    }
    json_builder_end_object(builder); json_builder_end_object(builder);
    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *gen = json_generator_new(); json_generator_set_root(gen, root); json_generator_set_pretty(gen, TRUE);
    gsize length; char *text = json_generator_to_data(gen, &length);
    char *dir = g_path_get_dirname(path);
    gboolean ok = g_mkdir_with_parents(dir, 0700) == 0;
    if (!ok) g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot create %s: %s", dir, g_strerror(errno));
    if (ok) ok = g_file_set_contents_full(path, text, length, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error);
    g_free(dir); g_free(text); g_object_unref(gen); json_node_free(root); g_object_unref(builder); shortcut_bindings_free(defaults);
    return ok;
}
char *shortcut_label(const char *key) {
    guint val; GdkModifierType mods;
    if (!key || !gtk_accelerator_parse(key, &val, &mods)) return g_strdup("Unassigned");
    return gtk_accelerator_get_label(val, mods);
}
char *shortcut_from_event(GdkEvent *event, GError **error) {
    guint val = gdk_key_event_get_keyval(event);
    GdkModifierType mods = gdk_event_get_modifier_state(event) & gtk_accelerator_get_default_mod_mask();
    /* Store the base symbol plus Shift (e.g. Shift+comma, not Shift+less).
     * Preserve the active layout/group, including non-US keyboards. */
    guint base;
    if (gdk_display_translate_key(gdk_event_get_display(event), gdk_key_event_get_keycode(event),
        mods & ~GDK_SHIFT_MASK, gdk_key_event_get_layout(event), &base, NULL, NULL, NULL)) val = base;
    char *name = gtk_accelerator_name(gdk_keyval_to_lower(val), mods);
    char *key = shortcut_normalize(name, error); g_free(name); return key;
}
int shortcut_match_event(const ShortcutBindings *b, GdkEvent *event) {
    char *normalized = shortcut_from_event(event, NULL);
    if (normalized) {
        for (int i = 0; i < SC_COUNT; i++) if (g_strcmp0(b->keys[i], normalized) == 0) { g_free(normalized); return i; }
        g_free(normalized);
    }
    int partial = -1;
    for (int i = 0; i < SC_COUNT; i++) if (b->keys[i]) {
        guint key; GdkModifierType mods; gtk_accelerator_parse(b->keys[i], &key, &mods);
        GdkKeyMatch match = gdk_key_event_matches(event, key, mods);
        if (match == GDK_KEY_MATCH_EXACT) return i;
        if (match == GDK_KEY_MATCH_PARTIAL && partial < 0) partial = i;
    }
    return partial;
}
