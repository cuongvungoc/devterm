#include "shortcuts-ui.h"
#include "dialog-layout.h"
#include <glib/gstdio.h>

struct _ShortcutSettings {
    GObject parent_instance;
    GtkApplication *application; /* Owner. */
    ShortcutBindings *bindings;
    char *path, *load_error;
    GBytes *disk;
    gboolean disk_known;
    GtkWidget *dialog; /* Borrowed; editor owns a settings reference. */
};
G_DEFINE_TYPE(ShortcutSettings, shortcut_settings, G_TYPE_OBJECT)
static guint changed_signal;
static void shortcut_settings_finalize(GObject *object) {
    ShortcutSettings *s = SHORTCUT_SETTINGS(object);
    shortcut_bindings_free(s->bindings); g_free(s->path); g_free(s->load_error); g_clear_pointer(&s->disk, g_bytes_unref);
    G_OBJECT_CLASS(shortcut_settings_parent_class)->finalize(object);
}
static void shortcut_settings_class_init(ShortcutSettingsClass *klass) {
    G_OBJECT_CLASS(klass)->finalize = shortcut_settings_finalize;
    changed_signal = g_signal_new("changed", TYPE_SHORTCUT_SETTINGS, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void shortcut_settings_init(ShortcutSettings *s) { (void)s; }
/* NULL bytes means a missing file, distinct from a zero-length file. */
static gboolean read_disk(const char *path, GBytes **bytes, GError **error) {
    char *text; gsize length; GError *local = NULL; *bytes = NULL;
    if (g_file_get_contents(path, &text, &length, &local)) { *bytes = g_bytes_new_take(text, length); return TRUE; }
    if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) { g_error_free(local); return TRUE; }
    g_propagate_error(error, local); return FALSE;
}
static void register_accels(ShortcutSettings *s) {
    for (int i = 0; i < SC_COUNT; i++) {
        const ShortcutDefinition *d = shortcut_definition(i);
        char *action = g_strconcat("win.", d->id, NULL);
        /* Copy/paste are dispatched only in terminal focus, never globally. */
        const char *keys[] = {d->terminal_only ? NULL : s->bindings->keys[i], NULL};
        gtk_application_set_accels_for_action(s->application, action, keys); g_free(action);
    }
}
ShortcutSettings *shortcut_settings_get(GtkApplication *application) {
    ShortcutSettings *s = g_object_get_data(G_OBJECT(application), "shortcut-settings");
    if (s) return s;
    s = g_object_new(TYPE_SHORTCUT_SETTINGS, NULL); s->application = application; s->path = shortcuts_config_path();
    s->disk_known = read_disk(s->path, &s->disk, NULL);
    GError *error = NULL; s->bindings = shortcuts_load(s->path, &error);
    if (!s->bindings) {
        s->load_error = g_strdup_printf("Cannot load %s: %s. Defaults are active; the file has been preserved.", s->path, error->message);
        g_clear_error(&error); s->bindings = shortcut_bindings_new();
    }
    register_accels(s);
    g_object_set_data_full(G_OBJECT(application), "shortcut-settings", s, g_object_unref); return s;
}
const ShortcutBindings *shortcut_settings_bindings(ShortcutSettings *s) { return s->bindings; }
const char *shortcut_settings_error(ShortcutSettings *s) { return s->load_error; }
gboolean shortcut_settings_apply(ShortcutSettings *s, const ShortcutBindings *bindings, gboolean recover, GError **error) {
    if (!shortcut_bindings_validate(bindings, error)) return FALSE;
    if (s->load_error && !recover) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "The original configuration is invalid. Use Restore Defaults & Back Up Invalid File before applying."); return FALSE;
    }
    GBytes *disk = NULL;
    if (!read_disk(s->path, &disk, error)) return FALSE;
    gboolean same = s->disk_known && ((!disk && !s->disk) || (disk && s->disk && g_bytes_equal(disk, s->disk)));
    g_clear_pointer(&disk, g_bytes_unref);
    if (!same) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "The shortcuts file changed externally or could not be read at startup. Restart DevTerm to reload it before saving."); return FALSE;
    }
    if (s->load_error && s->disk) {
        char *uuid = g_uuid_string_random(), *backup = g_strdup_printf("%s.invalid-%s.bak", s->path, uuid);
        GFile *source = g_file_new_for_path(s->path), *target = g_file_new_for_path(backup);
        gboolean ok = g_file_copy(source, target, G_FILE_COPY_NONE, NULL, NULL, NULL, error);
        g_object_unref(source); g_object_unref(target); g_free(uuid); g_free(backup);
        if (!ok) return FALSE;
    }
    if (!shortcuts_save(s->path, bindings, error)) return FALSE;
    ShortcutBindings *next = shortcut_bindings_copy(bindings);
    shortcut_bindings_free(s->bindings); s->bindings = next;
    g_clear_pointer(&s->load_error, g_free); g_clear_pointer(&s->disk, g_bytes_unref);
    s->disk_known = read_disk(s->path, &s->disk, NULL);
    register_accels(s); g_signal_emit(s, changed_signal, 0); return TRUE;
}
void shortcut_settings_tooltip(ShortcutSettings *s, GtkWidget *widget, ShortcutId id, const char *description) {
    char *label = shortcut_label(s->bindings->keys[id]);
    char *text = s->bindings->keys[id] ? g_strdup_printf("%s (%s)", description, label) : g_strdup(description);
    gtk_widget_set_tooltip_text(widget, text); g_free(text); g_free(label);
}

typedef struct {
    ShortcutSettings *settings;
    ShortcutBindings *draft;
    GtkWidget *dialog, *search, *message, *apply, *reassign, *reset_all, *help;
    GtkWidget *rows[SC_COUNT], *keys[SC_COUNT], *states[SC_COUNT];
    GtkSizeGroup *key_width;
    int recording, conflict, candidate_id;
    char *candidate;
    gboolean recover;
} Editor;
static void editor_message(Editor *e, const char *text, gboolean error) {
    gtk_label_set_text(GTK_LABEL(e->message), text); gtk_widget_set_visible(e->message, text && *text);
    if (error) gtk_widget_add_css_class(e->message, "error"); else gtk_widget_remove_css_class(e->message, "error");
}
static void clear_candidate(Editor *e) {
    g_clear_pointer(&e->candidate, g_free); e->conflict = -1;
    gtk_widget_set_visible(e->reassign, FALSE);
}
static void refresh(Editor *e) {
    ShortcutBindings *defaults = shortcut_bindings_new();
    for (int i = 0; i < SC_COUNT; i++) {
        char *key = shortcut_label(e->draft->keys[i]);
        gtk_button_set_label(GTK_BUTTON(e->keys[i]), i == e->recording ? "Press keys…" : key);
        gtk_widget_set_tooltip_text(e->keys[i], i == e->recording ? "Press a shortcut; Esc cancels recording." : key);
        gtk_label_set_text(GTK_LABEL(e->states[i]), g_strcmp0(e->draft->keys[i], defaults->keys[i]) ? "Customized" : "Default");
        g_free(key);
    }
    shortcut_bindings_free(defaults);
    gboolean pending = e->recording >= 0 || e->candidate != NULL;
    gtk_widget_set_sensitive(e->apply, !pending && (!e->settings->load_error || e->recover));
}
static void propose(Editor *e, int id, const char *key) {
    clear_candidate(e); e->recording = -1;
    GError *error = NULL; char *normalized = shortcut_normalize(key, &error);
    if (error) { editor_message(e, error->message, TRUE); g_clear_error(&error); refresh(e); return; }
    int conflict = shortcut_bindings_conflict(e->draft, id, normalized);
    if (conflict >= 0) {
        e->candidate = g_strdup(normalized); e->candidate_id = id; e->conflict = conflict;
        char *label = shortcut_label(normalized);
        char *message = g_strdup_printf("%s is used by %s. Reassign moves it to %s; the previous action becomes unassigned. Change another shortcut to cancel.",
            label, shortcut_definition(conflict)->label, shortcut_definition(id)->label);
        editor_message(e, message, TRUE); g_free(message); g_free(label);
        gtk_widget_set_visible(e->reassign, TRUE);
    } else {
        shortcut_bindings_set(e->draft, id, normalized, NULL);
        editor_message(e, "Pending changes. Apply saves and activates them in all windows. Assigned keys are intercepted before the terminal; check for shell/TUI conflicts.", FALSE);
    }
    g_free(normalized); refresh(e);
}
static int row_id(GtkWidget *button) { return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "shortcut-id")); }
static void change_clicked(GtkButton *button, gpointer data) {
    Editor *e = data; clear_candidate(e); e->recording = row_id(GTK_WIDGET(button));
    editor_message(e, "Press a shortcut. Esc cancels recording. Use Ctrl, Alt or Super with a key, or a function key.", FALSE); refresh(e);
}
static void remove_clicked(GtkButton *button, gpointer data) { propose(data, row_id(GTK_WIDGET(button)), NULL); }
static void reset_clicked(GtkButton *button, gpointer data) {
    int id = row_id(GTK_WIDGET(button)); propose(data, id, shortcut_definition(id)->default_key);
}
static void reassign_clicked(GtkButton *button, gpointer data) {
    (void)button; Editor *e = data;
    if (e->candidate && e->conflict >= 0) {
        char *key = g_strdup(e->candidate); int id = e->candidate_id;
        shortcut_bindings_set(e->draft, e->conflict, NULL, NULL); propose(e, id, key); g_free(key);
    }
}
static void reset_all_clicked(GtkButton *button, gpointer data) {
    (void)button; Editor *e = data; clear_candidate(e); e->recording = -1;
    shortcut_bindings_free(e->draft); e->draft = shortcut_bindings_new();
    e->recover = e->settings->load_error != NULL;
    editor_message(e, e->recover ? "Apply will back up the original invalid file as shortcuts.json.invalid-<unique-id>.bak, then save these defaults. Cancel leaves the original untouched." :
        "Defaults restored in this preview. Apply to save, or Cancel to keep your existing settings.", FALSE); refresh(e);
}
static gboolean modifier_key(guint key) {
    return key == GDK_KEY_Control_L || key == GDK_KEY_Control_R || key == GDK_KEY_Shift_L || key == GDK_KEY_Shift_R ||
        key == GDK_KEY_Alt_L || key == GDK_KEY_Alt_R || key == GDK_KEY_Super_L || key == GDK_KEY_Super_R ||
        key == GDK_KEY_Meta_L || key == GDK_KEY_Meta_R || key == GDK_KEY_ISO_Level3_Shift || key == GDK_KEY_Caps_Lock;
}
static gboolean editor_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state, gpointer data) {
    (void)code; (void)state; Editor *e = data;
    GtkMenuButton *info = GTK_MENU_BUTTON(gtk_widget_get_last_child(e->help));
    GtkPopover *help = gtk_menu_button_get_popover(info);
    if (gtk_widget_get_visible(GTK_WIDGET(help))) {
        if (key == GDK_KEY_Escape) { gtk_popover_popdown(help); return TRUE; }
        return FALSE;
    }
    if (key == GDK_KEY_Escape) {
        if (e->recording >= 0 || e->candidate) {
            e->recording = -1; clear_candidate(e); editor_message(e, "Shortcut change cancelled.", FALSE); refresh(e);
        } else gtk_dialog_response(GTK_DIALOG(e->dialog), GTK_RESPONSE_CANCEL);
        return TRUE;
    }
    if (e->recording < 0) return FALSE;
    if (modifier_key(key)) return TRUE;
    GError *error = NULL;
    char *name = shortcut_from_event(gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller)), &error);
    if (!name) { editor_message(e, error->message, TRUE); g_clear_error(&error); return TRUE; }
    propose(e, e->recording, name); g_free(name); return TRUE;
}
static void filter_rows(GtkEditable *editable, gpointer data) {
    Editor *e = data; char *query = g_utf8_casefold(gtk_editable_get_text(editable), -1);
    for (int i = 0; i < SC_COUNT; i++) {
        const ShortcutDefinition *d = shortcut_definition(i);
        char *text = g_strdup_printf("%s %s %s", d->label, d->group, d->id), *folded = g_utf8_casefold(text, -1);
        gtk_widget_set_visible(e->rows[i], strstr(folded, query) != NULL); g_free(text); g_free(folded);
    }
    g_free(query);
}
static void editor_destroyed(GtkWidget *dialog, gpointer data) {
    (void)dialog; Editor *e = data; e->settings->dialog = NULL;
    shortcut_bindings_free(e->draft); g_free(e->candidate); g_object_unref(e->settings); g_object_unref(e->key_width); g_free(e);
}
static void editor_response(GtkDialog *dialog, int response, gpointer data) {
    Editor *e = data;
    if (response != GTK_RESPONSE_APPLY) { gtk_window_destroy(GTK_WINDOW(dialog)); return; }
    if (e->recording >= 0 || e->candidate) return;
    GError *error = NULL;
    if (!shortcut_settings_apply(e->settings, e->draft, e->recover, &error)) {
        editor_message(e, error->message, TRUE); g_clear_error(&error); return;
    }
    e->recover = FALSE; gtk_button_set_label(GTK_BUTTON(e->reset_all), "Reset All");
    editor_message(e, "Shortcuts saved and active in all windows.", FALSE); refresh(e);
}
static GtkWidget *editor_button(Editor *e, GtkBox *row, const char *label, int id, GCallback callback) {
    GtkWidget *button = gtk_button_new_with_label(label);
    g_object_set_data(G_OBJECT(button), "shortcut-id", GINT_TO_POINTER(id));
    g_signal_connect(button, "clicked", callback, e); gtk_box_append(row, button);
    if (id >= 0) {
        char *accessible = g_strdup_printf("%s: %s", label, shortcut_definition(id)->label);
        gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, accessible, -1); g_free(accessible);
    }
    return button;
}
void shortcut_settings_show(ShortcutSettings *settings, GtkWindow *parent) {
    if (settings->dialog) { gtk_window_present(GTK_WINDOW(settings->dialog)); return; }
    Editor *e = g_new0(Editor, 1); e->settings = g_object_ref(settings);
    e->key_width = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    e->draft = shortcut_bindings_copy(settings->bindings); e->recording = e->conflict = -1;
    e->dialog = settings->dialog = gtk_dialog_new_with_buttons("Keyboard Shortcuts", parent, GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL, "Apply", GTK_RESPONSE_APPLY, NULL);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(e->dialog), TRUE);
    dialog_layout_apply(GTK_WINDOW(e->dialog), "shortcuts", 960, 800);
    gtk_widget_add_css_class(e->dialog, "devterm-dialog");
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(e->dialog)); gtk_box_set_spacing(GTK_BOX(box), 8);
    gtk_widget_set_vexpand(box, TRUE);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16); gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    GtkWidget *intro = e->help = dialog_layout_help("Click a shortcut to change it. Apply saves your changes.",
        "Copy/Paste shortcuts apply only when a terminal has focus. Esc cancels recording. "
        "Cancel discards changes since the last Apply. Desktop-reserved keys may not reach DevTerm.\n\n"
        "Text editing uses GTK defaults: Draft Undo Ctrl+Z, Redo Ctrl+Shift+Z. "
        "Search: Enter / Shift+Enter, Esc to close.");
    gtk_box_append(GTK_BOX(box), intro);
    e->search = gtk_search_entry_new(); gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(e->search), "Search actions or groups…");
    gtk_box_append(GTK_BOX(box), e->search); g_signal_connect(e->search, "changed", G_CALLBACK(filter_rows), e);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *list = gtk_list_box_new(); gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "settings-list"); gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list); gtk_box_append(GTK_BOX(box), scroll);
    for (int i = 0; i < SC_COUNT; i++) {
        const ShortcutDefinition *d = shortcut_definition(i);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(row, 8); gtk_widget_set_margin_end(row, 8);
        GtkWidget *name = gtk_label_new(d->label); gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END); gtk_widget_set_tooltip_text(name, d->label);
        gtk_widget_set_hexpand(name, TRUE);
        GtkWidget *meta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8), *group = gtk_label_new(d->group);
        gtk_widget_add_css_class(meta, "muted"); gtk_widget_add_css_class(meta, "caption"); gtk_box_append(GTK_BOX(meta), group);
        e->states[i] = gtk_label_new(""); gtk_box_append(GTK_BOX(meta), e->states[i]);
        GtkWidget *names = dialog_layout_pair(name, meta, 400, 440, 65);
        gtk_widget_set_hexpand(names, TRUE); gtk_box_append(GTK_BOX(row), names);
        e->keys[i] = editor_button(e, GTK_BOX(row), "Change", i, G_CALLBACK(change_clicked));
        gtk_size_group_add_widget(e->key_width, e->keys[i]);
        editor_button(e, GTK_BOX(row), "Remove", i, G_CALLBACK(remove_clicked));
        editor_button(e, GTK_BOX(row), "Reset", i, G_CALLBACK(reset_clicked));
        gtk_list_box_append(GTK_LIST_BOX(list), row); e->rows[i] = gtk_widget_get_parent(row);
    }
    e->message = gtk_label_new(""); gtk_label_set_wrap(GTK_LABEL(e->message), TRUE); gtk_label_set_xalign(GTK_LABEL(e->message), 0); gtk_box_append(GTK_BOX(box), e->message);
    GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *apply_button = gtk_dialog_get_widget_for_response(GTK_DIALOG(e->dialog), GTK_RESPONSE_APPLY);
    GtkWidget *actions = gtk_widget_get_parent(apply_button); gtk_widget_set_hexpand(tools, TRUE);
    gtk_widget_set_halign(actions, GTK_ALIGN_FILL); gtk_box_prepend(GTK_BOX(actions), tools);
    e->reset_all = editor_button(e, GTK_BOX(tools), settings->load_error ? "Restore Defaults & Back Up Invalid File" : "Reset All", -1, G_CALLBACK(reset_all_clicked));
    e->reassign = editor_button(e, GTK_BOX(tools), "Reassign", -1, G_CALLBACK(reassign_clicked)); gtk_widget_set_visible(e->reassign, FALSE);
    e->apply = gtk_dialog_get_widget_for_response(GTK_DIALOG(e->dialog), GTK_RESPONSE_APPLY);
    gtk_widget_add_css_class(e->apply, "suggested-action");
    GtkEventController *keys = gtk_event_controller_key_new(); gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(editor_key), e); gtk_widget_add_controller(e->dialog, keys);
    g_signal_connect(e->dialog, "response", G_CALLBACK(editor_response), e); g_signal_connect(e->dialog, "destroy", G_CALLBACK(editor_destroyed), e);
    editor_message(e, settings->load_error ? settings->load_error : "", settings->load_error != NULL);
    refresh(e); gtk_window_present(GTK_WINDOW(e->dialog));
}
