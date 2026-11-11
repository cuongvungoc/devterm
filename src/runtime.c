#include "runtime.h"
#include <gio/gio.h>

gboolean runtime_init_schemas(void) {
    char *self = g_file_read_link("/proc/self/exe", NULL);
    char *directory = self ? g_path_get_dirname(self) : NULL;
    char *schemas = directory ? g_build_filename(directory, "runtime", "share", "glib-2.0", "schemas", NULL) : NULL;
    char *compiled = schemas ? g_build_filename(schemas, "gschemas.compiled", NULL) : NULL;
    if (!compiled || !g_file_test(compiled, G_FILE_TEST_IS_REGULAR)) {
        /* Meson tests run build-tree binaries against this installed SDK. */
        g_free(schemas);
        schemas = g_strdup("/usr/lib/devterm/runtime/share/glib-2.0/schemas");
        g_free(compiled); compiled = g_build_filename(schemas, "gschemas.compiled", NULL);
    }
    gboolean ready = FALSE;
    if (g_file_test(compiled, G_FILE_TEST_IS_REGULAR)) {
        char *original = g_strdup(g_getenv("GSETTINGS_SCHEMA_DIR"));
        g_setenv("GSETTINGS_SCHEMA_DIR", schemas, TRUE);
        /* GLib caches its default schema-source chain on first access. Loading
         * here keeps GTK's own schemas alongside the host's desktop schemas,
         * while restoring the exact environment before any PTY/client starts. */
        GSettingsSchemaSource *source = g_settings_schema_source_get_default();
        GSettingsSchema *schema = source ? g_settings_schema_source_lookup(source, "org.gtk.gtk4.Settings.FileChooser", TRUE) : NULL;
        ready = schema != NULL;
        if (schema) g_settings_schema_unref(schema);
        if (original) g_setenv("GSETTINGS_SCHEMA_DIR", original, TRUE);
        else g_unsetenv("GSETTINGS_SCHEMA_DIR");
        g_free(original);
    }
    if (!ready) g_printerr("DevTerm private GTK schemas are missing or invalid. Reinstall the matching DevTerm package.\n");
    g_free(compiled); g_free(schemas); g_free(directory); g_free(self);
    return ready;
}
