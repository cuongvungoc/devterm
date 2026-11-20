/* Exercise startup, menu actions and CSS through the actual application. */
#define main devterm_application_main
#include "../src/main.c"
#undef main
#include <glib/gstdio.h>

static char *config;
static gboolean expected_dark;
static guint mapped, painted;

static void settle(void) {
    gint64 end = g_get_monotonic_time() + 180000;
    while (g_get_monotonic_time() < end) {
        while (g_main_context_iteration(NULL, FALSE)) {}
        g_usleep(1000);
    }
}
static void assert_appearance(App *app, gboolean dark) {
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(app->window), "dark-appearance");
    GVariant *state = g_action_get_state(action);
    g_assert_cmpint(g_variant_get_boolean(state), ==, dark); g_variant_unref(state);
    g_assert_cmpint(app->preferences->appearance, ==, dark ? PREFERENCES_APPEARANCE_DARK : PREFERENCES_APPEARANCE_LIGHT);
    gboolean setting; g_object_get(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", &setting, NULL);
    g_assert_cmpint(setting, ==, dark);
    GdkRGBA actual, expected; g_assert_true(gdk_rgba_parse(&expected, dark ? "#111722" : "#F2F5F9"));
    g_assert_true(gtk_style_context_lookup_color(gtk_widget_get_style_context(app->window), "dt_bg", &actual));
    g_assert_true(gdk_rgba_equal(&actual, &expected));
}
static void after_paint(GdkFrameClock *clock, gpointer data) {
    App *app = data; assert_appearance(app, expected_dark); painted++;
    g_signal_handlers_disconnect_by_func(clock, after_paint, data);
}
static void on_map(GtkWidget *window, gpointer data) {
    (void)data; App *app = g_object_get_data(G_OBJECT(window), "app");
    assert_appearance(app, expected_dark); mapped++;
    g_signal_connect(gtk_widget_get_frame_clock(window), "after-paint", G_CALLBACK(after_paint), app);
}
static void on_window_added(GtkApplication *application, GtkWindow *window, gpointer data) {
    (void)application; (void)data; g_signal_connect(window, "map", G_CALLBACK(on_map), NULL);
}
static GtkApplication *new_application(void) {
    static guint serial;
    char *id = g_strdup_printf("com.example.DevTerm.AppearanceTests.Instance%u", ++serial);
    GtkApplication *application = gtk_application_new(id, G_APPLICATION_NON_UNIQUE); g_free(id);
    GError *error = NULL; g_application_register(G_APPLICATION(application), NULL, &error);
    g_assert_no_error(error); return application;
}
static App *present(GtkApplication *application) {
    activate(application, NULL); settle();
    return g_object_get_data(G_OBJECT(gtk_application_get_active_window(application)), "app");
}
static void destroy_windows(GtkApplication *application) {
    while (gtk_application_get_windows(application))
        gtk_window_destroy(gtk_application_get_windows(application)->data);
    settle(); g_object_unref(application);
}
static void toggle(App *app, const char *name) {
    g_action_group_activate_action(G_ACTION_GROUP(app->window), name, NULL); settle();
}
static void assert_saved(gboolean dark, gboolean confirm) {
    char *path = preferences_config_path(); Preferences *saved = preferences_load(path, NULL);
    g_assert_nonnull(saved); g_assert_cmpint(saved->confirm_close_sessions, ==, confirm);
    g_assert_cmpint(saved->appearance, ==, dark ? PREFERENCES_APPEARANCE_DARK : PREFERENCES_APPEARANCE_LIGHT);
    g_free(saved); g_free(path);
}
static void startup_child(void) {
    expected_dark = g_strcmp0(g_getenv("DEVTERM_TEST_APPEARANCE"), "light") != 0;
    GtkApplication *application = new_application();
    g_signal_connect(application, "window-added", G_CALLBACK(on_window_added), NULL);
    present(application); g_assert_cmpuint(mapped, ==, 1); g_assert_cmpuint(painted, ==, 1);
    destroy_windows(application);
}
static void assert_fresh_process(gboolean dark) {
    g_setenv("DEVTERM_TEST_APPEARANCE", dark ? "dark" : "light", TRUE);
    g_test_trap_subprocess("/subprocess/appearance-startup", 15 * G_TIME_SPAN_SECOND, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_passed();
}
static void remove_preferences(void) {
    char *path = preferences_config_path(); g_remove(path); g_free(path);
}
static void test_restart_and_sync(void) {
    remove_preferences(); GtkApplication *application = new_application();
    App *first = present(application), *second = present(application);
    assert_appearance(first, TRUE); assert_appearance(second, TRUE);
    toggle(first, "dark-appearance"); assert_appearance(first, FALSE); assert_appearance(second, FALSE); assert_saved(FALSE, TRUE);
    toggle(second, "confirm-close-sessions"); assert_saved(FALSE, FALSE);
    App *third = present(application); assert_appearance(third, FALSE);
    destroy_windows(application); assert_fresh_process(FALSE); assert_saved(FALSE, FALSE);
    application = new_application(); first = present(application); assert_appearance(first, FALSE);
    toggle(first, "dark-appearance"); assert_appearance(first, TRUE); assert_saved(TRUE, FALSE);
    toggle(first, "confirm-close-sessions"); assert_saved(TRUE, TRUE);
    destroy_windows(application); assert_fresh_process(TRUE); remove_preferences();
}
static void test_legacy_startup(void) {
    remove_preferences(); assert_fresh_process(TRUE);
    char *path = preferences_config_path(), *directory = g_path_get_dirname(path);
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    const char *legacy = "{\"version\":1,\"confirm_close_sessions\":false}";
    g_assert_true(g_file_set_contents(path, legacy, -1, NULL)); assert_fresh_process(TRUE);
    char *text = NULL; g_file_get_contents(path, &text, NULL, NULL); g_assert_cmpstr(text, ==, legacy);
    g_free(text); g_free(directory); g_free(path); remove_preferences();
}
static void test_failed_save_keeps_theme(void) {
    remove_preferences(); GtkApplication *application = new_application();
    App *first = present(application), *second = present(application);
    toggle(first, "dark-appearance"); assert_saved(FALSE, TRUE);
    char *path = preferences_config_path(); const char *broken = "{broken";
    g_assert_true(g_file_set_contents(path, broken, -1, NULL));
    toggle(first, "dark-appearance"); assert_appearance(first, FALSE); assert_appearance(second, FALSE);
    char *text = NULL; g_file_get_contents(path, &text, NULL, NULL); g_assert_cmpstr(text, ==, broken); g_free(text);
    /* The error alert is transient for the triggering window; dismiss it before teardown. */
    GListModel *windows = gtk_window_get_toplevels(); gboolean found_alert = FALSE;
    for (guint i = g_list_model_get_n_items(windows); i > 0; i--) {
        GtkWindow *window = g_list_model_get_item(windows, i - 1);
        if (gtk_window_get_transient_for(window) == GTK_WINDOW(first->window)) {
            g_assert_true(gtk_window_get_modal(window)); found_alert = TRUE; gtk_window_destroy(window);
        }
        g_object_unref(window);
    }
    g_assert_true(found_alert);
    destroy_windows(application); assert_fresh_process(TRUE);
    g_free(path); remove_preferences();
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    if (!g_test_subprocess()) {
        config = g_dir_make_tmp("devterm-appearance-XXXXXX", NULL); g_assert_nonnull(config);
        g_setenv("XDG_CONFIG_HOME", config, TRUE);
    }
    if (!gtk_init_check()) return 77;
    g_test_add_func("/subprocess/appearance-startup", startup_child);
    g_test_add_func("/appearance/restart-and-multiwindow", test_restart_and_sync);
    g_test_add_func("/appearance/default-and-legacy-first-frame", test_legacy_startup);
    g_test_add_func("/appearance/failed-save-keeps-theme", test_failed_save_keeps_theme);
    int result = g_test_run();
    if (config) {
        char *directory = g_build_filename(config, "devterm", NULL);
        g_rmdir(directory); g_rmdir(config); g_free(directory); g_free(config);
    }
    return result;
}
