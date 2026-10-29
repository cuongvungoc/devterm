#include "sessions-ui.h"
#include "credentials.h"
#include <string.h>

struct SessionManager {
    int refs; gboolean closing, valid;
    GWeakRef parent;
    GtkWidget *window, *list, *message;
    GPtrArray *profiles; char *path, *load_error;
    GCancellable *cancel;
    SessionConnect connect; SessionCurrentAction current; gpointer data;
};
static SessionManager *manager_ref(SessionManager *m) { m->refs++; return m; }
static void manager_unref(gpointer data) {
    SessionManager *m = data; if (--m->refs) return;
    g_weak_ref_clear(&m->parent); g_ptr_array_unref(m->profiles); g_object_unref(m->cancel);
    g_free(m->path); g_free(m->load_error); g_free(m);
}
static void notify(SessionManager *m, const char *message) {
    if (!m->closing && m->message) gtk_label_set_text(GTK_LABEL(m->message), message);
}
static void report_error(SessionManager *m, const char *message) {
    if (m->closing) return;
    notify(m, message);
    GtkWindow *parent = g_weak_ref_get(&m->parent);
    if (parent) {
        GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", message);
        gtk_alert_dialog_show(dialog, parent); g_object_unref(dialog); g_object_unref(parent);
    }
}
static gint index_of(SessionManager *m, const char *id) {
    for (guint i = 0; i < m->profiles->len; i++) if (g_str_equal(((SessionProfile *)m->profiles->pdata[i])->id, id)) return i;
    return -1;
}
static gboolean same_endpoint(const SessionProfile *a, const SessionProfile *b) {
    char *first = session_endpoint(a), *second = session_endpoint(b);
    gboolean same = g_str_equal(first, second); g_free(first); g_free(second); return same;
}
static SessionProfile *selected(SessionManager *m) {
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(m->list));
    if (!row) return NULL;
    gint i = index_of(m, g_object_get_data(G_OBJECT(row), "profile-id"));
    return i >= 0 ? m->profiles->pdata[i] : NULL;
}
static void refresh(SessionManager *m) {
    GtkWidget *child; while ((child = gtk_widget_get_first_child(m->list))) gtk_list_box_remove(GTK_LIST_BOX(m->list), child);
    for (guint i = 0; i < m->profiles->len; i++) {
        SessionProfile *p = m->profiles->pdata[i];
        const char *type = p->type == SESSION_SSH ? "SSH" : p->type == SESSION_TELNET ? "Telnet" : "Serial";
        char *text = p->type == SESSION_SERIAL ? g_strdup_printf("%s — %s — %s (%d baud)", p->name, type, p->device, p->baud) :
            g_strdup_printf("%s — %s — %s@%s:%d", p->name, type, p->username, p->host, p->port);
        GtkWidget *row = gtk_list_box_row_new(), *label = gtk_label_new(text); g_free(text);
        gtk_widget_set_halign(label, GTK_ALIGN_START); gtk_widget_set_margin_start(label, 8); gtk_widget_set_margin_end(label, 8);
        gtk_widget_set_margin_top(label, 8); gtk_widget_set_margin_bottom(label, 8);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
        g_object_set_data_full(G_OBJECT(row), "profile-id", g_strdup(p->id), g_free);
        gtk_list_box_append(GTK_LIST_BOX(m->list), row);
    }
}
typedef struct { SessionManager *manager; SessionProfile *profile; } SecretChange;
static void secret_changed(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source; SecretChange *c = data; GError *error = NULL;
    if (!credential_change_finish(result, &error)) {
        char *text = g_strdup_printf("Profile updated, but keyring operation failed: %s. Password was not written to the configuration.", error->message);
        report_error(c->manager, text); g_free(text); g_clear_error(&error);
    } else notify(c->manager, "Profile and keyring updated");
    session_profile_free(c->profile); manager_unref(c->manager); g_free(c);
}
static void change_secret(SessionManager *m, const SessionProfile *p, const char *password) {
    SecretChange *c = g_new0(SecretChange, 1); c->manager = manager_ref(m); c->profile = session_profile_copy(p);
    if (password) credential_store(p, password, m->cancel, secret_changed, c);
    else credential_clear(p, m->cancel, secret_changed, c);
}
typedef struct { SessionManager *manager; SessionProfile *profile; GtkWidget *password, *remember; } Connection;
static void connection_free(gpointer data) {
    Connection *c = data; session_profile_free(c->profile); manager_unref(c->manager); g_free(c);
}
static void launch(Connection *c, const char *password) {
    SessionManager *m = c->manager;
    if (!m->closing && m->connect) {
        m->connect(c->profile, password, m->data);
        if (m->window) gtk_window_destroy(GTK_WINDOW(m->window));
    }
}
static gboolean password_valid(const char *password) {
    if (!password || !*password || strlen(password) > 1023 || !g_utf8_validate(password, -1, NULL)) return FALSE;
    for (const char *s = password; *s; s++) if ((guchar)*s < 32 || *s == 127) return FALSE;
    return TRUE;
}
static void password_response(GtkDialog *dialog, int response, gpointer data) {
    Connection *c = data; SessionManager *m = c->manager;
    if (response == GTK_RESPONSE_ACCEPT && !m->closing) {
        const char *password = gtk_editable_get_text(GTK_EDITABLE(c->password));
        if (!password_valid(password)) { gtk_widget_set_tooltip_text(c->password, "Enter 1–1023 UTF-8 bytes without control characters"); gtk_widget_add_css_class(c->password, "error"); return; }
        gboolean remember = gtk_check_button_get_active(GTK_CHECK_BUTTON(c->remember));
        if (remember) {
            change_secret(m, c->profile, password);
        } else if (c->profile->remember) change_secret(m, c->profile, NULL);
        gint index = index_of(m, c->profile->id);
        if (index >= 0 && m->valid && same_endpoint(m->profiles->pdata[index], c->profile)) {
            SessionProfile *stored = m->profiles->pdata[index]; gboolean old = stored->remember; stored->remember = remember;
            GError *error = NULL;
            if (!sessions_save(m->path, m->profiles, &error)) { stored->remember = old; report_error(m, error->message); g_clear_error(&error); }
        }
        /* The live pane's immutable profile must retain this choice too, so
         * reconnect can look up the password remembered by this dialog. */
        c->profile->remember = remember;
        char *secret = g_strdup(password);
        gtk_editable_set_text(GTK_EDITABLE(c->password), ""); launch(c, secret); session_secret_free(secret);
    }
    gtk_window_destroy(GTK_WINDOW(dialog));
}
static void password_dialog(Connection *c, const char *detail) {
    SessionManager *m = c->manager;
    if (m->closing) { connection_free(c); return; }
    GtkWindow *parent = g_weak_ref_get(&m->parent);
    if (!parent) { connection_free(c); return; }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Session Password", parent, GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL, "Connect", GTK_RESPONSE_ACCEPT, NULL);
    g_object_unref(parent); gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
    g_object_set_data_full(G_OBJECT(dialog), "connection", c, connection_free);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_box_set_spacing(GTK_BOX(box), 8);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16); gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    char *text = g_strdup_printf("%s\n%s", c->profile->name, detail);
    GtkWidget *label = gtk_label_new(text); g_free(text); gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_widget_set_size_request(label, 440, -1); gtk_box_append(GTK_BOX(box), label);
    c->password = gtk_password_entry_new(); gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(c->password), FALSE);
    g_object_set(c->password, "activates-default", TRUE, NULL);
    gtk_accessible_update_property(GTK_ACCESSIBLE(c->password), GTK_ACCESSIBLE_PROPERTY_LABEL, "Session password", -1);
    gtk_box_append(GTK_BOX(box), c->password);
    c->remember = gtk_check_button_new_with_label("Remember password in system keyring");
    gint index = index_of(m, c->profile->id);
    gboolean can_remember = m->valid && index >= 0 && same_endpoint(m->profiles->pdata[index], c->profile);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c->remember), can_remember && c->profile->remember);
    gtk_widget_set_sensitive(c->remember, can_remember);
    gtk_box_append(GTK_BOX(box), c->remember);
    g_signal_connect(dialog, "response", G_CALLBACK(password_response), c);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_window_present(GTK_WINDOW(dialog)); gtk_widget_grab_focus(c->password);
}
static void password_found(GObject *source, GAsyncResult *result, gpointer data) {
    (void)source; Connection *c = data; GError *error = NULL;
    char *password = credential_lookup_finish(result, &error);
    if (c->manager->closing) { session_secret_free(password); g_clear_error(&error); connection_free(c); return; }
    if (password && password_valid(password)) { launch(c, password); session_secret_free(password); connection_free(c); }
    else {
        session_secret_free(password);
        const char *detail = error ? "Keyring is unavailable or locked. Enter a password for this connection; configuration files never store it." :
            "Enter the password for this connection.";
        password_dialog(c, detail);
    }
    g_clear_error(&error);
}
void session_manager_connect(SessionManager *m, const SessionProfile *p) {
    if (m->closing) return;
    Connection *c = g_new0(Connection, 1); c->manager = manager_ref(m); c->profile = session_profile_copy(p);
    if (!session_needs_password(p)) { launch(c, NULL); connection_free(c); }
    else if (p->remember) { notify(m, "Retrieving password from system keyring…"); credential_lookup(p, m->cancel, password_found, c); }
    else password_dialog(c, "Enter the password for this connection.");
}

enum { NAME, TYPE, HOST, PORT, USER, AUTH, IDENTITY, DEVICE, BAUD, BITS, PARITY, STOP, FLOW, AUTO, INITIAL,
       LOGIN, PASSWORD_PROMPT, READY, TIMEOUT, REMEMBER, SECRET, FIELD_COUNT };
typedef struct { SessionManager *manager; SessionProfile *original; GtkWidget *fields[FIELD_COUNT], *rows[FIELD_COUNT], *error; } Editor;
static void editor_free(gpointer data) { Editor *e = data; session_profile_free(e->original); manager_unref(e->manager); g_free(e); }
static GtkWidget *entry(const char *text) { GtkWidget *w = gtk_entry_new(); gtk_editable_set_text(GTK_EDITABLE(w), text); return w; }
static GtkWidget *number(int value, int minimum, int maximum) {
    GtkWidget *w = gtk_spin_button_new_with_range(minimum, maximum, 1); gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), value); return w;
}
static GtkWidget *dropdown(const char *const *labels, guint selected) {
    GtkWidget *w = gtk_drop_down_new_from_strings(labels); gtk_drop_down_set_selected(GTK_DROP_DOWN(w), selected); return w;
}
static guint choice(Editor *e, int field) { return gtk_drop_down_get_selected(GTK_DROP_DOWN(e->fields[field])); }
static gboolean checked(Editor *e, int field) { return gtk_check_button_get_active(GTK_CHECK_BUTTON(e->fields[field])); }
static void editor_visibility(GObject *object, GParamSpec *spec, gpointer data) {
    (void)object; (void)spec; Editor *e = data; guint type = choice(e, TYPE);
    gboolean ssh = type == SESSION_SSH, serial = type == SESSION_SERIAL, automatic = !ssh && checked(e, AUTO);
    gboolean need_password = ssh ? choice(e, AUTH) == 0 : automatic;
    for (int i = 0; i < FIELD_COUNT; i++) {
        gboolean visible = TRUE;
        if (i == HOST || i == PORT) visible = !serial;
        else if (i == USER) visible = ssh || automatic;
        else if (i == AUTH) visible = ssh;
        else if (i == IDENTITY) visible = ssh && !need_password;
        else if (i >= DEVICE && i <= FLOW) visible = serial;
        else if (i == AUTO || i == INITIAL) visible = !ssh;
        else if (i >= LOGIN && i <= READY) visible = automatic;
        else if (i == REMEMBER) visible = need_password;
        else if (i == SECRET) visible = need_password && checked(e, REMEMBER);
        gtk_widget_set_visible(e->rows[i], visible);
    }
    int port = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->fields[PORT]));
    if (port == 22 || port == 23) gtk_spin_button_set_value(GTK_SPIN_BUTTON(e->fields[PORT]), type == SESSION_TELNET ? 23 : 22);
}
static void editor_toggled(GtkCheckButton *button, gpointer data) { editor_visibility(G_OBJECT(button), NULL, data); }
static void editor_row(Editor *e, GtkBox *box, int field, const char *name, GtkWidget *widget) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8), *label = gtk_label_new(name);
    gtk_widget_set_size_request(label, 185, -1); gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), label); gtk_widget_set_hexpand(widget, TRUE); gtk_box_append(GTK_BOX(row), widget);
    e->fields[field] = widget; e->rows[field] = row; gtk_box_append(box, row);
    gtk_accessible_update_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
}
static void profile_response(GtkDialog *dialog, int response, gpointer data) {
    Editor *e = data; SessionManager *m = e->manager;
    if (response != GTK_RESPONSE_ACCEPT || m->closing) { gtk_window_destroy(GTK_WINDOW(dialog)); return; }
    SessionProfile *p = session_profile_copy(e->original);
#define TEXT(f, n) g_free(p->f); p->f = g_strdup(gtk_editable_get_text(GTK_EDITABLE(e->fields[n])))
#define NUM(f, n) p->f = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(e->fields[n]))
    TEXT(name, NAME); TEXT(host, HOST); TEXT(username, USER); TEXT(identity, IDENTITY); TEXT(device, DEVICE);
    TEXT(login_prompt, LOGIN); TEXT(password_prompt, PASSWORD_PROMPT); TEXT(ready_prompt, READY);
    NUM(port, PORT); NUM(baud, BAUD); NUM(data_bits, BITS); NUM(stop_bits, STOP); NUM(timeout, TIMEOUT);
#undef TEXT
#undef NUM
    p->type = choice(e, TYPE); p->password_auth = choice(e, AUTH) == 0;
    const char *parity[] = {"none", "even", "odd"}, *flow[] = {"none", "hard", "soft"};
    g_free(p->parity); p->parity = g_strdup(parity[choice(e, PARITY)]); g_free(p->flow); p->flow = g_strdup(flow[choice(e, FLOW)]);
    p->auto_login = p->type != SESSION_SSH && checked(e, AUTO); p->send_initial_enter = checked(e, INITIAL);
    p->remember = session_needs_password(p) && checked(e, REMEMBER);
    const char *secret = gtk_editable_get_text(GTK_EDITABLE(e->fields[SECRET]));
    GError *error = NULL;
    if (!session_profile_validate(p, &error)) { gtk_label_set_text(GTK_LABEL(e->error), error->message); g_clear_error(&error); session_profile_free(p); return; }
    char *before = session_endpoint(e->original), *after = session_endpoint(p);
    gboolean same = g_str_equal(before, after); g_free(before); g_free(after);
    if (p->remember && ((!*secret && (!same || !e->original->remember)) || (*secret && !password_valid(secret)))) {
        gtk_label_set_text(GTK_LABEL(e->error), "Enter a password to remember (1–1023 UTF-8 bytes, no control characters). A different endpoint requires a new password.");
        session_profile_free(p); return;
    }
    gint i = index_of(m, p->id); SessionProfile *old = i >= 0 ? m->profiles->pdata[i] : NULL;
    if (old) m->profiles->pdata[i] = p; else g_ptr_array_add(m->profiles, p);
    if (!sessions_save(m->path, m->profiles, &error)) {
        if (old) m->profiles->pdata[i] = old; else g_ptr_array_steal_index(m->profiles, m->profiles->len - 1);
        session_profile_free(p); gtk_label_set_text(GTK_LABEL(e->error), error->message); g_clear_error(&error); return;
    }
    notify(m, "Profile saved");
    if (old && (old->remember && (!p->remember || !same))) change_secret(m, old, NULL);
    if (p->remember && *secret) change_secret(m, p, secret);
    session_profile_free(old); gtk_editable_set_text(GTK_EDITABLE(e->fields[SECRET]), "");
    refresh(m); gtk_window_destroy(GTK_WINDOW(dialog));
}
static void edit_profile(SessionManager *m, SessionProfile *draft) {
    if (!m->valid || m->closing) { session_profile_free(draft); return; }
    Editor *e = g_new0(Editor, 1); e->manager = manager_ref(m); e->original = draft;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Session Profile", GTK_WINDOW(m->window), GTK_DIALOG_MODAL,
        "Cancel", GTK_RESPONSE_CANCEL, "Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE); gtk_window_set_default_size(GTK_WINDOW(dialog), 700, 650);
    g_object_set_data_full(G_OBJECT(dialog), "editor", e, editor_free);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, TRUE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 16); gtk_widget_set_margin_end(box, 16); gtk_widget_set_margin_top(box, 16); gtk_widget_set_margin_bottom(box, 16);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_box_append(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll);
    SessionProfile *p = draft;
    const char *types[] = {"SSH", "Telnet", "Serial", NULL}, *auth[] = {"Password", "SSH key / agent", NULL};
    const char *parity[] = {"none", "even", "odd", NULL}, *flow[] = {"none", "hardware", "software", NULL};
    editor_row(e, GTK_BOX(box), NAME, "Name", entry(p->name));
    editor_row(e, GTK_BOX(box), TYPE, "Protocol", dropdown(types, p->type));
    editor_row(e, GTK_BOX(box), HOST, "Host", entry(p->host)); editor_row(e, GTK_BOX(box), PORT, "Port", number(p->port, 1, 65535));
    editor_row(e, GTK_BOX(box), USER, "Username", entry(p->username)); editor_row(e, GTK_BOX(box), AUTH, "SSH authentication", dropdown(auth, !p->password_auth));
    editor_row(e, GTK_BOX(box), IDENTITY, "Private key (absolute path)", entry(p->identity));
    editor_row(e, GTK_BOX(box), DEVICE, "Serial device (/dev/…)", entry(p->device));
    editor_row(e, GTK_BOX(box), BAUD, "Baud rate", number(p->baud, 50, 4000000));
    editor_row(e, GTK_BOX(box), BITS, "Data bits", number(p->data_bits, 5, 8));
    editor_row(e, GTK_BOX(box), PARITY, "Parity", dropdown(parity, g_str_equal(p->parity, "even") ? 1 : g_str_equal(p->parity, "odd") ? 2 : 0));
    editor_row(e, GTK_BOX(box), STOP, "Stop bits", number(p->stop_bits, 1, 2));
    editor_row(e, GTK_BOX(box), FLOW, "Flow control", dropdown(flow, g_str_equal(p->flow, "hard") ? 1 : g_str_equal(p->flow, "soft") ? 2 : 0));
    GtkWidget *automatic = gtk_check_button_new(); gtk_check_button_set_active(GTK_CHECK_BUTTON(automatic), p->auto_login);
    editor_row(e, GTK_BOX(box), AUTO, "Automatic login", automatic);
    GtkWidget *initial = gtk_check_button_new(); gtk_check_button_set_active(GTK_CHECK_BUTTON(initial), p->send_initial_enter);
    editor_row(e, GTK_BOX(box), INITIAL, "Send Enter on connection", initial);
    editor_row(e, GTK_BOX(box), LOGIN, "Username prompt (regex)", entry(p->login_prompt));
    editor_row(e, GTK_BOX(box), PASSWORD_PROMPT, "Password prompt (regex)", entry(p->password_prompt));
    editor_row(e, GTK_BOX(box), READY, "Ready prompt (regex)", entry(p->ready_prompt));
    editor_row(e, GTK_BOX(box), TIMEOUT, "Connect/login timeout (s)", number(p->timeout, 5, 120));
    GtkWidget *remember = gtk_check_button_new(); gtk_check_button_set_active(GTK_CHECK_BUTTON(remember), p->remember);
    editor_row(e, GTK_BOX(box), REMEMBER, "Remember in system keyring", remember);
    GtkWidget *password = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(password), FALSE);
    gtk_widget_set_tooltip_text(password, "Leave empty to keep an existing saved password for the same endpoint");
    editor_row(e, GTK_BOX(box), SECRET, "Password (keyring only)", password);
    GtkWidget *note = gtk_label_new("Telnet does not encrypt network traffic. Auto-login hides authentication output and stops on failure/timeout.\nPrompts match the end of received text; leave username prompt empty for password-only devices.\nUse a precise ready prompt for your device. SSH password mode does not answer OTP/keyboard-interactive challenges.");
    gtk_label_set_wrap(GTK_LABEL(note), TRUE); gtk_box_append(GTK_BOX(box), note);
    e->error = gtk_label_new(""); gtk_label_set_wrap(GTK_LABEL(e->error), TRUE); gtk_widget_add_css_class(e->error, "error"); gtk_box_append(GTK_BOX(box), e->error);
    g_signal_connect(e->fields[TYPE], "notify::selected", G_CALLBACK(editor_visibility), e);
    g_signal_connect(e->fields[AUTH], "notify::selected", G_CALLBACK(editor_visibility), e);
    g_signal_connect(automatic, "toggled", G_CALLBACK(editor_toggled), e); g_signal_connect(remember, "toggled", G_CALLBACK(editor_toggled), e);
    g_signal_connect(dialog, "response", G_CALLBACK(profile_response), e); editor_visibility(NULL, NULL, e);
    gtk_window_present(GTK_WINDOW(dialog));
}
static void add_clicked(GtkButton *button, gpointer data) { (void)button; edit_profile(data, session_profile_new()); }
static void edit_clicked(GtkButton *button, gpointer data) { (void)button; SessionManager *m = data; SessionProfile *p = selected(m); if (p) edit_profile(m, session_profile_copy(p)); }
static void duplicate_clicked(GtkButton *button, gpointer data) {
    (void)button; SessionManager *m = data; SessionProfile *p = selected(m); if (!p) return;
    SessionProfile *draft = session_profile_copy(p); g_free(draft->id); draft->id = g_uuid_string_random(); draft->remember = FALSE;
    char *name = g_strdup_printf("%s copy", p->name); g_free(draft->name); draft->name = name; edit_profile(m, draft);
}
static void delete_clicked(GtkButton *button, gpointer data) {
    (void)button; SessionManager *m = data; SessionProfile *p = selected(m); if (!p || !m->valid) return;
    gint index = index_of(m, p->id); g_ptr_array_steal_index(m->profiles, index); GError *error = NULL;
    if (!sessions_save(m->path, m->profiles, &error)) { g_ptr_array_insert(m->profiles, index, p); notify(m, error->message); g_clear_error(&error); return; }
    change_secret(m, p, NULL); session_profile_free(p); refresh(m);
}
static void connect_clicked(GtkButton *button, gpointer data) { (void)button; SessionManager *m = data; SessionProfile *p = selected(m); if (p) session_manager_connect(m, p); }
static void row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data) { (void)box; (void)row; connect_clicked(NULL, data); }
static void current_clicked(GtkButton *button, gpointer data) {
    SessionManager *m = data;
    if (!m->closing && m->current) m->current(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "reconnect")), m->data);
}
static void manager_destroyed(GtkWidget *window, gpointer data) {
    (void)window; SessionManager *m = data; m->window = m->list = m->message = NULL;
}
static void manager_response(GtkDialog *dialog, int response, gpointer data) { (void)response; (void)data; gtk_window_destroy(GTK_WINDOW(dialog)); }
static void button(GtkBox *box, const char *label, GCallback callback, SessionManager *m, gboolean sensitive) {
    GtkWidget *w = gtk_button_new_with_label(label); gtk_widget_set_sensitive(w, sensitive);
    g_signal_connect(w, "clicked", callback, m); gtk_box_append(box, w);
}
SessionManager *session_manager_new(GtkWindow *parent, SessionConnect connect, SessionCurrentAction current, gpointer data) {
    SessionManager *m = g_new0(SessionManager, 1); m->refs = 1; g_weak_ref_init(&m->parent, parent);
    m->connect = connect; m->current = current; m->data = data; m->cancel = g_cancellable_new(); m->path = sessions_config_path();
    GError *error = NULL; m->profiles = sessions_load(m->path, &error); m->valid = m->profiles != NULL;
    if (!m->valid) { m->load_error = g_strdup_printf("Cannot load sessions: %s. Fix %s before editing profiles.", error->message, m->path); g_clear_error(&error); m->profiles = g_ptr_array_new_with_free_func(session_profile_free); }
    return m;
}
void session_manager_show(SessionManager *m) {
    if (m->closing) return;
    if (m->window) { gtk_window_present(GTK_WINDOW(m->window)); return; }
    GtkWindow *parent = g_weak_ref_get(&m->parent); if (!parent) return;
    m->window = gtk_dialog_new_with_buttons("Sessions", parent, 0, "Close", GTK_RESPONSE_CLOSE, NULL); g_object_unref(parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(m->window), TRUE); gtk_window_set_default_size(GTK_WINDOW(m->window), 800, 440);
    g_object_set_data_full(G_OBJECT(m->window), "manager", manager_ref(m), manager_unref);
    g_signal_connect(m->window, "destroy", G_CALLBACK(manager_destroyed), m); g_signal_connect(m->window, "response", G_CALLBACK(manager_response), m);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(m->window)); gtk_box_set_spacing(GTK_BOX(box), 8);
    gtk_widget_set_margin_start(box, 12); gtk_widget_set_margin_end(box, 12); gtk_widget_set_margin_top(box, 12); gtk_widget_set_margin_bottom(box, 12);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_box_append(GTK_BOX(box), row);
    button(GTK_BOX(row), "Add", G_CALLBACK(add_clicked), m, m->valid); button(GTK_BOX(row), "Edit", G_CALLBACK(edit_clicked), m, m->valid);
    button(GTK_BOX(row), "Duplicate", G_CALLBACK(duplicate_clicked), m, m->valid); button(GTK_BOX(row), "Delete", G_CALLBACK(delete_clicked), m, m->valid);
    button(GTK_BOX(row), "Connect", G_CALLBACK(connect_clicked), m, TRUE);
    GtkWidget *scroll = gtk_scrolled_window_new(); gtk_widget_set_vexpand(scroll, TRUE); m->list = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(m->list), FALSE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), m->list); gtk_box_append(GTK_BOX(box), scroll);
    g_signal_connect(m->list, "row-activated", G_CALLBACK(row_activated), m);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6); gtk_box_append(GTK_BOX(box), row);
    button(GTK_BOX(row), "Disconnect current pane", G_CALLBACK(current_clicked), m, TRUE);
    GtkWidget *reconnect = gtk_button_new_with_label("Reconnect current pane"); g_object_set_data(G_OBJECT(reconnect), "reconnect", GINT_TO_POINTER(TRUE));
    g_signal_connect(reconnect, "clicked", G_CALLBACK(current_clicked), m); gtk_box_append(GTK_BOX(row), reconnect);
    m->message = gtk_label_new(m->load_error ? m->load_error : "Select a profile, then Connect. Each connection opens a new tab.");
    gtk_label_set_wrap(GTK_LABEL(m->message), TRUE); gtk_box_append(GTK_BOX(box), m->message); refresh(m);
    gtk_window_present(GTK_WINDOW(m->window));
}
void session_manager_free(SessionManager *m) {
    if (!m || m->closing) return;
    m->closing = TRUE; m->connect = NULL; m->current = NULL; m->data = NULL; g_cancellable_cancel(m->cancel);
    if (m->window) gtk_window_destroy(GTK_WINDOW(m->window));
    manager_unref(m);
}
