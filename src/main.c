#include <gtk/gtk.h>
#include <adwaita.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <locale.h>
#include "editor-widget.h"
#include "document.h"
#include "preferences.h"
#include "tab-bar.h"
#include "tab.h"
#include "find-replace-bar.h"
#include "editor-print.h"
#include "status-bar.h"
#include "theme-manager.h"

#ifdef HAVE_VTE
#include <vte/vte.h>
#endif

/* Define file type entries for the menu */
typedef struct {
    const char *name;
    const char *id;
} FileTypeEntry;

static const FileTypeEntry file_types[] = {
    { N_("Plain Text"), "plain" },
    { "C", "c" },
    { "C++", "cpp" },
    { "Python", "python" },
    { "Bash", "bash" },
    { "Rust", "rust" },
    { N_("Header"), "h" },
    { "YAML", "yaml" },
    { "JSON", "json" },
    { "XML", "xml" },
    { "JavaScript", "javascript" },
    { N_("Desktop Entry"), "desktop" },
    { "Markdown", "markdown" },
    { NULL, NULL }
};

typedef struct _ViteWindow ViteWindow;

struct _ViteWindow {
    AdwApplicationWindow *window;
    ViteTabBar *tab_bar;
    GtkStack *stack;
    AdwWindowTitle *window_title;
    GtkWidget *header_progress;
    GtkWidget *header_spinner;
    GtkWidget *status_bar;
    GtkWidget *last_active_editor; /* Fallback when focus is lost (e.g. to a popover) */
    int loading_count;
    gboolean close_when_done;
    gboolean force_close_once;
    GWeakRef active_dialog_ref;
    
    /* Fullscreen Support */
    GtkOverlay *main_overlay;
    GtkWidget *main_box;
    GtkWidget *titlebar_container; 
    GtkWidget *fullscreen_restore_btn;
    AdwHeaderBar *header_bar;
    GtkWidget *save_button; /* Reference to the save button */

    guint auto_hide_source_id;
};

typedef struct {
    ViteTab *tab;
    ViteTabBar *tab_bar; /* Weak Ref */
    GtkWidget *header_progress; /* Weak Ref */
    GtkWidget *header_spinner; /* Weak Ref */
    char *filename;
    ViteWindow *window; /* Raw Pointer - Valid only if gtkw_ref is valid */
    GtkWindow *gtkw_ref; /* Weak Ref to actual GtkWindow */
    Document *doc; /* Reference to document being loaded */
    gboolean is_reload;
    FileEncoding prev_encoding;
    NewlineType prev_newline;
    gboolean has_prev_format;
} LoadContext;



/* Forward declarations */
static void on_load_complete(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_load_progress(double progress, FileEncoding encoding, NewlineType newline, void *user_data);


/* Globals removed: main_window, main_tab_bar, main_stack, main_window_title */
static int untitled_count = 1;
#define MAX_RECENT_FILES 10000

static gboolean open_file(GtkApplication *app, ViteWindow *target_window, GFile *file, gboolean allow_reuse);
static void trigger_process_next_file(void);
static void queue_open_file(GtkApplication *app, ViteWindow *target_window, GFile *file, gboolean allow_reuse);

/* Queue for sequential file loading */
typedef struct {
    GtkApplication *app;
    ViteWindow *target_window;
    GFile *file;
    gboolean allow_reuse;
} QueuedFile;

static GQueue *pending_open_files = NULL;
static gboolean is_opening = FALSE;
static void create_new_tab (ViteWindow *win, const char *title, Document *doc);
static ViteWindow *setup_window(AdwApplicationWindow *window);
static void activate(GtkApplication *app, gpointer user_data);
static void update_recent_files_list(GtkListBox *list_box, GtkApplication *app, GtkPopover *popover);
// static void on_action_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data);
static gboolean filter_recent_items(GtkListBoxRow *row, gpointer user_data);
static void on_recent_context_menu(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static void add_to_local_recents(const char *uri);
static GList* load_local_recents(void);
static void save_local_recents(GList *uris);
static void on_close_recent_btn_clicked(GtkButton *btn, gpointer user_data);
static void on_open_dialog_response(GtkFileDialog *dialog, GAsyncResult *result, gpointer user_data);
static GtkWidget *get_active_editor(ViteWindow *win);

static void move_tab_to_window(ViteWindow *target_win, ViteTab *tab, int position);


/* Unused callback */
#if 0
static void
on_file_opened (GObject* source_object G_GNUC_UNUSED, GAsyncResult* res G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GtkApplication *app = GTK_APPLICATION(user_data);
    GFile *file = gtk_file_dialog_open_finish(dialog, res, NULL);
    if (file) {
        /* Route single file opens through the new queue system as well! */
        GFile *files[1] = { file };
        on_open(app, files, 1, NULL, NULL);
        g_object_unref(file);
    }
}
#endif



static GtkWidget *
find_first_editor_recursive(GtkWidget *widget) {
    if (!widget) return NULL;
    if (EDITOR_IS_WIDGET(widget)) return widget;
    
    /* Traverse based on container types we expect */
    if (GTK_IS_SCROLLED_WINDOW(widget)) {
        return find_first_editor_recursive(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(widget)));
    }
    if (GTK_IS_OVERLAY(widget)) {
        return find_first_editor_recursive(gtk_overlay_get_child(GTK_OVERLAY(widget)));
    }
    
    /* Generic child iteration for Box, Paned, etc. */
    GtkWidget *child = gtk_widget_get_first_child(widget);
    while (child) {
        GtkWidget *res = find_first_editor_recursive(child);
        if (res) return res;
        child = gtk_widget_get_next_sibling(child);
    }
    return NULL;
}

static void
find_all_editors_recursive_internal(GtkWidget *widget, GPtrArray *editors)
{
    if (!widget) return;
    if (EDITOR_IS_WIDGET(widget)) {
        g_ptr_array_add(editors, widget);
        return;
    }

    if (GTK_IS_SCROLLED_WINDOW(widget)) {
        find_all_editors_recursive_internal(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(widget)), editors);
    } else if (GTK_IS_OVERLAY(widget)) {
        find_all_editors_recursive_internal(gtk_overlay_get_child(GTK_OVERLAY(widget)), editors);
    } else {
        GtkWidget *child = gtk_widget_get_first_child(widget);
        while (child) {
            find_all_editors_recursive_internal(child, editors);
            child = gtk_widget_get_next_sibling(child);
        }
    }
}

static GPtrArray *
find_all_editors_recursive(GtkWidget *page)
{
    GPtrArray *editors = g_ptr_array_new();
    find_all_editors_recursive_internal(page, editors);
    return editors;
}

static void
page_set_document(GtkWidget *page, Document *new_doc)
{
    GPtrArray *editors = find_all_editors_recursive(page);
    Document *old_doc = NULL;

    for (guint i = 0; i < editors->len; i++) {
        EditorWidget *ed = EDITOR_WIDGET(g_ptr_array_index(editors, i));
        if (i == 0) {
            old_doc = document_ref(editor_widget_get_document(ed));
        }
        editor_widget_set_document(ed, new_doc);
    }

    if (old_doc && old_doc != new_doc) {
        document_free(old_doc);
    }
    g_ptr_array_unref(editors);
}

static GtkWidget *get_editor_from_page(GtkWidget *page);
static void on_tab_clicked (ViteTab *tab, gpointer user_data G_GNUC_UNUSED);
static void on_new_tab_clicked_header(GtkButton *btn G_GNUC_UNUSED, gpointer user_data);
static void on_new_window_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_preferences_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_overflow_changed(ViteTabBar *bar, gboolean overflowing, gpointer user_data);
static void on_tab_dropped(ViteTabBar *bar, ViteTab *tab, int position, gpointer user_data);
static void update_open_tabs_list(GtkWidget *widget, gpointer user_data);
static GtkWidget *create_view_container(ViteWindow *win, GtkWidget *editor);
static void on_find_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_replace_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_save_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_save_as_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_discard_all_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_print_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_fullscreen_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_shortcuts_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_about_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_split_right(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_split_down(GSimpleAction *action, GVariant *parameter, gpointer user_data);
#ifdef HAVE_VTE
static void on_terminal_toggle(GSimpleAction *action, GVariant *parameter, gpointer user_data);
#endif
static void on_zoom_in_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_zoom_out_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_zoom_reset_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void check_close_when_done(ViteWindow *win);
static void on_close_split_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_close_tab_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_reopen_closed_tab_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data);
static void on_quit_window_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data);

static void on_document_modified(Document *doc, gboolean modified, void *user_data);
static void on_document_content_changed(Document *doc, void *user_data);
static void on_document_changed_externally(Document *doc, void *user_data);
static void on_ignore_external_change(GtkButton *btn, gpointer user_data);
static void on_reload_external_change(GtkButton *btn, gpointer user_data);
static void on_recent_item_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data);
static void on_tab_close_clicked(ViteTab *tab, gpointer user_data G_GNUC_UNUSED);
static void on_tab_move_to_new_window(ViteTab *tab, gpointer user_data G_GNUC_UNUSED);
static void on_open_tab_close_btn_clicked(GtkButton *btn, gpointer user_data);
static void load_css(void);
static gboolean on_search_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
static void on_search_changed(GtkSearchEntry *entry, gpointer user_data);
static void update_window_title_for_tab(ViteTab *tab);
static void update_header_spinner(ViteWindow *win);
static void update_reload_action_enabled(ViteWindow *win, GtkWidget *editor);
static gboolean tab_has_unsaved_changes(ViteTab *tab);
static void close_tab_now(ViteWindow *win, ViteTab *tab);

#define MAX_RECENTLY_CLOSED 20
static GList *recently_closed_files = NULL;

typedef struct _SaveChangesDialogData SaveChangesDialogData;
typedef void (*SaveChangesDialogCallback)(const char *response, SaveChangesDialogData *dialog, gpointer user_data);

struct _SaveChangesDialogData {
    GWeakRef win_ref;
    GtkWindow *window;
    GtkWidget *save_button;
    GPtrArray *tabs;     /* strong refs: ViteTab* */
    GPtrArray *checks;   /* GtkCheckButton* by row index */
    GPtrArray *entries;  /* GtkEntry* by row index */
    SaveChangesDialogCallback callback;
    gpointer user_data;
    gboolean handled;
    gboolean closing_programmatically;
};

static void save_async_with_progress(ViteWindow *win, ViteTab *tab, Document *doc, const char *path);
static gboolean save_changes_dialog_tab_selected(SaveChangesDialogData *dialog, ViteTab *tab);
static char *save_changes_dialog_filename_for_tab(SaveChangesDialogData *dialog, ViteTab *tab);
static void show_save_changes_dialog(ViteWindow *win, GPtrArray *tabs, SaveChangesDialogCallback callback, gpointer user_data);

static void
save_changes_dialog_data_free(SaveChangesDialogData *dialog)
{
    if (!dialog) return;
    g_weak_ref_clear(&dialog->win_ref);
    if (dialog->tabs) g_ptr_array_unref(dialog->tabs);
    if (dialog->checks) g_ptr_array_unref(dialog->checks);
    if (dialog->entries) g_ptr_array_unref(dialog->entries);
    g_free(dialog);
}

static Document *
get_tab_document(ViteTab *tab)
{
    if (!tab || !VITE_IS_TAB(tab)) return NULL;
    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    GtkWidget *editor = get_editor_from_page(page);
    if (!EDITOR_IS_WIDGET(editor)) return NULL;
    return editor_widget_get_document(EDITOR_WIDGET(editor));
}

static gboolean
tab_has_unsaved_changes(ViteTab *tab)
{
    Document *doc = get_tab_document(tab);
    return doc && document_is_modified(doc);
}

static char *
path_with_tilde(const char *path)
{
    if (!path) return g_strdup("");
    const char *home = g_get_home_dir();
    if (home && g_str_has_prefix(path, home)) {
        return g_strconcat("~", path + strlen(home), NULL);
    }
    return g_strdup(path);
}

static gboolean
is_valid_filename_char(char c)
{
    return g_ascii_isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.';
}

static char *
sanitize_filename(const char *text)
{
    if (!text) return g_strdup("untitled");
    GString *out = g_string_new(NULL);
    for (const char *p = text; *p; p++) {
        if (is_valid_filename_char(*p)) {
            g_string_append_c(out, *p);
        }
    }
    char *trimmed = g_strstrip(g_string_free(out, FALSE));
    if (!trimmed || !*trimmed) {
        g_free(trimmed);
        return g_strdup("untitled");
    }
    if (strlen(trimmed) > 50) {
        trimmed[50] = '\0';
    }
    return trimmed;
}

static char *
default_save_directory(void)
{
    char *docs = g_build_filename(g_get_home_dir(), "Documents", NULL);
    if (g_file_test(docs, G_FILE_TEST_IS_DIR)) return docs;
    g_free(docs);
    return g_strdup(g_get_home_dir());
}

static char *
suggest_untitled_filename(ViteTab *tab, Document *doc)
{
    size_t len = 0;
    char *line = document_get_line(doc, 0, &len);
    char *from_line = NULL;
    if (line && len > 0) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *clean = sanitize_filename(line);
        if (clean && *clean && g_strcmp0(clean, "untitled") != 0) {
            from_line = clean;
        } else {
            g_free(clean);
        }
    }
    g_free(line);

    if (from_line) {
        char *name = g_str_has_suffix(from_line, ".txt") ? from_line : g_strconcat(from_line, ".txt", NULL);
        if (name != from_line) g_free(from_line);
        return name;
    }

    const char *title = vite_tab_get_title(tab);
    char *clean_title = sanitize_filename(title && *title ? title : "untitled");
    char *name = g_str_has_suffix(clean_title, ".txt") ? clean_title : g_strconcat(clean_title, ".txt", NULL);
    if (name != clean_title) g_free(clean_title);
    return name;
}

static char *
display_path_for_tab(ViteTab *tab)
{
    Document *doc = get_tab_document(tab);
    if (!doc) return g_strdup("");
    const char *path = document_get_file_path(doc);
    if (path && *path) {
        char *dir = g_path_get_dirname(path);
        char *shown = path_with_tilde(dir);
        g_free(dir);
        return shown;
    }
    char *def = default_save_directory();
    char *shown = path_with_tilde(def);
    g_free(def);
    return shown;
}

static void
save_changes_dialog_finish(SaveChangesDialogData *dialog, const char *response)
{
    if (!dialog || dialog->handled) return;
    dialog->handled = TRUE;

    if (dialog->callback) {
        dialog->callback(response, dialog, dialog->user_data);
    }

    if (dialog->window && GTK_IS_WINDOW(dialog->window) && !dialog->closing_programmatically) {
        dialog->closing_programmatically = TRUE;
        gtk_window_destroy(dialog->window);
    }
}

static void
on_save_changes_button_clicked(GtkButton *btn, gpointer user_data)
{
    SaveChangesDialogData *dialog = user_data;
    const char *response = g_object_get_data(G_OBJECT(btn), "save-dialog-response");
    save_changes_dialog_finish(dialog, response ? response : "cancel");
}

static gboolean
on_save_changes_close_request(GtkWindow *window G_GNUC_UNUSED, gpointer user_data)
{
    SaveChangesDialogData *dialog = user_data;
    if (dialog->closing_programmatically) return FALSE;
    save_changes_dialog_finish(dialog, "cancel");
    return TRUE;
}

static void
on_save_changes_dialog_destroy(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    save_changes_dialog_data_free((SaveChangesDialogData *)user_data);
}

static void
on_save_changes_dialog_map(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    SaveChangesDialogData *dialog = user_data;
    if (dialog->save_button) gtk_widget_grab_focus(dialog->save_button);
}

static gboolean
save_changes_dialog_tab_selected(SaveChangesDialogData *dialog, ViteTab *tab)
{
    if (!dialog || !tab) return FALSE;
    for (guint i = 0; i < dialog->tabs->len; i++) {
        if (g_ptr_array_index(dialog->tabs, i) == tab) {
            GtkCheckButton *check = GTK_CHECK_BUTTON(g_ptr_array_index(dialog->checks, i));
            return gtk_check_button_get_active(check);
        }
    }
    return FALSE;
}

static char *
save_changes_dialog_filename_for_tab(SaveChangesDialogData *dialog, ViteTab *tab)
{
    if (!dialog || !tab) return g_strdup("untitled.txt");
    for (guint i = 0; i < dialog->tabs->len; i++) {
        if (g_ptr_array_index(dialog->tabs, i) == tab) {
            GtkEntry *entry = GTK_ENTRY(g_ptr_array_index(dialog->entries, i));
            const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
            char *name = sanitize_filename(text);
            if (!strchr(name, '.')) {
                char *with_ext = g_strconcat(name, ".txt", NULL);
                g_free(name);
                return with_ext;
            }
            return name;
        }
    }
    return g_strdup("untitled.txt");
}

static void
show_save_changes_dialog(ViteWindow *win, GPtrArray *tabs, SaveChangesDialogCallback callback, gpointer user_data)
{
    if (!win || !tabs || tabs->len == 0) return;

    SaveChangesDialogData *dialog = g_new0(SaveChangesDialogData, 1);
    g_weak_ref_init(&dialog->win_ref, win->window);
    dialog->callback = callback;
    dialog->user_data = user_data;
    dialog->tabs = g_ptr_array_new_with_free_func(g_object_unref);
    dialog->checks = g_ptr_array_new();
    dialog->entries = g_ptr_array_new();

    GtkWidget *window = adw_window_new();
    dialog->window = GTK_WINDOW(window);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(win->window));
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 440, -1);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Header */
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(header, 12);
    gtk_widget_set_margin_bottom(header, 12);
    gtk_widget_set_margin_start(header, 12);
    gtk_widget_set_margin_end(header, 12);
    gtk_box_append(GTK_BOX(main_box), header);

    GtkWidget *title = gtk_label_new(_("Save Changes?"));
    gtk_widget_add_css_class(title, "title-2");
    gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(header), title);

    GtkWidget *body = gtk_label_new(_("Open documents contain unsaved changes.\nChanges which are not saved will be permanently lost."));
    gtk_widget_add_css_class(body, "dim-label");
    gtk_label_set_wrap(GTK_LABEL(body), TRUE);
    gtk_label_set_justify(GTK_LABEL(body), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(body, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(header), body);

    /* File List with ScrolledWindow */
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 400);
    gtk_box_append(GTK_BOX(main_box), scroll);

    GtkWidget *files_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(files_box, 12);
    gtk_widget_set_margin_bottom(files_box, 12);
    gtk_widget_set_margin_start(files_box, 24);
    gtk_widget_set_margin_end(files_box, 24);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), files_box);

    for (guint i = 0; i < tabs->len; i++) {
        ViteTab *tab = VITE_TAB(g_ptr_array_index(tabs, i));
        Document *doc = get_tab_document(tab);
        if (!doc) continue;

        g_ptr_array_add(dialog->tabs, g_object_ref(tab));

        GtkWidget *file_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        
        GtkWidget *check = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), TRUE);
        gtk_widget_set_focus_on_click(check, FALSE);
        gtk_box_append(GTK_BOX(file_row), check);
        g_ptr_array_add(dialog->checks, check);

        GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_hexpand(info_box, TRUE);

        const char *path = document_get_file_path(doc);
        char *entry_text = NULL;
        if (path && *path) {
            entry_text = g_path_get_basename(path);
        } else {
            entry_text = suggest_untitled_filename(tab, doc);
        }

        GtkWidget *entry = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(entry), entry_text);
        gtk_widget_set_hexpand(entry, TRUE);
        g_ptr_array_add(dialog->entries, entry);
        gtk_box_append(GTK_BOX(info_box), entry);
        g_free(entry_text);

        char *path_text = display_path_for_tab(tab);
        GtkWidget *path_label = gtk_label_new(path_text);
        gtk_widget_add_css_class(path_label, "dim-label");
        gtk_widget_add_css_class(path_label, "caption");
        gtk_widget_set_halign(path_label, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(path_label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(path_label), 40);
        gtk_box_append(GTK_BOX(info_box), path_label);
        g_free(path_text);

        gtk_box_append(GTK_BOX(file_row), info_box);
        gtk_box_append(GTK_BOX(files_box), file_row);
    }

    /* Buttons */
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(button_box), TRUE);
    gtk_widget_set_margin_top(button_box, 12);
    gtk_widget_set_margin_bottom(button_box, 24);
    gtk_widget_set_margin_start(button_box, 24);
    gtk_widget_set_margin_end(button_box, 24);
    gtk_box_append(GTK_BOX(main_box), button_box);

    GtkWidget *cancel_btn = gtk_button_new_with_label(_("Cancel"));
    GtkWidget *discard_btn = gtk_button_new_with_label(_("Discard All"));
    GtkWidget *save_btn = gtk_button_new_with_label(_("Save"));
    
    gtk_widget_add_css_class(discard_btn, "destructive-action");
    gtk_widget_add_css_class(save_btn, "suggested-action");
    
    g_object_set_data(G_OBJECT(cancel_btn), "save-dialog-response", "cancel");
    g_object_set_data(G_OBJECT(discard_btn), "save-dialog-response", "discard");
    g_object_set_data(G_OBJECT(save_btn), "save-dialog-response", "save");
    
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_save_changes_button_clicked), dialog);
    g_signal_connect(discard_btn, "clicked", G_CALLBACK(on_save_changes_button_clicked), dialog);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(on_save_changes_button_clicked), dialog);
    dialog->save_button = save_btn;

    gtk_box_append(GTK_BOX(button_box), cancel_btn);
    gtk_box_append(GTK_BOX(button_box), discard_btn);
    gtk_box_append(GTK_BOX(button_box), save_btn);

    GtkWidget *handle = gtk_window_handle_new();
    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(handle), main_box);
    adw_window_set_content(ADW_WINDOW(window), handle);
    
    g_signal_connect(window, "close-request", G_CALLBACK(on_save_changes_close_request), dialog);
    g_signal_connect(window, "destroy", G_CALLBACK(on_save_changes_dialog_destroy), dialog);
    g_signal_connect(window, "map", G_CALLBACK(on_save_changes_dialog_map), dialog);
    
    gtk_window_present(GTK_WINDOW(window));
}

static void
reset_tab_to_empty(ViteWindow *win, ViteTab *tab)
{
    if (!win || !tab || !VITE_IS_TAB(tab)) return;

    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (page) {
        Document *doc = document_new(NULL);
        GtkWidget *view_container = gtk_widget_get_first_child(page);
        page_set_document(page, doc);

        document_add_modification_callback(doc, on_document_modified, tab);
        document_add_content_callback(doc, on_document_content_changed, tab);
        if (view_container) {
            document_add_external_change_callback(doc, on_document_changed_externally, view_container);
        }
        document_free(doc);
    }

    char *title = g_strdup_printf("Untitled %d", untitled_count++);
    vite_tab_set_title(tab, title);
    g_object_set_data_full(G_OBJECT(tab), "original_title", g_strdup(title), g_free);
    g_free(title);

    vite_tab_set_modified(tab, FALSE);
    vite_tab_set_operation_type(tab, VITE_OP_NONE);
    vite_tab_set_loading(tab, FALSE);

    if (vite_tab_is_active(tab)) {
        update_window_title_for_tab(tab);
    }
}

static void
remember_recently_closed_file(const char *path)
{
    if (!path || !*path) return;
    for (GList *l = recently_closed_files; l; l = l->next) {
        if (g_strcmp0((const char *)l->data, path) == 0) {
            g_free(l->data);
            recently_closed_files = g_list_delete_link(recently_closed_files, l);
            break;
        }
    }
    recently_closed_files = g_list_prepend(recently_closed_files, g_strdup(path));
    if (g_list_length(recently_closed_files) > MAX_RECENTLY_CLOSED) {
        GList *last = g_list_last(recently_closed_files);
        if (last) {
            g_free(last->data);
            recently_closed_files = g_list_delete_link(recently_closed_files, last);
        }
    }
}

static char *
pop_recently_closed_file(void)
{
    if (!recently_closed_files) return NULL;
    GList *first = recently_closed_files;
    char *path = first->data;
    recently_closed_files = g_list_delete_link(recently_closed_files, first);
    return path;
}

/* Reload-from-disk implementation */
static ViteTab *
find_tab_for_widget(GtkWidget *widget)
{
    while (widget) {
        ViteTab *tab = g_object_get_data(G_OBJECT(widget), "tab");
        if (tab && VITE_IS_TAB(tab)) {
            return tab;
        }
        widget = gtk_widget_get_parent(widget);
    }
    return NULL;
}

static gboolean
reload_tab_from_disk(ViteWindow *win, ViteTab *tab)
{
    GtkWidget *page;
    GtkWidget *editor;
    Document *doc;
    const char *path;
    GPtrArray *editors;
    LoadContext *ctx;
    GCancellable *cancellable;

    if (!win || !tab) return FALSE;
    if (vite_tab_is_loading(tab)) return FALSE;

    page = g_object_get_data(G_OBJECT(tab), "page");
    if (!page) return FALSE;

    editor = get_editor_from_page(page);
    if (!editor || !EDITOR_IS_WIDGET(editor)) return FALSE;

    doc = editor_widget_get_document(EDITOR_WIDGET(editor));
    if (!doc) return FALSE;

    path = document_get_file_path(doc);
    if (!path || !*path) return FALSE;

    editors = find_all_editors_recursive(page);
    for (guint i = 0; i < editors->len; i++) {
        GtkWidget *ed = g_ptr_array_index(editors, i);
        gtk_widget_set_sensitive(ed, FALSE);
    }
    g_ptr_array_unref(editors);

    vite_tab_set_operation_type(tab, VITE_OP_LOADING);
    vite_tab_set_loading(tab, TRUE);
    update_window_title_for_tab(tab);
    update_reload_action_enabled(win, editor);

    win->loading_count++;
    update_header_spinner(win);

    ctx = g_new0(LoadContext, 1);
    ctx->tab = tab;
    ctx->tab_bar = win->tab_bar;
    ctx->header_progress = win->header_progress;
    ctx->header_spinner = win->header_spinner;
    ctx->filename = g_strdup(path);
    ctx->window = win;
    ctx->gtkw_ref = GTK_WINDOW(win->window);
    ctx->doc = document_ref(doc);
    ctx->is_reload = TRUE;
    ctx->prev_encoding = document_get_encoding(doc);
    ctx->prev_newline = document_get_newline_type(doc);
    ctx->has_prev_format = TRUE;

    g_object_add_weak_pointer(G_OBJECT(tab), (gpointer *)&ctx->tab);
    if (ctx->tab_bar) {
        g_object_add_weak_pointer(G_OBJECT(ctx->tab_bar), (gpointer *)&ctx->tab_bar);
    }
    if (ctx->header_progress) {
        g_object_add_weak_pointer(G_OBJECT(ctx->header_progress), (gpointer *)&ctx->header_progress);
    }
    if (ctx->header_spinner) {
        g_object_add_weak_pointer(G_OBJECT(ctx->header_spinner), (gpointer *)&ctx->header_spinner);
    }
    if (ctx->gtkw_ref) {
        g_object_add_weak_pointer(G_OBJECT(ctx->gtkw_ref), (gpointer *)&ctx->gtkw_ref);
    }

    document_set_progress_callback(doc, on_load_progress, ctx);

    cancellable = g_cancellable_new();
    vite_tab_set_cancellable(tab, cancellable);
    document_load_file_async(doc, path, cancellable, on_load_complete, ctx);
    g_object_unref(cancellable);

    return TRUE;
}

static gboolean
reload_active_tab_from_disk(ViteWindow *win)
{
    ViteTab *tab;

    if (!win || !win->tab_bar) return FALSE;
    tab = vite_tab_bar_get_active_tab(win->tab_bar);
    if (!tab) return FALSE;
    return reload_tab_from_disk(win, tab);
}

static void
update_reload_action_enabled(ViteWindow *win, GtkWidget *editor)
{
    GAction *act;
    gboolean enabled = FALSE;

    if (!win || !win->window) return;

    act = g_action_map_lookup_action(G_ACTION_MAP(win->window), "discard-changes");
    if (!act || !G_IS_SIMPLE_ACTION(act)) return;

    if (editor && EDITOR_IS_WIDGET(editor)) {
        Document *doc = editor_widget_get_document(EDITOR_WIDGET(editor));
        if (doc && document_get_file_path(doc)) {
            ViteTab *tab = find_tab_for_widget(editor);
            enabled = (!tab || !vite_tab_is_loading(tab));
        }
    }

    g_simple_action_set_enabled(G_SIMPLE_ACTION(act), enabled);
}

static void
on_discard_all_response(AdwAlertDialog *dialog G_GNUC_UNUSED, const char *response, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (g_strcmp0(response, "reload") == 0) {
        reload_active_tab_from_disk(win);
    }
}



static void
on_discard_all_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    gboolean modified = FALSE;
    
    /* Check active tab */
    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    if (!tab) return;
    
    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    GtkWidget *editor = get_editor_from_page(page);
    if (!editor) return;
    
    Document *doc = editor_widget_get_document(EDITOR_WIDGET(editor));
    if (!doc || !document_get_file_path(doc)) return;

    modified = document_is_modified(doc);
    if (!modified) {
        reload_active_tab_from_disk(win);
        return;
    }
    
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Reload from Disk?"),
                                                 _("This will replace the current tab with file contents from disk and clear undo/redo history. Unsaved changes will be lost.")));
    
    adw_alert_dialog_add_response(dialog, "cancel", _("Cancel"));
    adw_alert_dialog_add_response(dialog, "reload", _("Reload"));
    adw_alert_dialog_set_response_appearance(dialog, "reload", ADW_RESPONSE_DESTRUCTIVE);
    
    adw_alert_dialog_set_default_response(dialog, "cancel");
    adw_alert_dialog_set_close_response(dialog, "cancel");
    
    /* Handled by helper to initiate the actual reload logic */
    /* We need to pass win to identify the specific tab/doc later if we wanted to be robust, 
       but for now we re-lookup active tab which is fine for modal dialog flow. */
    
    /* Wait, we need to actually implement the reload. pass win. */
    g_signal_connect(dialog, "response", G_CALLBACK(on_discard_all_response), win);
    
    adw_alert_dialog_choose(dialog, GTK_WIDGET(win->window), NULL, NULL, NULL);
}

static void
on_print_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    GtkWidget *editor = get_active_editor(win);
    
    if (EDITOR_IS_WIDGET(editor)) {
         editor_print_start(EDITOR_WIDGET(editor));
    }
}

#if 0
static gboolean
auto_hide_header(gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (win) {
        win->auto_hide_source_id = 0;
        if (win->titlebar_container) {
            adw_toolbar_view_set_reveal_top_bars(ADW_TOOLBAR_VIEW(win->titlebar_container), FALSE);
        }
    }
    return G_SOURCE_REMOVE;
}
#endif

static void
restore_ui_after_fullscreen_real(ViteWindow *win)
{
    if (!win || !win->window) return;
    
    /* Double check we are still not fullscreen (user didn't toggle back rapidly) */
    if (!gtk_window_is_fullscreen(GTK_WINDOW(win->window))) {
        if (win->titlebar_container) {
             adw_toolbar_view_set_reveal_top_bars(ADW_TOOLBAR_VIEW(win->titlebar_container), TRUE);
             adw_toolbar_view_set_extend_content_to_top_edge(ADW_TOOLBAR_VIEW(win->titlebar_container), FALSE);
             
             adw_toolbar_view_set_reveal_bottom_bars(ADW_TOOLBAR_VIEW(win->titlebar_container), TRUE);
             adw_toolbar_view_set_extend_content_to_bottom_edge(ADW_TOOLBAR_VIEW(win->titlebar_container), FALSE);
        }
        if (win->header_bar) adw_header_bar_set_show_end_title_buttons(win->header_bar, TRUE);
        if (win->fullscreen_restore_btn) gtk_widget_set_visible(win->fullscreen_restore_btn, FALSE);
    }
}

static void
on_window_fullscreen_state_changed(GtkWindow *window, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    gboolean is_fullscreen = gtk_window_is_fullscreen(window);
    
    if (is_fullscreen) {
        /* ENTERING FULLSCREEN */
        if (win->titlebar_container) {
             /* Extend content to edges so bars overlay content */
             adw_toolbar_view_set_extend_content_to_top_edge(ADW_TOOLBAR_VIEW(win->titlebar_container), TRUE);
             adw_toolbar_view_set_reveal_top_bars(ADW_TOOLBAR_VIEW(win->titlebar_container), FALSE);
             
             adw_toolbar_view_set_extend_content_to_bottom_edge(ADW_TOOLBAR_VIEW(win->titlebar_container), TRUE);
             adw_toolbar_view_set_reveal_bottom_bars(ADW_TOOLBAR_VIEW(win->titlebar_container), FALSE);
        }
        if (win->header_bar) adw_header_bar_set_show_end_title_buttons(win->header_bar, FALSE);
        if (win->fullscreen_restore_btn) gtk_widget_set_visible(win->fullscreen_restore_btn, TRUE);
        
    } else {
        /* EXITING FULLSCREEN */
        if (win->auto_hide_source_id > 0) {
            g_source_remove(win->auto_hide_source_id);
            win->auto_hide_source_id = 0;
        }
        restore_ui_after_fullscreen_real(win);
    }
}

static void
on_fullscreen_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (!win || !win->window) return;
    
    /* Just toggle state; listener handles UI */
    if (gtk_window_is_fullscreen(GTK_WINDOW(win->window))) {
        gtk_window_unfullscreen(GTK_WINDOW(win->window));
    } else {
        gtk_window_fullscreen(GTK_WINDOW(win->window));
    }
}

static void
on_shortcuts_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;

    const char *shortcuts_ui_1 = 
    "<?xml version='1.0' encoding='UTF-8'?>"
    "<interface>"
    "  <object class='GtkShortcutsWindow' id='shortcuts_window'>"
    "    <property name='modal'>1</property>"
    "    <child>"
    "      <object class='GtkShortcutsSection'>"
    "        <property name='section-name'>shortcuts</property>"
    "        <property name='max-height'>10</property>"
    "        <child>"
    "          <object class='GtkShortcutsGroup'>"
    "            <property name='title' translatable='yes'>File Operations</property>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;T</property>"
    "                <property name='title' translatable='yes'>New Tab</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;N</property>"
    "                <property name='title' translatable='yes'>New Window</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;O</property>"
    "                <property name='title' translatable='yes'>Open File</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;S</property>"
    "                <property name='title' translatable='yes'>Save</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;&lt;shift&gt;S</property>"
    "                <property name='title' translatable='yes'>Save As</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>F5</property>"
    "                <property name='title' translatable='yes'>Reload</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;W</property>"
    "                <property name='title' translatable='yes'>Close Tab</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;&lt;shift&gt;T</property>"
    "                <property name='title' translatable='yes'>Reopen Closed Tab</property>"
    "              </object>"
    "            </child>"
    "          </object>"
    "        </child>";

    const char *shortcuts_ui_2 = 
    "        <child>"
    "          <object class='GtkShortcutsGroup'>"
    "            <property name='title' translatable='yes'>Editor Actions</property>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;P</property>"
    "                <property name='title' translatable='yes'>Print</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;Q</property>"
    "                <property name='title' translatable='yes'>Quit</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;Z</property>"
    "                <property name='title' translatable='yes'>Undo</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;Y</property>"
    "                <property name='title' translatable='yes'>Redo</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;X</property>"
    "                <property name='title' translatable='yes'>Cut</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;C</property>"
    "                <property name='title' translatable='yes'>Copy</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;V</property>"
    "                <property name='title' translatable='yes'>Paste</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;&lt;shift&gt;F</property>"
    "                <property name='title' translatable='yes'>Filter Lines</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;F</property>"
    "                <property name='title' translatable='yes'>Find</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;H</property>"
    "                <property name='title' translatable='yes'>Replace</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;G</property>"
    "                <property name='title' translatable='yes'>Go to Line</property>"
    "              </object>"
    "            </child>"
    "          </object>"
    "        </child>";

    const char *shortcuts_ui_3 = 
    "        <child>"
    "          <object class='GtkShortcutsGroup'>"
    "            <property name='title' translatable='yes'>View Options</property>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>F11</property>"
    "                <property name='title' translatable='yes'>Fullscreen</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;plus</property>"
    "                <property name='title' translatable='yes'>Zoom In</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;minus</property>"
    "                <property name='title' translatable='yes'>Zoom Out</property>"
    "              </object>"
    "            </child>"
    "            <child>"
    "              <object class='GtkShortcutsShortcut'>"
    "                <property name='accelerator'>&lt;ctrl&gt;0</property>"
    "                <property name='title' translatable='yes'>Reset Zoom</property>"
    "              </object>"
    "            </child>"
    "          </object>"
    "        </child>"
    "      </object>"
    "    </child>"
    "  </object>"
    "</interface>";

    char *tmp_ui = g_strconcat(shortcuts_ui_1, shortcuts_ui_2, NULL);
    char *shortcuts_ui = g_strconcat(tmp_ui, shortcuts_ui_3, NULL);
    g_free(tmp_ui);
    GtkBuilder *builder = gtk_builder_new_from_string(shortcuts_ui, -1);
    g_free(shortcuts_ui);
    GtkWidget *win_shortcuts = GTK_WIDGET(gtk_builder_get_object(builder, "shortcuts_window"));
    
    if (win_shortcuts) {
        gtk_window_set_default_size(GTK_WINDOW(win_shortcuts), 400, 500);
        gtk_window_set_transient_for(GTK_WINDOW(win_shortcuts), GTK_WINDOW(win->window));
        gtk_window_present(GTK_WINDOW(win_shortcuts));
    }

    g_object_unref(builder);
}

static void
on_about_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    
    AdwAboutDialog *about = ADW_ABOUT_DIALOG(adw_about_dialog_new());
    gtk_widget_add_css_class(GTK_WIDGET(about), "vite-about-dialog");
    adw_about_dialog_set_application_name(about, "ViTE");
    adw_about_dialog_set_application_icon(about, "io.github.fastrizwaan.ViTE");
    adw_about_dialog_set_version(about, "0.1");
    adw_about_dialog_set_developer_name(about, "Mohammed Asif Ali Rizvan");
    adw_about_dialog_set_license_type(about, GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_comments(about, "A Virtual Text Editor built with GTK4 and Libadwaita.");
    adw_about_dialog_set_website(about, "https://github.com/fastrizwaan/ViTE");
    adw_about_dialog_set_issue_url(about, "https://github.com/fastrizwaan/ViTE/issues");
    
    adw_about_dialog_add_credit_section(about, "Created By", (const char *[]) { "Mohammed Asif Ali Rizvan", NULL });
    
    adw_dialog_present(ADW_DIALOG(about), GTK_WIDGET(win->window));
}

static void
on_open_dialog_response(GtkFileDialog *dialog, GAsyncResult *result, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    GListModel *files = gtk_file_dialog_open_multiple_finish(dialog, result, NULL);
    if (files) {
        guint n = g_list_model_get_n_items(files);
        for (guint i = 0; i < n; i++) {
            GFile *file = g_list_model_get_item(files, i);
            queue_open_file(gtk_window_get_application(GTK_WINDOW(win->window)), win, file, TRUE);
            g_object_unref(file);
        }
        g_object_unref(files);
    }
}

static void
on_open_btn_clicked(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    ViteWindow *win = NULL;
    if (btn) {
         GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(btn));
         win = g_object_get_data(G_OBJECT(root), "vite-window");
    }
    
    /* Fallback? We shouldn't need it if triggered from UI */
    if (!win) return;
    
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(win->window), NULL, (GAsyncReadyCallback)on_open_dialog_response, win);
}

static void update_window_title_for_tab(ViteTab *tab);

/* ============================================================================
 * Widget Lifecycle Helper Functions
 * These provide safe, consistent patterns for GtkStack widget manipulation.
 * ============================================================================ */

/* Forward Declarations */
static void on_editor_notify_indentation(GObject *editor, GParamSpec *pspec, gpointer user_data);
static void update_status_bar_from_editor(ViteWindow *win, GtkWidget *editor);

/**
 * Safely removes a child from a GtkStack with transition locking.
 * Prevents snapshot assertions by disabling transitions during removal.
 */
static void
stack_safe_remove_child(GtkStack *stack, GtkWidget *child)
{
    if (!stack || !child) return;
    if (gtk_widget_get_parent(child) != GTK_WIDGET(stack)) return; /* Already removed or wrong parent */
    
    guint dur = gtk_stack_get_transition_duration(stack);
    gtk_stack_set_transition_duration(stack, 0);
    gtk_widget_set_visible(child, FALSE);
    gtk_stack_remove(stack, child); /* Use gtk_stack_remove for proper bookkeeping */
    gtk_stack_set_transition_duration(stack, dur);
}

/**
 * Safely adds a child to a GtkStack and makes it visible.
 * Prevents snapshot assertions by disabling transitions during addition.
 * Always generates a unique name to avoid collisions.
 */
static void
stack_safe_add_child(GtkStack *stack, GtkWidget *child, const char *base_name)
{
    if (!stack || !child) return;
    
    /* If already parented to this stack, just make visible */
    if (gtk_widget_get_parent(child) == GTK_WIDGET(stack)) {
        gtk_widget_set_visible(child, TRUE);
        gtk_stack_set_visible_child(stack, child);
        return;
    }
    
    guint dur = gtk_stack_get_transition_duration(stack);
    gtk_stack_set_transition_duration(stack, 0);
    
    /* Always generate unique name to prevent any collision */
    char name[128];
    snprintf(name, sizeof(name), "%s_%u", base_name, g_random_int());
    
    gtk_stack_add_named(stack, child, name);
    gtk_widget_set_visible(child, TRUE);
    gtk_stack_set_visible_child(stack, child);
    gtk_stack_set_transition_duration(stack, dur);
}

/**
 * Callback for deferred focus restoration.
 * Runs after the current event loop iteration when hierarchy is stable.
 */
static gboolean
focus_widget_idle_cb(gpointer user_data)
{
    GtkWidget *widget = GTK_WIDGET(user_data);
    if (widget && GTK_IS_WIDGET(widget)) {
        GtkRoot *root = gtk_widget_get_root(widget);
        if (root && GTK_IS_WINDOW(root)) {
            gtk_widget_grab_focus(widget);
        }
    }
    g_object_unref(widget);
    return G_SOURCE_REMOVE;
}

/**
 * Schedules focus restoration to run after the current event loop.
 * This avoids gtk_window_get_focus assertions during hierarchy changes.
 */
static void
defer_focus(GtkWidget *widget)
{
    if (widget && GTK_IS_WIDGET(widget)) {
        g_object_ref(widget);
        g_idle_add(focus_widget_idle_cb, widget);
    }
}

static void
update_header_spinner(ViteWindow *win)
{
    if (!win->header_spinner) return;
    
    /* Show logic: 
       - Must have active loading (loading_count > 0)
       - Tab bar must NOT be visible (avoids redundancy)
    */
    gboolean active = (win->loading_count > 0);
    gboolean tab_bar_visible = win->tab_bar && gtk_widget_get_visible(GTK_WIDGET(win->tab_bar));
    
    if (active && !tab_bar_visible) {
        if (!gtk_widget_get_visible(win->header_spinner)) {
            gtk_widget_set_visible(win->header_spinner, TRUE);
            gtk_spinner_start(GTK_SPINNER(win->header_spinner));
        }
    } else {
        if (gtk_widget_get_visible(win->header_spinner)) {
            gtk_spinner_stop(GTK_SPINNER(win->header_spinner));
            gtk_widget_set_visible(win->header_spinner, FALSE);
        }
    }
    
    check_close_when_done(win);
}

static void
on_tab_bar_visible_changed(GObject *obj G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    update_header_spinner(win);
}

/* Forward declarations */
static void close_split_view(GtkWidget *overlay);
static void update_window_title_for_tab(ViteTab *tab);

static void
on_overlay_focus_leave(GtkEventControllerFocus *controller G_GNUC_UNUSED, gpointer user_data)
{
    (void)user_data;
}

static void
on_overlay_focus_enter(GtkEventControllerFocus *controller G_GNUC_UNUSED, gpointer user_data)
{
    GtkWidget *overlay = GTK_WIDGET(user_data);
    
    /* Track last active editor for global menu actions when focus is lost (e.g. to popover) */
    GtkWidget *child = gtk_overlay_get_child(GTK_OVERLAY(overlay));
    GtkWidget *editor = NULL;
    
    if (child) {
        if (GTK_IS_SCROLLED_WINDOW(child)) {
            editor = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(child));
        } else if (EDITOR_IS_WIDGET(child)) {
            editor = child;
        }
    }
    
    if (editor && EDITOR_IS_WIDGET(editor)) {
        GtkRoot *root = gtk_widget_get_root(overlay);
        if (root) {
            ViteWindow *win = g_object_get_data(G_OBJECT(root), "vite-window");
            if (win) win->last_active_editor = editor;
        }
    }
    
    /* Find the ViteTab ancestor */
    GtkWidget *iter = overlay;
    while (iter && !VITE_IS_TAB(iter)) {
        GtkWidget *parent = gtk_widget_get_parent(iter);
        if (!parent) break;
        iter = parent;
        
        ViteTab *tab = g_object_get_data(G_OBJECT(iter), "tab");
        if (tab) {
             vite_tab_set_last_focused_child(tab, overlay);
             return;
        }
    }
}

/* Retry logic with correct pre-fetch */


/* Retry logic with correct pre-fetch */

static void
on_find_bar_progress(ViteFindReplaceBar *bar, double progress, gboolean busy, gpointer user_data G_GNUC_UNUSED)
{
    /* Find the tab containing this bar */
    GtkWidget *widget = GTK_WIDGET(bar);
    GtkRoot *root = gtk_widget_get_root(widget);
    if (!root) return;
    
    ViteWindow *win = g_object_get_data(G_OBJECT(root), "vite-window");
    if (!win) return;
    
    /* Iterate tabs to find which one owns the page containing this bar */
    /* The bar is in a ViewContainer, which is in a StackPage usually, or nested in paneds */
    /* Scan tabs */
    GList *tabs = vite_tab_bar_get_tabs(win->tab_bar);
    ViteTab *found_tab = NULL;
    
    for (GList *l = tabs; l != NULL; l = l->next) {
        ViteTab *t = VITE_TAB(l->data);
        GtkWidget *page = g_object_get_data(G_OBJECT(t), "page");
        if (page && gtk_widget_is_ancestor(widget, page)) {
            found_tab = t;
            break;
        }
    }
    g_list_free(tabs);
    
    if (found_tab) {
        if (busy) {
            /* For replace operations, only show progress bar, not spinner */
            vite_tab_set_progress(found_tab, progress);
        } else {
            vite_tab_set_progress(found_tab, 0.0);
        }
    }
}

static void
on_editor_undo_redo_progress(EditorWidget *editor, double progress, gboolean busy, gpointer user_data G_GNUC_UNUSED)
{
    /* Find the tab containing this editor */
    GtkWidget *widget = GTK_WIDGET(editor);
    GtkRoot *root = gtk_widget_get_root(widget);
    if (!root) return;
    
    ViteWindow *win = g_object_get_data(G_OBJECT(root), "vite-window");
    if (!win) return;
    
    /* Find the tab that owns this editor */
    GList *tabs = vite_tab_bar_get_tabs(win->tab_bar);
    ViteTab *found_tab = NULL;
    
    for (GList *l = tabs; l != NULL; l = l->next) {
        ViteTab *t = VITE_TAB(l->data);
        GtkWidget *page = g_object_get_data(G_OBJECT(t), "page");
        if (page && gtk_widget_is_ancestor(widget, page)) {
            found_tab = t;
            break;
        }
    }
    g_list_free(tabs);
    
    if (found_tab) {
        if (busy) {
            /* Show spinner and progress on the tab itself */
            vite_tab_set_loading(found_tab, TRUE);
            vite_tab_set_progress(found_tab, progress);
            
            /* Update headerbar progress only if requested conditions met 
               User wants header progress when it's "header only" (single tab)
            */
            if (win->header_progress) {
                GList *all_tabs = vite_tab_bar_get_tabs(win->tab_bar);
                guint tab_count = g_list_length(all_tabs);
                g_list_free(all_tabs);
                
                if (tab_count <= 1) {
                    gtk_widget_set_visible(win->header_progress, TRUE);
                    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(win->header_progress), progress);
                } else {
                    gtk_widget_set_visible(win->header_progress, FALSE);
                }
            }
        } else {
            /* Hide spinner and progress */
            vite_tab_set_loading(found_tab, FALSE);
            vite_tab_set_progress(found_tab, 0.0);
            
            if (win->header_progress) {
                gtk_widget_set_visible(win->header_progress, FALSE);
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(win->header_progress), 0.0);
            }
        }
    }
}


static GtkWidget *
create_view_container(ViteWindow *win, GtkWidget *editor)
{
    /* Use a Box as the root for potential future widgets (e.g. breadcrumbs),
       but FindBar moves to Overlay. */
    GtkWidget *root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root_box, "view-split");
    
    /* Create Overlay to hold Editor */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(overlay, TRUE);
    gtk_widget_set_vexpand(overlay, TRUE);
    
    /* Add Editor to Overlay */
    gtk_widget_set_hexpand(editor, TRUE);
    gtk_widget_set_vexpand(editor, TRUE);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), editor);
    
    /* Add Overlay to Root Box */
    gtk_box_append(GTK_BOX(root_box), overlay);
    
    g_object_set_data(G_OBJECT(root_box), "vite-window", win);

    /* Track focus for splitting - track on overlay since that is the split unit */
    GtkEventController *controller = gtk_event_controller_focus_new();
    g_signal_connect(controller, "enter", G_CALLBACK(on_overlay_focus_enter), overlay);
    g_signal_connect(controller, "leave", G_CALLBACK(on_overlay_focus_leave), overlay);
    gtk_widget_add_controller(overlay, controller);
    
    /* Unwrap editor if needed for FindBar access */
    GtkWidget *real_editor = editor;
    if (GTK_IS_SCROLLED_WINDOW(real_editor)) {
         real_editor = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(real_editor));
    }

    /* Add Find/Replace Bar to Root Box (below overlay) */
    if (EDITOR_IS_WIDGET(real_editor)) {
        GtkWidget *find_bar = vite_find_replace_bar_new(EDITOR_WIDGET(real_editor));
        gtk_widget_set_visible(find_bar, FALSE);
        
        /* Full width integrated bar */
        gtk_widget_set_halign(find_bar, GTK_ALIGN_FILL);
        gtk_widget_set_valign(find_bar, GTK_ALIGN_START);
        
        gtk_widget_set_margin_top(find_bar, 0);
        gtk_widget_set_margin_bottom(find_bar, 0);
        
        gtk_box_append(GTK_BOX(root_box), find_bar);
        
        /* Store logical association */
        g_object_set_data(G_OBJECT(root_box), "find_bar", find_bar);
        
        g_signal_connect(find_bar, "progress-changed", G_CALLBACK(on_find_bar_progress), win);
        
    }
    
    /* External Change Revealer */
    GtkWidget *revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    
    GtkWidget *info_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(info_box, "view-split"); /* gives it a nice background/border */
    gtk_widget_set_margin_start(info_box, 12);
    gtk_widget_set_margin_end(info_box, 12);
    gtk_widget_set_margin_top(info_box, 6);
    gtk_widget_set_margin_bottom(info_box, 6);
    
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
    GtkWidget *label = gtk_label_new(_("This file has been modified by another program."));
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    
    GtkWidget *btn_reload = gtk_button_new_with_label(_("Reload"));
    GtkWidget *btn_ignore = gtk_button_new_with_label(_("Ignore"));
    gtk_widget_add_css_class(btn_reload, "suggested-action");
    
    gtk_box_append(GTK_BOX(info_box), icon);
    gtk_box_append(GTK_BOX(info_box), label);
    gtk_box_append(GTK_BOX(info_box), btn_ignore);
    gtk_box_append(GTK_BOX(info_box), btn_reload);
    
    gtk_revealer_set_child(GTK_REVEALER(revealer), info_box);
    
    /* Insert at the top of the root box (index 0) */
    gtk_box_prepend(GTK_BOX(root_box), revealer);
    
    /* Store references */
    g_object_set_data(G_OBJECT(root_box), "external_change_revealer", revealer);
    g_object_set_data(G_OBJECT(btn_reload), "revealer", revealer);
    g_object_set_data(G_OBJECT(btn_ignore), "revealer", revealer);
    
    /* We'll pass the editor widget to extract the document inside the callbacks */
    g_signal_connect(btn_ignore, "clicked", G_CALLBACK(on_ignore_external_change), editor);
    g_signal_connect(btn_reload, "clicked", G_CALLBACK(on_reload_external_change), editor);

    return root_box;
}

#if 0
static gboolean
refresh_all_windows_idle(gpointer user_data)
{
    GtkApplication *app = GTK_APPLICATION(user_data);
    GList *windows = gtk_application_get_windows(app);
    for (GList *l = windows; l != NULL; l = l->next) {
        gtk_widget_queue_draw(GTK_WIDGET(l->data));
    }
    return G_SOURCE_REMOVE;
}
#endif

static GtkWidget *
get_scrolled_window_from_view(GtkWidget *view_root)
{
    /* Supports both old Overlay structure (if any legacy exists) and new Box structure */
    if (GTK_IS_OVERLAY(view_root)) {
        return gtk_overlay_get_child(GTK_OVERLAY(view_root));
    }
    else if (GTK_IS_BOX(view_root)) {
        /* Iterate children to find the Overlay, then get ScrolledWindow */
        GtkWidget *child = gtk_widget_get_first_child(view_root);
        while (child) {
            if (GTK_IS_OVERLAY(child)) {
                return gtk_overlay_get_child(GTK_OVERLAY(child));
            }
            child = gtk_widget_get_next_sibling(child);
        }
    }
    return NULL;
}





static void
on_preferences_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    /* connected with window as user_data */
    ViteWindow *win = (ViteWindow *)user_data;
    if (!win) return;
    
    /* Try to find editor from focus first */
    GtkWidget *focus = NULL;
    if (win->window && GTK_IS_WINDOW(win->window)) {
        focus = gtk_window_get_focus(GTK_WINDOW(win->window));
    }
    GtkWidget *editor = NULL;
    
    GtkWidget *iter = focus;
    while (iter) {
        if (EDITOR_IS_WIDGET(iter)) {
            editor = iter;
            break;
        }
        iter = gtk_widget_get_parent(iter);
    }
    
    /* Fallback: Active Tab */
    if (!editor) {
        ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
        if (tab) {
            GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
            if (page) {
                editor = find_first_editor_recursive(page);
            }
        }
    }
    
    if (editor) {
        show_preferences_dialog(GTK_WINDOW(win->window), EDITOR_WIDGET(editor));
    }
}

static GtkWidget *
get_editor_from_page(GtkWidget *page) {
    /* Use recursive finder to handle Box/Paned/Overlay hierarchy */
    return find_first_editor_recursive(page);
}

static void
update_window_title_for_tab(ViteTab *tab)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(tab));
    if (!root) return;
    
    ViteWindow *win = g_object_get_data(G_OBJECT(root), "vite-window");
    if (!win) return;
    
    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (!page) return;
    
    GtkWidget *editor = get_editor_from_page(page);
    if (!EDITOR_IS_WIDGET(editor)) return;
    
    Document *doc = editor_widget_get_document(EDITOR_WIDGET(editor));
    
    char *title = NULL;
    char *subtitle = NULL;
    const char *doc_path = document_get_file_path(doc);
    
    if (doc_path) {
        GFile *f = g_file_new_for_path(doc_path);
        title = g_file_get_basename(f);
        
        /* Null safety for basename */
        if (!title || !*title) {
            g_free(title);
            title = g_strdup("Untitled");
        }
        
        char *dir = g_path_get_dirname(doc_path);
        const char *home = g_get_home_dir();
        if (dir && home && g_str_has_prefix(dir, home)) {
            subtitle = g_strconcat("~", dir + strlen(home), NULL);
        } else if (dir) {
            subtitle = g_strdup(dir);
        } else {
            subtitle = g_strdup("");
        }
        g_free(dir);
        g_object_unref(f);
    } else {
        const char *tab_title = vite_tab_get_title(tab);
        title = g_strdup(tab_title && *tab_title ? tab_title : "Untitled");
        subtitle = g_strdup("Unsaved Document");
    }
    
    /* UTF-8 validation for title */
    if (title && !g_utf8_validate(title, -1, NULL)) {
        char *safe = g_utf8_make_valid(title, -1);
        g_free(title);
        title = safe;
    }
    
    if (document_is_modified(doc)) {
        char *tmp = g_strdup_printf("• %s", title);
        g_free(title);
        title = tmp;
    } else if (!doc_path) {
        /* Check if this is the only tab */
        GList *tabs = vite_tab_bar_get_tabs(win->tab_bar);
        if (g_list_length(tabs) == 1 && !vite_tab_is_loading(tab)) {
            /* Single untitled, unmodified tab -> Default Title */
            g_free(title);
            g_free(subtitle);
            title = g_strdup("Virtual Text Editor");
            subtitle = NULL;
        }
        g_list_free(tabs);
    }
    
    adw_window_title_set_title(win->window_title, title ? title : "Virtual Text Editor");
    adw_window_title_set_subtitle(win->window_title, subtitle);
    
    g_free(title);
    g_free(subtitle);
}

#if 0
/* Backward compatibility wrapper if needed, but we should update call sites */
static void
update_window_title(Document *doc G_GNUC_UNUSED)
{
    /* Deprecated - do nothing */
}
#endif

static void
on_document_content_changed(Document *doc, void *user_data)
{
    ViteTab *tab = VITE_TAB(user_data);
    if (!document_get_file_path(doc) && !vite_tab_is_loading(tab)) {
        size_t len;
        char *line = document_get_line(doc, 0, &len);
        if (line && len > 0) {
            /* Truncate to 20 characters (safe UTF-8) */
            if (g_utf8_validate(line, -1, NULL)) {
                long char_count = g_utf8_strlen(line, -1);
                if (char_count > 20) {
                    char *ptr = g_utf8_offset_to_pointer(line, 20);
                    if (ptr) *ptr = '\0';
                }
            } else {
                 /* Fallback for invalid UTF-8: just clamp bytes but respect boundaries? 
                    Actually, if invalid, just cap bytes but ensure we don't cut mid-sequence?
                    Let's just use strict byte limit of 20 but back off if continuation. */
                 if (len > 20) {
                     int cut = 20;
                     while (cut > 0 && (line[cut] & 0xC0) == 0x80) cut--;
                     line[cut] = '\0';
                 }
            } 
            
            /* Basic hygiene: remove newlines */
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            
            /* Check for leading whitespace/empty after strip */
            char *stripped = g_strstrip(g_strdup(line)); /* Duplicate because g_strstrip modifies in place */
            
            if (stripped[0] != '\0') {
                /* Use the STRIPPED version or original? User said: "when non white space is entered use it". 
                   "if the starting text is white space do not use it".
                   I'll use the STRIPPED version for the title, it looks cleaner. */
                vite_tab_set_title(tab, stripped);
            } else {
                 /* Text is all whitespace or empty - Restore original title */
                 const char *orig = g_object_get_data(G_OBJECT(tab), "original_title");
                 if (orig) vite_tab_set_title(tab, orig);
            }
            g_free(stripped);
        } else {
             /* Empty document */
             const char *orig = g_object_get_data(G_OBJECT(tab), "original_title");
             if (orig) vite_tab_set_title(tab, orig);
        }
        g_free(line);
        
        if (vite_tab_is_active(tab)) {
            update_window_title_for_tab(tab);
        }

        /* Auto-detect language if currently Plain Text */
        GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
        GtkWidget *editor = get_editor_from_page(page);
        if (EDITOR_IS_WIDGET(editor)) {
             SyntaxContext *ctx = editor_widget_get_syntax_context(EDITOR_WIDGET(editor));
             if (ctx && syntax_context_get_language(ctx) == LANG_NONE) {
                 /* Read sample */
                 char *sample = document_get_text_range(doc, 0, 1024);
                 if (sample) {
                     const char *detected = syntax_detect_language(sample);
                     if (detected) {
                         editor_widget_set_language(EDITOR_WIDGET(editor), detected);
                         if (vite_tab_is_active(tab)) {
                             on_tab_clicked(tab, NULL); /* Refresh status bar */
                         }
                     }
                     g_free(sample);
                 }
             }
        }
    }
}

static void
on_document_modified(Document *doc G_GNUC_UNUSED, gboolean modified, void *user_data)
{
    ViteTab *tab = VITE_TAB(user_data);
    GtkWidget *page;
    GtkWidget *editor;
    GtkRoot *root;
    ViteWindow *win;

    vite_tab_set_modified(tab, modified);
    
    /* Update window title if this is the active tab */
    if (vite_tab_is_active(tab)) {
        update_window_title_for_tab(tab);

        page = g_object_get_data(G_OBJECT(tab), "page");
        editor = page ? get_editor_from_page(page) : NULL;
        root = gtk_widget_get_root(GTK_WIDGET(tab));
        win = root ? g_object_get_data(G_OBJECT(root), "vite-window") : NULL;
        if (win) {
            update_reload_action_enabled(win, editor);
        }
    }
}

/* Transformations for GBinding */
static gboolean
transform_boolean_to_variant (GBinding     *binding G_GNUC_UNUSED,
                              const GValue *from_value,
                              GValue       *to_value,
                              gpointer      user_data G_GNUC_UNUSED)
{
  gboolean value = g_value_get_boolean (from_value);
  g_value_set_variant (to_value, g_variant_new_boolean (value));
  return TRUE;
}

static gboolean
transform_variant_to_boolean (GBinding     *binding G_GNUC_UNUSED,
                              const GValue *from_value,
                              GValue       *to_value,
                              gpointer      user_data G_GNUC_UNUSED)
{
  GVariant *variant = g_value_get_variant (from_value);
  if (variant && g_variant_is_of_type (variant, G_VARIANT_TYPE_BOOLEAN)) {
    g_value_set_boolean (to_value, g_variant_get_boolean (variant));
    return TRUE;
  }
  return FALSE;
}

static void
bind_action_to_editor (GActionMap *map, const char *action_name, GObject *editor, const char *prop_name)
{
    GAction *act = g_action_map_lookup_action(map, action_name);
    if (!act) return;
    
    /* Remove old binding if exists */
    GBinding *old_b = g_object_get_data(G_OBJECT(act), "editor-binding");
    if (old_b) {
        g_binding_unbind(old_b);
        g_object_set_data(G_OBJECT(act), "editor-binding", NULL);
    }
    
    if (editor) {
        /* Bind Editor Prop -> Action State. 
           SYNC_CREATE ensures Action state immediately matches Editor state.
           BIDIRECTIONAL ensures clicking Menu (Action State Change) updates Editor Prop.
        */
        GBinding *b = g_object_bind_property_full (editor, prop_name, act, "state",
                                     G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL,
                                     transform_boolean_to_variant,
                                     transform_variant_to_boolean,
                                     NULL, NULL);
        g_object_set_data(G_OBJECT(act), "editor-binding", b);
    }
}

static void
on_tab_clicked (ViteTab *tab, gpointer user_data)
{
    ViteWindow *win = NULL;
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(tab));
    if (root) {
        win = g_object_get_data(G_OBJECT(root), "vite-window");
    }


/* Fallback if called manually before rooting */
    if (!win && user_data) {
        win = (ViteWindow*)user_data;
    }

    if (!win) return;

    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (!page) return;

    /* Per-Tab Split Model: Just switch the stack to this tab's page */
    gtk_stack_set_visible_child(win->stack, page);
    vite_tab_bar_set_active_tab(win->tab_bar, tab);
    
    /* Update Status Bar */
    if (win && win->status_bar) {
        GtkWidget *editor = NULL;
        GtkWidget *focus = NULL;
        if (win->window && GTK_IS_WINDOW(win->window)) {
            focus = gtk_window_get_focus(GTK_WINDOW(win->window));
        }
        
        GtkWidget *iter = focus;
        while (iter) {
            if (EDITOR_IS_WIDGET(iter)) {
                editor = iter;
                break;
            }
            iter = gtk_widget_get_parent(iter);
        }
        
        /* Fallback: Active Tab's page */
        if (!editor && page) {
            editor = find_first_editor_recursive(page);
        }

        if (editor && EDITOR_IS_WIDGET(editor)) {
             size_t ln, col;
             editor_widget_get_cursor_position(EDITOR_WIDGET(editor), &ln, &col);
             vite_status_bar_set_cursor_position(VITE_STATUS_BAR(win->status_bar), ln + 1, col + 1);
             
             vite_status_bar_set_insert_mode(VITE_STATUS_BAR(win->status_bar), editor_widget_get_insert_mode(EDITOR_WIDGET(editor)));
             
             /* Also update File Type / Encoding / Line Endings using Document properties */
             const char *lang_name = editor_widget_get_language_name(EDITOR_WIDGET(editor));
             const char *orig_lang = g_object_get_data(G_OBJECT(editor), "original-file-type");
             if (!orig_lang) orig_lang = "Plain Text";
             gboolean lang_changed = (g_strcmp0(lang_name ? lang_name : "Plain Text", orig_lang) != 0);

             if (g_strcmp0(lang_name, "Plain Text") == 0) {
                 vite_status_bar_set_file_type(VITE_STATUS_BAR(win->status_bar), NULL, lang_changed);
             } else {
                 vite_status_bar_set_file_type(VITE_STATUS_BAR(win->status_bar), lang_name, lang_changed);
             }
             
             /* BIND Menu Actions to Editor Properties */
             /* This mirrors the logic in preferences.c, ensuring robust sync */
             GActionMap *map = G_ACTION_MAP(GTK_WINDOW(win->window));
             
             bind_action_to_editor(map, "show-line-numbers", G_OBJECT(editor), "show-line-numbers");
             bind_action_to_editor(map, "enable-word-wrap", G_OBJECT(editor), "wrap-lines");
             bind_action_to_editor(map, "enable-folding", G_OBJECT(editor), "enable-folding");
             bind_action_to_editor(map, "enable-minimap", G_OBJECT(editor), "minimap-enabled");
             
             /* Encoding/LineEnding Actions are stateful but we update them imperatively 
                because they don't map 1:1 to a boolean property via binding easily. */
             
             Document *doc = editor_widget_get_document(EDITOR_WIDGET(editor));
             if (doc) {
                 /* Encoding - Keep imperative */
                 FileEncoding enc = document_get_encoding(doc);
                 const char *enc_id = file_encoding_to_id(enc);
                 const char *orig_enc = g_object_get_data(G_OBJECT(editor), "original-encoding");
                 if (!orig_enc) orig_enc = "utf-8";
                 gboolean enc_changed = (g_strcmp0(enc_id, orig_enc) != 0);
                 
                 vite_status_bar_set_encoding(VITE_STATUS_BAR(win->status_bar), enc_id, enc_changed);
                 
                 /* Line Ending - Keep imperative */
                 NewlineType nl = document_get_newline_type(doc);
                 const char *nl_id = "lf";
                 if (nl == NEWLINE_CRLF) nl_id = "crlf";
                 else if (nl == NEWLINE_CR) nl_id = "cr";
                 const char *orig_nl = g_object_get_data(G_OBJECT(editor), "original-newline");
                 if (!orig_nl) orig_nl = "lf";
                 gboolean nl_changed = (g_strcmp0(nl_id, orig_nl) != 0);
                 
                 vite_status_bar_set_line_ending(VITE_STATUS_BAR(win->status_bar), nl_id, nl_changed);
                 
                 /* Encoding Action State */
                 const char *enc_key = file_encoding_to_id(enc);
                 GAction *act = g_action_map_lookup_action(map, "set-encoding");
                 if (act) g_simple_action_set_state(G_SIMPLE_ACTION(act), g_variant_new_string(enc_key));
                 
                 /* Line Ending Action State */
                 const char *nl_key = "lf";
                 if (nl == NEWLINE_CRLF) nl_key = "crlf";
                 else if (nl == NEWLINE_CR) nl_key = "cr";
                 act = g_action_map_lookup_action(map, "set-line-ending");
                 if (act) g_simple_action_set_state(G_SIMPLE_ACTION(act), g_variant_new_string(nl_key));
                  
                  /* Update Indentation Status */
                  int tab_w = 4;
                  int indent_s = 0;
                  g_object_get(G_OBJECT(editor), "tab-width", &tab_w, "indent-style", &indent_s, NULL);
                  
                  gpointer orig_w_ptr = g_object_get_data(G_OBJECT(editor), "original-indent-width");
                  gpointer orig_style_ptr = g_object_get_data(G_OBJECT(editor), "original-indent-style");
                  int orig_w = orig_w_ptr ? GPOINTER_TO_INT(orig_w_ptr) : 4;
                  int orig_style = orig_style_ptr ? GPOINTER_TO_INT(orig_style_ptr) : 0;
                  
                  gboolean indent_changed = (tab_w != orig_w || indent_s != orig_style);
                  vite_status_bar_set_indentation(VITE_STATUS_BAR(win->status_bar), tab_w, indent_s == 1, indent_changed);
                  
                  /* Update Folding Action State */
                  GAction *fold_act = g_action_map_lookup_action(map, "enable-folding");
                  if (fold_act) {
                      SyntaxContext *ctx = editor_widget_get_syntax_context(EDITOR_WIDGET(editor));
                      if (ctx && syntax_context_get_language(ctx) == LANG_NONE) {
                          g_simple_action_set_enabled(G_SIMPLE_ACTION(fold_act), FALSE);
                      } else {
                          g_simple_action_set_enabled(G_SIMPLE_ACTION(fold_act), TRUE);
                      }
                  }
             }
        }
    }
    
    update_reload_action_enabled(win, get_editor_from_page(page));

    /* Update title based on active view in this page */
    /* Update title based on active view in this page */
    update_window_title_for_tab(tab);
    
}

static void
close_split_view(GtkWidget *view_container)
{
    if (!view_container) return;
    
    GtkWidget *parent = gtk_widget_get_parent(view_container);
    if (!parent) return;
    
    /* Case 1: Parent is TabRoot (Box) -> Closing the last view */
    if (GTK_IS_BOX(parent)) {
        /* Find the tab associated with this view to close it */
        GtkRoot *root = gtk_widget_get_root(view_container);
        ViteWindow *win = g_object_get_data(G_OBJECT(root), "vite-window");
        
        if (win) {
            /* Check if parent is indeed a page root */
            GList *tabs = vite_tab_bar_get_tabs(win->tab_bar);
            for (GList *l = tabs; l != NULL; l = l->next) {
                ViteTab *t = VITE_TAB(l->data);
                GtkWidget *page = g_object_get_data(G_OBJECT(t), "page");
                if (page == parent) {
                    on_tab_close_clicked(t, NULL);
                    break;
                }
            }
            g_list_free(tabs);
        }
        return;
    }
    
    /* Case 2: Parent is Paned -> Collapsing a split */
    if (GTK_IS_PANED(parent)) {
        GtkPaned *paned = GTK_PANED(parent);
        GtkWidget *start = gtk_paned_get_start_child(paned);
        GtkWidget *end = gtk_paned_get_end_child(paned);
        
        GtkWidget *sibling = (start == view_container) ? end : start;
        
        /* If sibling is NULL, just remove myself */
        if (!sibling) {
             if (start == view_container) gtk_paned_set_start_child(paned, NULL);
             else gtk_paned_set_end_child(paned, NULL);
             return;
        }
        
        /* Promote sibling to replace parent */
        GtkWidget *grandparent = gtk_widget_get_parent(parent);
        if (!grandparent) return;
        
        /* Fix: Clear focus globally */
        GtkRoot *root = gtk_widget_get_root(view_container);
        ViteWindow *win = g_object_get_data(G_OBJECT(root), "vite-window");
        if (win && win->window) {
             gtk_window_set_focus(GTK_WINDOW(win->window), NULL);
        }
        
        g_object_ref(sibling); /* Protect sibling */
        
        /* Detach sibling */
        if (sibling == start) gtk_paned_set_start_child(paned, NULL);
        else gtk_paned_set_end_child(paned, NULL);
        
        /* Remove target */
        if (view_container == start) gtk_paned_set_start_child(paned, NULL);
        else gtk_paned_set_end_child(paned, NULL);
        
        /* Replace parent with sibling in grandparent */
        if (GTK_IS_BOX(grandparent)) {
            /* TabRoot */
            gtk_box_remove(GTK_BOX(grandparent), parent);
            gtk_box_append(GTK_BOX(grandparent), sibling);
        } else if (GTK_IS_PANED(grandparent)) {
            GtkPaned *gp = GTK_PANED(grandparent);
            if (gtk_paned_get_start_child(gp) == parent) {
                gtk_paned_set_start_child(gp, NULL);
                gtk_paned_set_start_child(gp, sibling);
            } else {
                gtk_paned_set_end_child(gp, NULL);
                gtk_paned_set_end_child(gp, sibling);
            }
        }
        
        g_object_unref(sibling);
        
        /* Restore focus */
        GtkWidget *new_focus = find_first_editor_recursive(sibling);
        if (new_focus) {
            defer_focus(new_focus);
        }
    }
}

static gboolean
replace_view_with_paned(GtkWidget *view_container, GtkWidget *paned)
{
    GtkWidget *parent = gtk_widget_get_parent(view_container);
    if (!parent) return FALSE;

    g_object_ref(view_container);

    if (GTK_IS_BOX(parent)) {
        gtk_box_remove(GTK_BOX(parent), view_container);
        gtk_box_append(GTK_BOX(parent), paned);
    } else if (GTK_IS_PANED(parent)) {
        if (gtk_paned_get_start_child(GTK_PANED(parent)) == view_container) {
            gtk_paned_set_start_child(GTK_PANED(parent), NULL);
            gtk_paned_set_start_child(GTK_PANED(parent), paned);
        } else {
            gtk_paned_set_end_child(GTK_PANED(parent), NULL);
            gtk_paned_set_end_child(GTK_PANED(parent), paned);
        }
    } else {
        g_object_unref(view_container);
        return FALSE;
    }

    gtk_paned_set_start_child(GTK_PANED(paned), view_container);
    g_object_unref(view_container);
    return TRUE;
}

static GtkWidget *
promote_to_view_container(GtkWidget *widget, GtkWidget *page)
{
    GtkWidget *view_container = widget;
    while (view_container && !gtk_widget_has_css_class(view_container, "view-split") && view_container != page) {
        view_container = gtk_widget_get_parent(view_container);
    }
    if (!view_container || !gtk_widget_has_css_class(view_container, "view-split")) return NULL;
    return view_container;
}

static GtkWidget *
resolve_split_target_view(ViteWindow *win, ViteTab *tab, GtkWidget *page, gboolean require_editor)
{
    GtkWidget *target_overlay = vite_tab_get_last_focused_child(tab);

    if (!target_overlay && win->window) {
        GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(win->window));
        if (focus && gtk_widget_is_ancestor(focus, page)) {
            target_overlay = gtk_widget_get_ancestor(focus, GTK_TYPE_OVERLAY);
        }
    }

    GtkWidget *view_container = promote_to_view_container(target_overlay, page);
    if (view_container && require_editor) {
        GtkWidget *editor = find_first_editor_recursive(view_container);
        if (!EDITOR_IS_WIDGET(editor)) view_container = NULL;
    }

    if (!view_container) {
        GtkWidget *fallback_editor = find_first_editor_recursive(page);
        if (fallback_editor) {
            view_container = promote_to_view_container(fallback_editor, page);
        }
    }

    return view_container;
}

static void
do_split(ViteWindow *win, GtkOrientation orientation)
{
    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    if (!tab) return;
    
    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (!page) return;

    GtkWidget *view_container = resolve_split_target_view(win, tab, page, TRUE);
    if (!view_container) return;
    
    GtkWidget *old_editor = find_first_editor_recursive(view_container);
    if (!EDITOR_IS_WIDGET(old_editor)) return;

    GtkWidget *paned = gtk_paned_new(orientation);
    if (!replace_view_with_paned(view_container, paned)) return;
    
    /* End child is NEW view */
    Document *doc = editor_widget_get_document(EDITOR_WIDGET(old_editor));
    
    GtkWidget *new_scrolled = gtk_scrolled_window_new();
    GtkWidget *new_editor = editor_widget_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(new_scrolled), new_editor);
    
    /* SYNC: Set same document and syntax context */
    editor_widget_set_document(EDITOR_WIDGET(new_editor), doc);
    
    SyntaxContext *ctx = editor_widget_get_syntax_context(EDITOR_WIDGET(old_editor));
    if (ctx) {
        editor_widget_set_syntax_context(EDITOR_WIDGET(new_editor), ctx);
    }
    
    /* SYNC: Settings */
    editor_widget_set_show_line_numbers(EDITOR_WIDGET(new_editor), editor_widget_get_show_line_numbers(EDITOR_WIDGET(old_editor)));
    editor_widget_set_word_wrap(EDITOR_WIDGET(new_editor), editor_widget_get_word_wrap(EDITOR_WIDGET(old_editor)));
    editor_widget_set_insert_mode(EDITOR_WIDGET(new_editor), editor_widget_get_insert_mode(EDITOR_WIDGET(old_editor)));
    
    /* Connect Indentation Sync Signals */
    g_signal_connect(new_editor, "notify::tab-width", G_CALLBACK(on_editor_notify_indentation), win);
    g_signal_connect(new_editor, "notify::indent-style", G_CALLBACK(on_editor_notify_indentation), win);
    g_signal_connect(new_editor, "notify::indent-width", G_CALLBACK(on_editor_notify_indentation), win);
    
    /* SYNC: Scroll and Zoom (Optional, but good for split)
       For now, just copy the language if it was manually overridden (redundant if shared but safe)
    */
    
    GtkWidget *new_view_container = create_view_container(win, new_scrolled);
    gtk_paned_set_end_child(GTK_PANED(paned), new_view_container);
    
    int size = (orientation == GTK_ORIENTATION_HORIZONTAL) ? gtk_widget_get_width(view_container) : gtk_widget_get_height(view_container);
    if (size > 0) gtk_paned_set_position(GTK_PANED(paned), size / 2);
    
    defer_focus(new_editor);
}

#ifdef HAVE_VTE
typedef struct {
    GWeakRef terminal_ref;
} TerminalTracker;

static GPtrArray *terminal_trackers = NULL; /* TerminalTracker* */
static gulong terminal_theme_changed_id = 0;

static char *
terminal_split_cwd(ViteWindow *win)
{
    GtkWidget *active = get_active_editor(win);
    if (active && EDITOR_IS_WIDGET(active)) {
        Document *doc = editor_widget_get_document(EDITOR_WIDGET(active));
        const char *path = doc ? document_get_file_path(doc) : NULL;
        if (path && *path) return g_path_get_dirname(path);
    }

    const char *home = g_get_home_dir();
    if (home && *home) return g_strdup(home);
    return g_get_current_dir();
}

static void
on_terminal_spawned(VteTerminal *term G_GNUC_UNUSED, GPid pid, GError *error, gpointer user_data G_GNUC_UNUSED)
{
    if (error) {
        g_warning("Failed to spawn embedded terminal: %s", error->message);
        g_error_free(error);
        return;
    }

    if (pid > 0) g_spawn_close_pid(pid);
}

static void
terminal_tracker_free(gpointer data)
{
    TerminalTracker *tracker = data;
    if (!tracker) return;
    g_weak_ref_clear(&tracker->terminal_ref);
    g_free(tracker);
}

static double
clamp01(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static double
terminal_darken_factor(gboolean dark_theme)
{
    return dark_theme ? 0.08 : 0.12;
}

static void
apply_terminal_theme_colors(VteTerminal *term)
{
    const ViteTheme *theme = theme_manager_get_current();
    if (!theme) return;

    GdkRGBA fg = theme->editor_fg;
    GdkRGBA bg = theme->editor_bg;

    /* Keep terminal slightly darker than the editor surface. */
    double darken = terminal_darken_factor(theme->is_dark);
    bg.red = clamp01(bg.red * (1.0 - darken));
    bg.green = clamp01(bg.green * (1.0 - darken));
    bg.blue = clamp01(bg.blue * (1.0 - darken));
    bg.alpha = 1.0;
    fg.alpha = 1.0;

    vte_terminal_set_colors(term, &fg, &bg, NULL, 0);
}

static void
sync_all_terminal_theme_colors(const ViteTheme *theme G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    if (!terminal_trackers) return;

    for (guint i = 0; i < terminal_trackers->len; ) {
        TerminalTracker *tracker = g_ptr_array_index(terminal_trackers, i);
        VteTerminal *term = tracker ? VTE_TERMINAL(g_weak_ref_get(&tracker->terminal_ref)) : NULL;
        if (!term) {
            g_ptr_array_remove_index(terminal_trackers, i);
            continue;
        }

        apply_terminal_theme_colors(term);
        g_object_unref(term);
        i++;
    }

    if (terminal_trackers->len == 0 && terminal_theme_changed_id != 0) {
        theme_manager_remove_changed_callback(terminal_theme_changed_id);
        terminal_theme_changed_id = 0;
    }
}

static void
on_tracked_terminal_destroy(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    TerminalTracker *target = user_data;
    if (!terminal_trackers || !target) return;

    for (guint i = 0; i < terminal_trackers->len; i++) {
        if (g_ptr_array_index(terminal_trackers, i) == target) {
            g_ptr_array_remove_index(terminal_trackers, i);
            break;
        }
    }

    if (terminal_trackers->len == 0 && terminal_theme_changed_id != 0) {
        theme_manager_remove_changed_callback(terminal_theme_changed_id);
        terminal_theme_changed_id = 0;
    }
}

static void
track_terminal(VteTerminal *term)
{
    if (!terminal_trackers) {
        terminal_trackers = g_ptr_array_new_with_free_func(terminal_tracker_free);
    }

    if (terminal_theme_changed_id == 0) {
        terminal_theme_changed_id = theme_manager_add_changed_callback(sync_all_terminal_theme_colors, NULL, NULL);
    }

    TerminalTracker *tracker = g_new0(TerminalTracker, 1);
    g_weak_ref_init(&tracker->terminal_ref, G_OBJECT(term));
    g_ptr_array_add(terminal_trackers, tracker);
    g_signal_connect(term, "destroy", G_CALLBACK(on_tracked_terminal_destroy), tracker);
}

static void
on_terminal_menu_copy_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkPopover *popover = GTK_POPOVER(user_data);
    VteTerminal *term = VTE_TERMINAL(g_object_get_data(G_OBJECT(popover), "terminal"));
    if (term) {
#if VTE_CHECK_VERSION(0, 50, 0)
        vte_terminal_copy_clipboard_format(term, VTE_FORMAT_TEXT);
#else
        vte_terminal_copy_clipboard(term);
#endif
    }
    gtk_popover_popdown(popover);
}

static void
on_terminal_menu_paste_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkPopover *popover = GTK_POPOVER(user_data);
    VteTerminal *term = VTE_TERMINAL(g_object_get_data(G_OBJECT(popover), "terminal"));
    if (term) {
        vte_terminal_paste_clipboard(term);
    }
    gtk_popover_popdown(popover);
}

static void
on_terminal_menu_close_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkPopover *popover = GTK_POPOVER(user_data);
    VteTerminal *term = VTE_TERMINAL(g_object_get_data(G_OBJECT(popover), "terminal"));
    if (!term) {
        gtk_popover_popdown(popover);
        return;
    }

    GtkWidget *view = GTK_WIDGET(term);
    while (view) {
        if (gtk_widget_has_css_class(view, "view-split") &&
            GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "is-terminal-view")) != 0) {
            close_split_view(view);
            break;
        }
        view = gtk_widget_get_parent(view);
    }

    gtk_popover_popdown(popover);
}

static void
on_terminal_menu_closed(GtkPopover *popover, gpointer user_data)
{
    VteTerminal *term = VTE_TERMINAL(user_data);
    if (term) {
        g_object_set_data(G_OBJECT(term), "context-menu-popover", NULL);
    }
    if (gtk_widget_get_parent(GTK_WIDGET(popover))) {
        gtk_widget_unparent(GTK_WIDGET(popover));
    }
}

static void
on_terminal_secondary_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
    if (n_press != 1) return;

    VteTerminal *term = VTE_TERMINAL(user_data);
    GtkWidget *old_pop = g_object_get_data(G_OBJECT(term), "context-menu-popover");
    if (old_pop) {
        gtk_popover_popdown(GTK_POPOVER(old_pop));
        if (gtk_widget_get_parent(old_pop)) {
            gtk_widget_unparent(old_pop);
        }
        g_object_set_data(G_OBJECT(term), "context-menu-popover", NULL);
    }

    GtkWidget *popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), TRUE);
    gtk_widget_set_parent(popover, GTK_WIDGET(term));

    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(box, 0);
    gtk_widget_set_margin_end(box, 0);
    gtk_widget_set_margin_top(box, 0);
    gtk_widget_set_margin_bottom(box, 0);

    GtkWidget *copy_btn = gtk_button_new_with_label(_("Copy"));
    GtkWidget *paste_btn = gtk_button_new_with_label(_("Paste"));
    GtkWidget *close_btn = gtk_button_new_with_label(_("Close"));
    gtk_widget_add_css_class(copy_btn, "menuitem");
    gtk_widget_add_css_class(paste_btn, "menuitem");
    gtk_widget_add_css_class(close_btn, "menuitem");
    gtk_widget_add_css_class(copy_btn, "flat");
    gtk_widget_add_css_class(paste_btn, "flat");
    gtk_widget_add_css_class(close_btn, "flat");
    gtk_widget_set_halign(copy_btn, GTK_ALIGN_FILL);
    gtk_widget_set_halign(paste_btn, GTK_ALIGN_FILL);
    gtk_widget_set_halign(close_btn, GTK_ALIGN_FILL);
    gtk_widget_set_sensitive(copy_btn, vte_terminal_get_has_selection(term));

    g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_terminal_menu_copy_clicked), popover);
    g_signal_connect(paste_btn, "clicked", G_CALLBACK(on_terminal_menu_paste_clicked), popover);
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_terminal_menu_close_clicked), popover);
    gtk_box_append(GTK_BOX(box), copy_btn);
    gtk_box_append(GTK_BOX(box), paste_btn);
    gtk_box_append(GTK_BOX(box), close_btn);

    g_object_set_data(G_OBJECT(popover), "terminal", term);
    gtk_widget_add_css_class(popover, "terminal-menu");
    g_object_set_data(G_OBJECT(term), "context-menu-popover", popover);
    g_signal_connect(popover, "closed", G_CALLBACK(on_terminal_menu_closed), term);

    gtk_popover_set_child(GTK_POPOVER(popover), box);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_popover_popup(GTK_POPOVER(popover));
}

static GtkWidget *
create_embedded_terminal(ViteWindow *win)
{
    VteTerminal *term = VTE_TERMINAL(vte_terminal_new());
    GtkWidget *terminal = GTK_WIDGET(term);
    gtk_widget_set_hexpand(terminal, TRUE);
    gtk_widget_set_vexpand(terminal, TRUE);
    gtk_widget_add_css_class(terminal, "monospace");

    vte_terminal_set_scrollback_lines(term, 10000);
    apply_terminal_theme_colors(term);
    track_terminal(term);

    GtkGestureClick *secondary = GTK_GESTURE_CLICK(gtk_gesture_click_new());
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary), GDK_BUTTON_SECONDARY);
    g_signal_connect(secondary, "pressed", G_CALLBACK(on_terminal_secondary_pressed), term);
    gtk_widget_add_controller(terminal, GTK_EVENT_CONTROLLER(secondary));

    const char *shell = g_getenv("SHELL");
    if (!shell || !*shell) shell = "/bin/sh";

    char *argv[] = { (char *)shell, (char *)"-i", NULL };
    char *cwd = terminal_split_cwd(win);

    vte_terminal_spawn_async(term,
                             VTE_PTY_DEFAULT,
                             cwd,
                             argv,
                             NULL,
                             G_SPAWN_SEARCH_PATH,
                             NULL,
                             NULL,
                             NULL,
                             -1,
                             NULL,
                             on_terminal_spawned,
                             NULL);
    g_free(cwd);

    return terminal;
}

static GtkWidget *
find_terminal_view_recursive(GtkWidget *widget)
{
    if (!widget) return NULL;

    if (gtk_widget_has_css_class(widget, "view-split") &&
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "is-terminal-view")) != 0) {
        return widget;
    }

    GtkWidget *child = gtk_widget_get_first_child(widget);
    while (child) {
        GtkWidget *res = find_terminal_view_recursive(child);
        if (res) return res;
        child = gtk_widget_get_next_sibling(child);
    }

    return NULL;
}

static void
do_terminal_split(ViteWindow *win, GtkOrientation orientation)
{
    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    if (!tab) return;

    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (!page) return;

    GtkWidget *view_container = resolve_split_target_view(win, tab, page, FALSE);
    if (!view_container) return;

    GtkWidget *paned = gtk_paned_new(orientation);
    if (!replace_view_with_paned(view_container, paned)) return;

    GtkWidget *terminal = create_embedded_terminal(win);
    
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), terminal);

    GtkWidget *terminal_view = create_view_container(win, scrolled);
    g_object_set_data(G_OBJECT(terminal_view), "is-terminal-view", GINT_TO_POINTER(1));
    gtk_paned_set_end_child(GTK_PANED(paned), terminal_view);

    int size = (orientation == GTK_ORIENTATION_HORIZONTAL) ? gtk_widget_get_width(view_container) : gtk_widget_get_height(view_container);
    if (size > 0) gtk_paned_set_position(GTK_PANED(paned), size / 2);

    defer_focus(terminal);
}
#endif

static void
on_split_right(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (win) do_split(win, GTK_ORIENTATION_HORIZONTAL);
}

static void
on_split_down(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (win) do_split(win, GTK_ORIENTATION_VERTICAL);
}

#ifdef HAVE_VTE
static void
on_terminal_toggle(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (!win || !win->tab_bar) return;

    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    if (!tab) return;

    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (!page) return;

    GtkWidget *terminal_view = find_terminal_view_recursive(page);
    if (terminal_view) {
        close_split_view(terminal_view);
        return;
    }

    do_terminal_split(win, GTK_ORIENTATION_VERTICAL);
}
#endif

static void
on_close_split_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    
    /* 1. Get Active Tab */
    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    if (!tab) return;
    
    /* 2. Get Focused Child via Tracks & Promote to ViewContainer */
    GtkWidget *target = vite_tab_get_last_focused_child(tab);
    
    if (target) {
        GtkWidget *vc = target;
        GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
        
        while (vc && !gtk_widget_has_css_class(vc, "view-split") && vc != page) {
            vc = gtk_widget_get_parent(vc);
        }
        
        if (vc && gtk_widget_has_css_class(vc, "view-split")) {
            close_split_view(vc);
        }
    }
}

static void
load_css(void)
{
    GString *css = g_string_new("");

    /* Part 1: Base styles and Findbar */
    g_string_append(css,
    "label {"
    "    min-width: 20px;"
    "}"
    "headerbar, headerbar.titlebar, .toolbar-view .top-bar {"
    "    border: none;"
    "    box-shadow: none;"
    "}"
    ".titlebar-box { background: @headerbar_bg_color; }"
    ".vite-tab-bar-container {"
    "    background: @headerbar_bg_color;"
    "}"
    "findbar { "
    "    background: @headerbar_bg_color;" 
    "    color: @headerbar_fg_color;"
    "    border-top: 1px solid alpha(@window_fg_color, 0.1);"
    "    padding: 6px;"
    "}"
    "findbar entry {"
    "    min-height: 28px;"
    "}"
    ".find-entry-wrapper {"
    "    background: alpha(@window_fg_color, 0.05);"
    "    border: 1px solid alpha(@window_fg_color, 0.1);"
    "    border-radius: 6px;"
    "    padding-left: 4px;"
    "    padding-right: 4px;"
    "}"
    ".find-entry-wrapper:focus-within {"
    "    border-color: @theme_selected_bg_color;"
    "    box-shadow: 0 0 0 1px @theme_selected_bg_color;"
    "}"
    ".transparent-entry, .transparent-entry:focus {"
    "    background: transparent;"
    "    border: none;"
    "    box-shadow: none;"
    "    outline: none;"
    "}"
    "findbar button.flat {"
    "    min-width: 28px;"
    "    min-height: 28px;"
    "    padding: 0;"
    "    border-radius: 6px;"
    "}"
    "findbar .dim-label {"
    "    color: alpha(@window_fg_color, 0.5);"
    "    font-size: 0.9em;"
    "    margin-left: 8px;"
    "    margin-right: 8px;"
    "}"
    "findbar .linked > button:first-child:dir(ltr) {"
    "    border-top-left-radius: 6px;"
    "    border-bottom-left-radius: 6px;"
    "}"
    "findbar .linked > button:last-child:dir(ltr) {"
    "    border-top-right-radius: 6px;"
    "    border-bottom-right-radius: 6px;"
    "}"
    "findbar .linked > button:first-child:dir(rtl) {"
    "    border-top-right-radius: 6px;"
    "    border-bottom-right-radius: 6px;"
    "}"
    "findbar .linked > button:last-child:dir(rtl) {"
    "    border-top-left-radius: 6px;"
    "    border-bottom-left-radius: 6px;"
    "}"
    );

    /* Part 2: Statusbar and Popovers */
    g_string_append(css,
    "statusbar {"
    "    background-color: alpha(@headerbar_bg_color, 1);"
    "    min-height: 1px;"
    "    padding-top: 1px;"
    "    padding-bottom: 2px;"
    "    padding-left: 4px;"
    "    padding-right: 4px;"    
    "    margin-left: 0px;"
    "    margin-right: 0px;"
    "}"
    "popover.editor-context-menu > contents {"
    "    min-width: 220px;"
    "    min-height: 295px;"
    "    padding: 0px;"
    "}"
    "popover.terminal-menu > contents {"
    "    padding: 4px;"
    "}"
    "popover.terminal-menu button {"
    "    font-weight: normal;"
    "    padding: 2px 12px;"
    "    min-height: 24px;"
    "}"
    "statusbar label {"
    "    opacity: 0.8;"
    "}"
    "statusbar button.flat {"
    "    min-height: 0;"
    "    padding-left: 10px;"
    "    padding-right: 10px;"
    "    padding-top: 4px;"
    "    padding-bottom: 4px;"
    "    margin-top: 1px;"
    "    margin-bottom: 2px;"
    "}"
    "statusbar menubutton > button {"
    "    min-height: 0;"
    "    padding-left: 10px;"
    "    padding-right: 10px;"
    "    padding-top: 4px;"
    "    padding-bottom: 4px;"
    "    margin-top: 1px;"
    "    margin-bottom: 2px;"
    "}"
    );

    /* Part 3: Headerbar elements and split buttons */
    g_string_append(css,
    ".titlebar-box {"
    "    background: @headerbar_bg_color;"
    "    color: @headerbar_fg_color;"
    "    padding-bottom: 0px;"
    "    min-height: 0px;"
    "}"
    ".open-split-btn {"
    "    background: alpha(@window_fg_color, 0.05);"
    "    border-radius: 10px;"
    "}"
    ".open-split-btn separator {"
    "    background: transparent;"
    "    min-width: 0;"
    "    border: none;"
    "    margin: 0;"
    "    opacity: 0;"
    "}"
    ".open-split-btn button:dir(ltr) {"
    "    border: none;"
    "    box-shadow: none;"
    "    outline: none;"
    "    background: transparent;"
    "    border-top-left-radius: 10px;"
    "    border-bottom-left-radius: 10px;"
    "    border-top-right-radius: 0px;"
    "    border-bottom-right-radius: 0px;"
    "    margin: 0px;"
    "    padding-top: 4px;"
    "    padding-bottom: 4px;"
    "    padding-left: 10px;"
    "    padding-right: 6px;"
    "}"
    ".open-split-btn button:dir(rtl) {"
    "    border: none;"
    "    box-shadow: none;"
    "    outline: none;"
    "    background: transparent;"
    "    border-top-right-radius: 10px;"
    "    border-bottom-right-radius: 10px;"
    "    border-top-left-radius: 0px;"
    "    border-bottom-left-radius: 0px;"
    "    margin: 0px;"
    "    padding-top: 4px;"
    "    padding-bottom: 4px;"
    "    padding-right: 10px;"
    "    padding-left: 6px;"
    "}"
    ".open-split-btn button:hover:dir(ltr) {"
    "    background: alpha(@window_fg_color, 0.12);"
    "    border-top-left-radius: 10px;"
    "    border-bottom-left-radius: 10px;"
    "    border-top-right-radius: 0px;"
    "    border-bottom-right-radius: 0px;"
    "}"
    ".open-split-btn button:hover:dir(rtl) {"
    "    background: alpha(@window_fg_color, 0.12);"
    "    border-top-right-radius: 10px;"
    "    border-bottom-right-radius: 10px;"
    "    border-top-left-radius: 0px;"
    "    border-bottom-left-radius: 0px;"
    "}"
    ".open-split-btn menubutton:last-child > button:dir(ltr) {"
    "    border-top-left-radius: 0px; "
    "    border-bottom-left-radius: 0px;"
    "    border-top-right-radius: 10px; "
    "    border-bottom-right-radius: 10px;"
    "    padding-left: 4px;"
    "    padding-right: 6px;"
    "}"
    ".open-split-btn menubutton:last-child > button:dir(rtl) {"
    "    border-top-right-radius: 0px; "
    "    border-bottom-right-radius: 0px;"
    "    border-top-left-radius: 10px; "
    "    border-bottom-left-radius: 10px;"
    "    padding-right: 4px;"
    "    padding-left: 6px;"
    "}"
    ".open-split-btn menubutton button:active,"
    ".open-split-btn menubutton button:checked {"
    "    background-color: alpha(@window_fg_color, 0.12);"
    "}"
    ".titlebar-box headerbar {"
    "    background: none;"
    "    border: none;"
    "    box-shadow: none;"
    "    margin-bottom: -4px;"
    "    padding-top: 0px;"
    "    padding-bottom: 0px;"
    "    min-height: 0px;"
    "}"
    );

    /* Part 4: Recent list, Scrollbars and Header Buttons (inc. Save) */
    g_string_append(css,
    ".recent-list {"
    "    background: transparent;"
    "    margin-right: -10px;"
    "}"
    ".recent-list row {"
    "    border-radius: 10px;"
    "    margin-right: 16px;"
    "}"
    ".recent-list .open-tabs-title, .recent-list .file-list-title {"
    "    color: @window_fg_color;"
    "}"
    ".recent-list .remove-btn {"
    "    opacity: 0;"
    "    transition: opacity 0.1s;"   
    "    margin-right: 0px;"
    "    padding: 4px;"
    "}"
    ".recent-list row:hover .remove-btn {"
    "    opacity: 1;"
    "    border-radius: 10px;"
    "    margin-right: 0px;"
    "    padding: 4px;"
    "}"
    ".header-progress progress, .header-progress trough {"
    "    min-height: 2px;"
    "}"
    ".header-progress {"
    "    padding: 0;"
    "    margin: 0;"
    "}"
    "scrollbar slider:hover, "
    "scrollbar trough:hover > slider, "
    "scrollbar:hover slider {"
    "    background-color: rgb(73, 152, 248);"
    "}"
    "scrollbar slider:active, "
    "scrollbar trough > slider:active, "
    "scrollbar:active slider {"
    "    background-color: rgb(53, 132, 228);"
    "}"
    ".header-button {"
    "    border: none;"
    "    box-shadow: none;"
    "    outline: none;"
    "    background: alpha(@window_fg_color, 0.05);"
    "    border-radius: 10px;"
    "    margin: 0px;"
    "    padding-top: 4px;"
    "    padding-bottom: 4px;"
    "    padding-left: 14px;"
    "    padding-right: 14px;"
    "}"
    ".header-button:hover {"
    "    background: alpha(@window_fg_color, 0.12);"
    "}"
    ".header-button:active {"
    "    background: alpha(@window_fg_color, 0.18);"
    "}"
    ".vite-preferences-dialog row label, "
    ".vite-preferences-dialog preferencesgroup > label {"
    "    color: @window_fg_color;"
    "}"
    ".vite-preferences-dialog row .subtitle, "
    ".vite-preferences-dialog row label.dim-label {"
    "    color: alpha(@window_fg_color, 0.8);"
    "}"
    ".vite-about-dialog label, "
    ".vite-about-dialog row label {"
    "    color: @window_fg_color;"
    "}"
    ".vite-about-dialog .dim-label, "
    ".vite-about-dialog row .subtitle, "
    ".vite-about-dialog .caption {"
    "    color: alpha(@window_fg_color, 0.8);"
    "}"
    "preferenceswindow row .title, "
    "preferencesdialog row .title {"
    "    color: @window_fg_color;"
    "}"
    "preferenceswindow row .subtitle, "
    "preferencesdialog row .subtitle {"
    "    color: alpha(@window_fg_color, 0.8);"
    "}"
    "list row:hover, "
    "listview row:hover, "
    ".navigation-sidebar row:hover {"
    "    background-color: alpha(@window_fg_color, 0.08);"
    "}"
    "/* Custom dropdown popover stable sizing */"
    ".vite-theme-combo popover contents {"
    "    min-width: 240px;"
    "    min-height: 200px;"
    "}"
    );

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css->str);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    g_string_free(css, TRUE);
}

static void
on_close_modified_tab_response(const char *response, SaveChangesDialogData *dialog, gpointer user_data)
{
    ViteTab *tab = VITE_TAB(user_data);
    if (!tab || !VITE_IS_TAB(tab)) return;

    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(tab));
    if (!root) return;

    ViteWindow *win = g_object_get_data(G_OBJECT(root), "vite-window");
    if (!win) return;

    if (g_strcmp0(response, "discard") == 0) {
        close_tab_now(win, tab);
        return;
    }

    if (g_strcmp0(response, "save") == 0) {
        Document *doc = get_tab_document(tab);
        if (!doc) return;

        if (!save_changes_dialog_tab_selected(dialog, tab)) {
            close_tab_now(win, tab);
            return;
        }

        char *filename = save_changes_dialog_filename_for_tab(dialog, tab);
        const char *current_path = document_get_file_path(doc);
        char *final_path = NULL;

        if (current_path && *current_path) {
            char *current_basename = g_path_get_basename(current_path);
            if (g_strcmp0(filename, current_basename) != 0) {
                /* Filename changed - Save As in same directory */
                char *dir = g_path_get_dirname(current_path);
                final_path = g_build_filename(dir, filename, NULL);
                g_free(dir);
            } else {
                /* Save normally */
                final_path = g_strdup(current_path);
            }
            g_free(current_basename);
        } else {
            /* Untitled file - Save to default directory */
            char *dir = default_save_directory();
            final_path = g_build_filename(dir, filename, NULL);
            g_free(dir);
        }

        vite_tab_set_close_when_done(tab, TRUE);
        save_async_with_progress(win, tab, doc, final_path);
        
        g_free(final_path);
        g_free(filename);
    }
}

static void
close_tab_now(ViteWindow *win, ViteTab *tab)
{
    if (!win || !tab || !VITE_IS_TAB(tab)) return;

    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    GtkWidget *parent = NULL;

    Document *doc_to_free = NULL;
    if (page && GTK_IS_WIDGET(page)) {
        GtkWidget *editor = get_editor_from_page(page);
        if (EDITOR_IS_WIDGET(editor)) {
            doc_to_free = editor_widget_get_document(EDITOR_WIDGET(editor));
        }
        g_object_ref(page);
        parent = gtk_widget_get_parent(page);
    } else {
        page = NULL;
    }

    if (doc_to_free) {
        const char *path = document_get_file_path(doc_to_free);
        if (path) remember_recently_closed_file(path);
    }

    if (vite_tab_bar_get_n_tabs(win->tab_bar) == 1) {
        reset_tab_to_empty(win, tab);
        if (page) g_object_unref(page);
        return;
    }

    g_object_set_data(G_OBJECT(tab), "page", NULL);
    vite_tab_set_close_when_done(tab, FALSE);

    vite_tab_bar_remove_tab(win->tab_bar, tab);

    if (page && parent) {
        if (GTK_IS_STACK(parent)) {
            stack_safe_remove_child(GTK_STACK(parent), page);
        } else {
            gtk_widget_unparent(page);
        }
    }

    if (page) g_object_unref(page);
    if (doc_to_free) document_free(doc_to_free);

    if (vite_tab_bar_get_n_tabs(win->tab_bar) == 0) {
        Document *doc = document_new(NULL);
        create_new_tab(win, "Untitled", doc);
        document_free(doc);
    }
}

static void
on_tab_close_clicked (ViteTab *tab, gpointer user_data G_GNUC_UNUSED)
{
    if (!tab || !VITE_IS_TAB(tab)) return;
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(tab));
    if (!root) return;

    ViteWindow *win = g_object_get_data(G_OBJECT(root), "vite-window");
    if (!win) return;

    if (tab_has_unsaved_changes(tab)) {
        GPtrArray *single = g_ptr_array_new();
        g_ptr_array_add(single, tab);
        show_save_changes_dialog(win, single, on_close_modified_tab_response, tab);
        g_ptr_array_unref(single);
        return;
    }

    close_tab_now(win, tab);
}

static void
on_new_tab_clicked_header (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    /* user_data was window in signal connect? */
    /* setup_window: g_signal_connect(btn_new, "clicked", G_CALLBACK(on_new_tab_clicked_header), window); */
    GtkWindow *window = GTK_WINDOW(user_data);
    ViteWindow *win = g_object_get_data(G_OBJECT(window), "vite-window");
    
    if (win) {
        Document *doc = document_new(NULL);
        create_new_tab(win, "Untitled", doc);
        document_free(doc);
    }
}


#if 0
static void
on_action_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    GtkWindow *win = GTK_WINDOW(user_data);
    int idx = gtk_list_box_row_get_index(row);
    
    if (idx == 0) {
        on_open_btn_clicked(NULL, win);
    } else if (idx == 1) {
        on_new_tab_clicked_header(NULL, win);
    }
    
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(list), GTK_TYPE_POPOVER);
    if (popover) {
        gtk_popover_popdown(GTK_POPOVER(popover));
    }
}
#endif

static void
on_recent_item_activated(GtkListBox *list_box, GtkListBoxRow *row, gpointer user_data)
{
    GtkApplication *app = GTK_APPLICATION(user_data);
    
    ViteWindow *target_win = NULL;
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(list_box));
    if (root) {
        target_win = g_object_get_data(G_OBJECT(root), "vite-window");
    }
    
    const char *uri = g_object_get_data(G_OBJECT(row), "uri");
    if (uri) {
        GFile *file = g_file_new_for_uri(uri);
        queue_open_file(app, target_win, file, TRUE);
        g_object_unref(file);
    }
    
    /* Close the popover */
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(list_box), GTK_TYPE_POPOVER);
    if (popover) {
        gtk_popover_popdown(GTK_POPOVER(popover));
    }
}

static char*
get_local_recents_path(void)
{
    const char *config_dir = g_get_user_config_dir();
    char *vite_dir = g_build_filename(config_dir, "vite", NULL);
    g_mkdir_with_parents(vite_dir, 0755);
    char *path = g_build_filename(vite_dir, "recent_files.txt", NULL);
    g_free(vite_dir);
    return path;
}

static GList*
load_local_recents(void)
{
    char *path = get_local_recents_path();
    GList *uris = NULL;
    char *content = NULL;
    gsize length;

    if (g_file_get_contents(path, &content, &length, NULL)) {
        char **lines = g_strsplit(content, "\n", -1);
        for (int i = 0; lines[i] != NULL; i++) {
            if (strlen(lines[i]) > 0) {
                uris = g_list_append(uris, g_strdup(lines[i]));
            }
        }
        g_strfreev(lines);
        g_free(content);
    }
    g_free(path);
    return uris;
}

static void
save_local_recents(GList *uris)
{
    char *path = get_local_recents_path();
    GString *content = g_string_new("");
    for (GList *l = uris; l != NULL; l = l->next) {
        g_string_append_printf(content, "%s\n", (char *)l->data);
    }
    g_file_set_contents(path, content->str, content->len, NULL);
    g_string_free(content, TRUE);
    g_free(path);
}

static void
add_to_local_recents(const char *uri)
{
    GList *uris = load_local_recents();
    
    /* Remove if already present */
    GList *link = g_list_find_custom(uris, uri, (GCompareFunc)g_strcmp0);
    if (link) {
        g_free(link->data);
        uris = g_list_delete_link(uris, link);
    }
    
    /* Prepend to top */
    uris = g_list_prepend(uris, g_strdup(uri));
    
    /* Limit to MAX_RECENT_FILES */
    while (g_list_length(uris) > MAX_RECENT_FILES) {
        GList *last = g_list_last(uris);
        g_free(last->data);
        uris = g_list_delete_link(uris, last);
    }
    
    save_local_recents(uris);
    g_list_free_full(uris, g_free);
}

typedef struct {
    GtkWidget *btn;
    gboolean overflowing;
} OverflowUpdate;

static void
update_overflow_idle (gpointer data)
{
    OverflowUpdate *u = data;
    if (GTK_IS_WIDGET(u->btn)) {
        gtk_widget_set_visible(u->btn, u->overflowing);
    }
    g_object_unref(u->btn);
    g_free(u);
}

static void
on_overflow_changed (ViteTabBar *bar G_GNUC_UNUSED, gboolean overflowing, gpointer user_data)
{
    GtkWidget *btn = GTK_WIDGET(user_data);
    
    OverflowUpdate *u = g_new(OverflowUpdate, 1);
    u->btn = g_object_ref(btn);
    u->overflowing = overflowing;
    g_idle_add_once(update_overflow_idle, u);
}

static void
on_popover_tab_row_activated (GtkListBox *list, GtkListBoxRow *row, gpointer user_data G_GNUC_UNUSED)
{
    ViteTab *tab = g_object_get_data(G_OBJECT(row), "tab");
    if (tab) {
        g_signal_emit_by_name(tab, "clicked");

        GtkRoot *tab_root = gtk_widget_get_root(GTK_WIDGET(tab));
        if (tab_root && GTK_IS_WINDOW(tab_root)) {
            gtk_window_present(GTK_WINDOW(tab_root));
        }
        
        GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(list), GTK_TYPE_POPOVER);
        if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
    }
}

static void
on_open_tab_close_btn_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkListBoxRow *row = GTK_LIST_BOX_ROW(user_data);
    if (!row) return;

    ViteTab *tab = g_object_get_data(G_OBJECT(row), "tab");
    if (!tab) return;

    on_tab_close_clicked(tab, NULL);

    GtkWidget *list = gtk_widget_get_parent(GTK_WIDGET(row));
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(row), GTK_TYPE_POPOVER);
    if (popover) {
        update_open_tabs_list(popover, list);
    }
}

static void
append_open_tabs_splitter(GtkListBox *list)
{
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_start(sep, 8);
    gtk_widget_set_margin_end(sep, 24);
    gtk_widget_set_margin_top(sep, 4);
    gtk_widget_set_margin_bottom(sep, 4);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), sep);
    gtk_list_box_append(list, row);
}

static void
append_open_tab_row(GtkListBox *list, ViteTab *tab)
{
    const char *title = vite_tab_get_title(tab);
    const char *display_name = (title && *title) ? title : _("Untitled");

    GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(row_box, 6);
    gtk_widget_set_margin_end(row_box, 0);
    gtk_widget_set_margin_top(row_box, 6);
    gtk_widget_set_margin_bottom(row_box, 6);

    GtkWidget *label = gtk_label_new(display_name);
    gtk_widget_add_css_class(label, "open-tabs-title");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 260);
    gtk_box_append(GTK_BOX(row_box), label);

    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(row_box), spacer);

    GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close_btn, "flat");
    gtk_widget_add_css_class(close_btn, "remove-btn");
    gtk_widget_set_valign(close_btn, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(row_box), close_btn);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), row_box);
    gtk_list_box_append(list, row);

    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_open_tab_close_btn_clicked), row);
    g_object_set_data_full(G_OBJECT(row), "tab", g_object_ref(tab), g_object_unref);
    g_object_set_data_full(G_OBJECT(row), "display-name", g_strdup(display_name), g_free);
}

static int
append_window_open_tabs(GtkListBox *list, ViteWindow *win)
{
    if (!win || !win->tab_bar) return 0;

    int count = 0;
    GList *tabs = vite_tab_bar_get_tabs(win->tab_bar);
    for (GList *l = tabs; l != NULL; l = l->next) {
        append_open_tab_row(list, VITE_TAB(l->data));
        count++;
    }
    g_list_free(tabs);

    return count;
}

static void
update_open_tabs_list (GtkWidget *popover, gpointer user_data G_GNUC_UNUSED)
{
    GtkListBox *list = GTK_LIST_BOX(user_data);
    if (!list) list = g_object_get_data(G_OBJECT(popover), "list");
    if (!list) return;

    /* Clear */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list)))) {
        if (GTK_IS_LIST_BOX_ROW(child)) {
            gtk_list_box_remove(list, child);
        } else {
            gtk_widget_unparent(child);
        }
    }
    
    ViteWindow *win = NULL;
    /* Popover parent is set to list widget, which is inside popover? 
       Wait, `gtk_widget_set_parent(popover, GTK_WIDGET(list))` was for context menu. 
       This function `update_open_tabs_list` is for... what?
       Ah, I don't see this function being used for header bar?
       Is it for "Open Tabs" list?
       I don't recall implementing "Open Tabs" list in my plan. 
       Maybe it's pre-existing?
       Let's check usage.
       It's likely dead code or from previous context menu implementation for "tabs list"?
       Or I missed it.
       If it uses `main_tab_bar`, it's for the window.
       
       Let's try to get window from popover's parent.
    */
    /* If popover is attached to something in the window */
    /* Let's assume we can get it via root */
    if (GTK_IS_WIDGET(popover)) {
         GtkRoot *r = gtk_widget_get_root(popover);
         win = g_object_get_data(G_OBJECT(r), "vite-window");
    }
    
    if (!win || !win->tab_bar) return;

    int current_window_tabs = append_window_open_tabs(list, win);

    GtkApplication *app = NULL;
    if (win->window) {
        app = gtk_window_get_application(GTK_WINDOW(win->window));
    }

    if (app) {
        gboolean needs_splitter = (current_window_tabs > 0);
        GList *windows = gtk_application_get_windows(app);
        for (GList *w = windows; w != NULL; w = w->next) {
            ViteWindow *other_win = g_object_get_data(G_OBJECT(w->data), "vite-window");
            if (!other_win || other_win == win || !other_win->tab_bar) continue;
            if (vite_tab_bar_get_n_tabs(other_win->tab_bar) == 0) continue;

            if (needs_splitter) {
                append_open_tabs_splitter(list);
            }

            append_window_open_tabs(list, other_win);
            needs_splitter = TRUE;
        }
    }

    GtkWidget *search_entry = g_object_get_data(G_OBJECT(popover), "search-entry");
    if (GTK_IS_SEARCH_ENTRY(search_entry)) {
        on_search_changed(GTK_SEARCH_ENTRY(search_entry), list);
    }
}

static void
update_recent_files_list(GtkListBox *list_box, GtkApplication *app G_GNUC_UNUSED, GtkPopover *popover G_GNUC_UNUSED)
{
    /* Clear current list */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list_box)))) {
        if (GTK_IS_LIST_BOX_ROW(child)) {
            gtk_list_box_remove(list_box, child);
        } else {
            gtk_widget_unparent(child);
        }
    }
    
    GList *uris = load_local_recents();
    int count = 0;
    
    for (GList *l = uris; l != NULL && count < MAX_RECENT_FILES; l = l->next) {
        const char *uri = l->data;
        GFile *file = g_file_new_for_uri(uri);
        char *path = g_file_get_path(file);
        if (!path) {
            g_object_unref(file);
            continue;
        }

        char *display_name = g_file_get_basename(file);
        
        /* Prettify path for subtitle */
        char *subtitle = NULL;
        char *dir = g_path_get_dirname(path);
        const char *home = g_get_home_dir();
        if (g_str_has_prefix(dir, home)) {
            subtitle = g_strconcat("~", dir + strlen(home), NULL);
        } else {
            subtitle = g_strdup(dir);
        }
        g_free(dir);

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start(row, 6);
        gtk_widget_set_margin_end(row, 0);
        gtk_widget_set_margin_top(row, 6);
        gtk_widget_set_margin_bottom(row, 6);
        
        /* Try to get icon for file - REMOVED */
        
        /* Text container (Title + Subtitle) */
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), vbox);

        GtkWidget *title_label = gtk_label_new(display_name);
        gtk_widget_add_css_class(title_label, "file-list-title");
        gtk_widget_set_halign(title_label, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(title_label), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(title_label), 260);
        gtk_box_append(GTK_BOX(vbox), title_label);

        if (subtitle) {
            GtkWidget *subtitle_label = gtk_label_new(subtitle);
            gtk_widget_set_halign(subtitle_label, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(subtitle_label), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(subtitle_label), 60);
            gtk_widget_add_css_class(subtitle_label, "dim-label");
            gtk_widget_add_css_class(subtitle_label, "caption");
            gtk_box_append(GTK_BOX(vbox), subtitle_label);
        }
        
        /* Spacer to push close button to the end */
        GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand(spacer, TRUE);
        gtk_box_append(GTK_BOX(row), spacer);

        /* Close Button */
        GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_widget_add_css_class(close_btn, "flat");
        gtk_widget_add_css_class(close_btn, "remove-btn");
        gtk_widget_set_valign(close_btn, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), close_btn);

        gtk_list_box_append(list_box, row);
        
        /* Store URI in row and metadata for filtering */
        GtkListBoxRow *list_row = gtk_list_box_get_row_at_index(list_box, count);
        g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_recent_btn_clicked), list_row);
        g_object_set_data_full(G_OBJECT(list_row), "uri", g_strdup(uri), g_free);
        g_object_set_data_full(G_OBJECT(list_row), "display-name", g_strdup(display_name), g_free);
        if (subtitle) g_object_set_data_full(G_OBJECT(list_row), "subtitle", subtitle, g_free); /* subtitle already strdup/concat'ed */

        g_free(display_name);
        g_free(path);
        g_object_unref(file);
        count++;
    }
    
    gtk_list_box_unselect_all(list_box);
    g_list_free_full(uris, g_free);
}

static gboolean
filter_recent_items(GtkListBoxRow *row, gpointer user_data)
{
    const char *search_text = (const char *)user_data;
    if (!search_text || strlen(search_text) == 0) return TRUE;
    
    const char *name = g_object_get_data(G_OBJECT(row), "display-name");
    const char *subtitle = g_object_get_data(G_OBJECT(row), "subtitle");
    
    char *name_lower = g_utf8_casefold(name ? name : "", -1);
    char *search_lower = g_utf8_casefold(search_text, -1);
    char *sub_lower = subtitle ? g_utf8_casefold(subtitle, -1) : NULL;
    
    gboolean match = FALSE;
    if (name_lower && g_strrstr(name_lower, search_lower) != NULL) match = TRUE;
    if (!match && sub_lower && g_strrstr(sub_lower, search_lower) != NULL) match = TRUE;
    
    g_free(name_lower);
    g_free(search_lower);
    if (sub_lower) g_free(sub_lower);
    
    return match;
}

static void
on_search_changed(GtkSearchEntry *entry, gpointer user_data)
{
    GtkListBox *list = GTK_LIST_BOX(user_data);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
    gtk_list_box_set_filter_func(list, filter_recent_items, (gpointer)text, NULL);
    gtk_list_box_unselect_all(list);
}

static void
remove_recent_item(GtkListBoxRow *row)
{
    const char *uri = g_object_get_data(G_OBJECT(row), "uri");
    if (uri) {
        GList *uris = load_local_recents();
        GList *link = g_list_find_custom(uris, uri, (GCompareFunc)g_strcmp0);
        if (link) {
            g_free(link->data);
            uris = g_list_delete_link(uris, link);
            save_local_recents(uris);
        }
        g_list_free_full(uris, g_free);
        
        GtkListBox *list = GTK_LIST_BOX(gtk_widget_get_parent(GTK_WIDGET(row)));
        gtk_list_box_remove(list, GTK_WIDGET(row));
    }
}

static void
on_remove_recent_clicked(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    remove_recent_item(GTK_LIST_BOX_ROW(user_data));
}

static void
on_close_recent_btn_clicked(GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    GtkListBoxRow *row = GTK_LIST_BOX_ROW(user_data);
    remove_recent_item(row);
}

static void
on_clear_all_confirmed(GObject *source_object, GAsyncResult *res, gpointer user_data G_GNUC_UNUSED)
{
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(source_object);
    const char *response = adw_alert_dialog_choose_finish(dialog, res);
    
    if (g_strcmp0(response, "clear") == 0) {
        char *path = get_local_recents_path();
        g_unlink(path);
        g_free(path);
        
        GtkListBox *list = GTK_LIST_BOX(user_data);
        GtkWidget *child;
        while ((child = gtk_widget_get_first_child(GTK_WIDGET(list)))) {
            if (GTK_IS_LIST_BOX_ROW(child)) {
                gtk_list_box_remove(list, child);
            } else {
                gtk_widget_unparent(child);
            }
        }
    }
}

static void
on_clear_all_clicked(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    GtkListBox *list = GTK_LIST_BOX(user_data);
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(list));
    
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Clear Recent Files?", 
        "This will remove all items from your recent files list. This action cannot be undone."));
    
    adw_alert_dialog_add_response(dialog, "cancel", "Cancel");
    adw_alert_dialog_add_response(dialog, "clear", "Clear All");
    adw_alert_dialog_set_response_appearance(dialog, "clear", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(dialog, "cancel");
    adw_alert_dialog_set_close_response(dialog, "cancel");
    
    adw_alert_dialog_choose(dialog, GTK_WIDGET(root), NULL, (GAsyncReadyCallback)on_clear_all_confirmed, list);
}

static void
on_recent_popover_unmap(GtkWidget *popover G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    GtkEditable *search_entry = GTK_EDITABLE(user_data);
    gtk_editable_set_text(search_entry, "");
}

static gboolean
on_search_key_pressed(GtkEventControllerKey *controller G_GNUC_UNUSED, guint keyval, guint keycode G_GNUC_UNUSED, GdkModifierType state G_GNUC_UNUSED, gpointer user_data)
{
    GtkPopover *popover = GTK_POPOVER(user_data);
    if (keyval == GDK_KEY_Escape) {
        gtk_popover_popdown(popover);
        return TRUE;
    }
    return FALSE;
}

static void
on_recent_context_menu(GtkGestureClick *gesture G_GNUC_UNUSED, int n_press G_GNUC_UNUSED, double x, double y, gpointer user_data)
{
    GtkListBox *list = GTK_LIST_BOX(user_data);
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(list, y);
    
    GMenu *menu = g_menu_new();
    GSimpleActionGroup *group = g_simple_action_group_new();
    
    if (row) {
        g_menu_append(menu, _("Remove from Recents"), "context.remove");
        GSimpleAction *act_remove = g_simple_action_new("remove", NULL);
        g_signal_connect(act_remove, "activate", G_CALLBACK(on_remove_recent_clicked), row);
        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act_remove));
    }
    
    g_menu_append(menu, _("Clear All Recents"), "context.clear-all");
    GSimpleAction *act_clear = g_simple_action_new("clear-all", NULL);
    g_signal_connect(act_clear, "activate", G_CALLBACK(on_clear_all_clicked), list);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act_clear));
    
    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_widget_set_parent(popover, GTK_WIDGET(list));
    
    /* Calculate precise point for popover */
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    
    gtk_widget_insert_action_group(popover, "context", G_ACTION_GROUP(group));
    gtk_popover_popup(GTK_POPOVER(popover));
    
    g_object_unref(menu);
    g_object_unref(group);
}

static void
on_new_window_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(win->window));
    activate(app, NULL);
}

static void
on_new_tab_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    Document *doc = document_new(NULL);
    create_new_tab(win, "Untitled", doc);
    document_free(doc);
}
static void
on_tab_move_to_new_window (ViteTab *tab, gpointer user_data G_GNUC_UNUSED)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(tab));
    ViteWindow *current_win = g_object_get_data(G_OBJECT(root), "vite-window");
    if (!current_win) return;
    
    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (!page) return;
    
    /* 1. Create new window */
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(current_win->window));
    AdwApplicationWindow *w = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    ViteWindow *new_win = setup_window(w);
    gtk_window_set_default_size(GTK_WINDOW(w), 800, 600);
    
    /* 2. Move tab */
    move_tab_to_window(new_win, tab, -1);
}



static gboolean
close_window_idle(gpointer user_data)
{
    GtkWindow *window = GTK_WINDOW(user_data);
    if (window && GTK_IS_WINDOW(window)) {
         gtk_window_close(window);
    }
    g_object_unref(window);
    return G_SOURCE_REMOVE;
}

static void
move_tab_to_window(ViteWindow *target_win, ViteTab *tab, int position)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(tab));
    if (!root) return; /* Already Detached? */
    
    ViteWindow *source_win = g_object_get_data(G_OBJECT(root), "vite-window");
    if (!source_win) return;
    
    if (source_win == target_win) return; 

    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (!page) return;
    
    /* Hold references during transfer */
    g_object_ref(tab);
    g_object_ref(page);
    
    /* Remove from source window using safe helper */
    vite_tab_bar_remove_tab(source_win->tab_bar, tab);
    if (source_win->stack && GTK_IS_STACK(source_win->stack)) {
        stack_safe_remove_child(source_win->stack, page);
    }
    
    /* Add to target window using safe helper */
    char id[64];
    snprintf(id, sizeof(id), "page_%p", (void *)page);
    if (target_win->stack && GTK_IS_STACK(target_win->stack)) {
        stack_safe_add_child(target_win->stack, page, id);
    }
    
    if (position < 0) position = vite_tab_bar_get_n_tabs(target_win->tab_bar);
    vite_tab_bar_insert_tab(target_win->tab_bar, tab, position);
    
    /* Activate */
    vite_tab_bar_set_active_tab(target_win->tab_bar, tab);
    
    /* Update window association on the Page (Overlay) just in case */
    g_object_set_data(G_OBJECT(page), "vite-window", target_win);
    
    update_window_title_for_tab(tab);
    gtk_window_present(GTK_WINDOW(target_win->window));

    g_object_unref(tab);
    g_object_unref(page);
    
    /* Check if source window empty */
    if (vite_tab_bar_get_n_tabs(source_win->tab_bar) == 0) {
        /* Close source window if empty - ASYNCHRONOUSLY to avoid Wayland/Popup crash */
        if (source_win->window && GTK_IS_WINDOW(source_win->window)) {
            g_object_ref(source_win->window);
            g_idle_add(close_window_idle, source_win->window);
        }
    }
}

static void
on_tab_dropped(ViteTabBar *tab_bar G_GNUC_UNUSED, ViteTab *tab, int position, gpointer user_data)
{
    ViteWindow *target_win = (ViteWindow *)user_data;
    move_tab_to_window(target_win, tab, position);
}

static gboolean
on_window_drop(GtkDropTarget *target G_GNUC_UNUSED, const GValue *value, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED, ViteWindow *win)
{

    if (value && G_VALUE_HOLDS(value, VITE_TYPE_TAB)) {
        ViteTab *tab = VITE_TAB(g_value_get_object(value));
        /* Move to end of tab list */
        move_tab_to_window(win, tab, -1);
        /* Signal successful drop to tab bar? The tab bar monitors itself via drag-end/failed?
           Actually, vite_tab_bar_clear_dragging_tab is manual.
           We might need to ensure the source tab bar knows it's done?
           move_tab_to_window removes it from source, so source updates. */
        return TRUE;
    } else if (value && G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
        GtkApplication *app = gtk_window_get_application(GTK_WINDOW(win->window));
        for (GSList *l = files; l != NULL; l = l->next) {
            GFile *file = G_FILE(l->data);
            queue_open_file(app, win, file, TRUE);
        }
        return TRUE;
    }
    return FALSE;
}

static void
on_cursor_moved(EditorWidget *editor, guint line, guint col, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    if (win->status_bar && GTK_WIDGET(editor) == get_active_editor(win)) {
        vite_status_bar_set_cursor_position(VITE_STATUS_BAR(win->status_bar), line, col);
    }
}

static void
on_insert_mode_changed(EditorWidget *editor, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    if (win->status_bar && GTK_WIDGET(editor) == get_active_editor(win)) {
        vite_status_bar_set_insert_mode(VITE_STATUS_BAR(win->status_bar), editor_widget_get_insert_mode(editor));
    }
}

static void
on_ignore_external_change(GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    GtkWidget *revealer = g_object_get_data(G_OBJECT(btn), "revealer");
    if (revealer && GTK_IS_REVEALER(revealer)) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
    }
}

static void
on_reload_external_change(GtkButton *btn, gpointer user_data)
{
    GtkWidget *editor = GTK_WIDGET(user_data);
    GtkRoot *root;
    ViteWindow *win;
    ViteTab *tab;
    
    root = gtk_widget_get_root(GTK_WIDGET(btn));
    if (!root) return;

    win = g_object_get_data(G_OBJECT(root), "vite-window");
    if (!win) return;

    tab = find_tab_for_widget(editor);
    if (!tab && win->tab_bar) {
        tab = vite_tab_bar_get_active_tab(win->tab_bar);
    }
    if (!tab) return;

    reload_tab_from_disk(win, tab);
}

static void
on_document_changed_externally(Document *doc G_GNUC_UNUSED, void *user_data)
{
    GtkWidget *page_root = GTK_WIDGET(user_data);
    if (!page_root) return;
    
    GtkWidget *revealer = g_object_get_data(G_OBJECT(page_root), "external_change_revealer");
    if (revealer && GTK_IS_REVEALER(revealer)) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), TRUE);
    }
}

static void
create_new_tab (ViteWindow *win, const char *title, Document *doc)
{
    if (!win) return;
    
    GtkWidget *scrolled = gtk_scrolled_window_new();
    GtkWidget *editor = editor_widget_new();
    
    /* Connect Indentation Sync Signals */
    g_signal_connect(editor, "notify::tab-width", G_CALLBACK(on_editor_notify_indentation), win);
    g_signal_connect(editor, "notify::indent-style", G_CALLBACK(on_editor_notify_indentation), win);
    g_signal_connect(editor, "notify::indent-width", G_CALLBACK(on_editor_notify_indentation), win);
    editor_widget_set_document(EDITOR_WIDGET(editor), doc);
    
    g_signal_connect(editor, "cursor-moved", G_CALLBACK(on_cursor_moved), win);
    g_signal_connect(editor, "insert-mode-changed", G_CALLBACK(on_insert_mode_changed), win);
    g_signal_connect(editor, "undo-redo-progress", G_CALLBACK(on_editor_undo_redo_progress), win);
    
    /* Synced via bindings in on_tab_clicked now */
    /* g_signal_connect(editor, "notify::show-line-numbers", ...); removed */
    
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), editor);
    
    const char *doc_path = document_get_file_path(doc);
    const char *selected_lang_id = "plain";  // Default to plain text
    
    if (doc_path) {
        const char *dot = strrchr(doc_path, '.');
        if (dot) {
            const char *ext = dot + 1;
            /* Restricted list as requested by user */
            if (g_ascii_strcasecmp(ext, "c") == 0) selected_lang_id = "c";
            else if (g_ascii_strcasecmp(ext, "py") == 0) selected_lang_id = "python";
            else if (g_ascii_strcasecmp(ext, "cpp") == 0) selected_lang_id = "cpp";
            else if (g_ascii_strcasecmp(ext, "json") == 0) selected_lang_id = "json";
            else if (g_ascii_strcasecmp(ext, "sh") == 0) selected_lang_id = "bash";
            else if (g_ascii_strcasecmp(ext, "rst") == 0) selected_lang_id = "rust"; /* Assuming rst means Rust here based on previous code, or ReStructredText? Previous code treated rst as rust. */
            else if (g_ascii_strcasecmp(ext, "rs") == 0) selected_lang_id = "rust";
            else if (g_ascii_strcasecmp(ext, "h") == 0) selected_lang_id = "h";
            else if (g_ascii_strcasecmp(ext, "js") == 0) selected_lang_id = "javascript";
            else if (g_ascii_strcasecmp(ext, "yaml") == 0 || g_ascii_strcasecmp(ext, "yml") == 0) selected_lang_id = "yaml";
            else if (g_ascii_strcasecmp(ext, "xml") == 0) selected_lang_id = "xml";
            else if (g_ascii_strcasecmp(ext, "desktop") == 0) selected_lang_id = "desktop";
            else if (g_ascii_strcasecmp(ext, "md") == 0 || g_ascii_strcasecmp(ext, "markdown") == 0 || g_ascii_strcasecmp(ext, "mkd") == 0) selected_lang_id = "markdown";
            
            if (strcmp(selected_lang_id, "plain") != 0) {
                editor_widget_set_language(EDITOR_WIDGET(editor), selected_lang_id);
            }
        }
    }
    
    /* Update action state and status bar regardless of whether language was set from extension or defaults to plain text */
    if (win && win->window) {
        /* Update the action state using the actual GtkWindow */
        GAction *action = g_action_map_lookup_action(G_ACTION_MAP(win->window), "set-file-type");
        if (action && G_IS_SIMPLE_ACTION(action)) {
            g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_string(selected_lang_id));
        }

        /* Also update the status bar to show the selected language */
        const char *display_name = "Plain Text";  // Default
        for (int i = 0; file_types[i].name != NULL; i++) {
            if (g_strcmp0(file_types[i].id, selected_lang_id) == 0) {
                display_name = file_types[i].name;
                break;
            }
        }
        /* Store original values for bold/normal diff tracking */
        g_object_set_data_full(G_OBJECT(editor), "original-file-type", g_strdup(display_name), g_free);
        g_object_set_data_full(G_OBJECT(editor), "original-encoding", g_strdup("utf-8"), g_free);
        g_object_set_data_full(G_OBJECT(editor), "original-newline", g_strdup("lf"), g_free);
        
        int tab_w = 4;
        int indent_s = 0;
        g_object_get(G_OBJECT(editor), "tab-width", &tab_w, "indent-style", &indent_s, NULL);
        g_object_set_data(G_OBJECT(editor), "original-indent-width", GINT_TO_POINTER(tab_w));
        g_object_set_data(G_OBJECT(editor), "original-indent-style", GINT_TO_POINTER(indent_s));

        if (win->status_bar) {
            if (g_strcmp0(display_name, "Plain Text") == 0) {
                vite_status_bar_set_file_type(VITE_STATUS_BAR(win->status_bar), NULL, FALSE);
            } else {
                vite_status_bar_set_file_type(VITE_STATUS_BAR(win->status_bar), _(display_name), FALSE);
            }
        }
    }
    
    /* Create per-view container (overlay + find bar + external-change revealer) */
    GtkWidget *view_container = create_view_container(win, scrolled);
    
    /* Wrap in TabRoot (Box) to support splitting */
    GtkWidget *page_root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(page_root, TRUE);
    gtk_widget_set_vexpand(page_root, TRUE);
    gtk_box_append(GTK_BOX(page_root), view_container);
    
    char id[32];
    sprintf(id, "page_%p", (void *)page_root);
    
    if (win->stack) {
        gtk_stack_add_named(win->stack, page_root, id);
    }
    
    char *final_title = g_strdup(title);
    
    /* Handle untitled numbering */
    if (g_strcmp0(title, "Untitled") == 0) {
        g_free(final_title);
        final_title = g_strdup_printf("Untitled %d", untitled_count++);
    }
    
    GtkWidget *tab = vite_tab_new(final_title);
    g_object_set_data_full(G_OBJECT(tab), "original_title", g_strdup(final_title), g_free);
    g_free(final_title);

    /* Tab Page is the Root Container (Box) */
    g_object_set_data(G_OBJECT(tab), "page", page_root);
    g_object_set_data(G_OBJECT(page_root), "tab", tab);
    
    g_signal_connect(tab, "clicked", G_CALLBACK(on_tab_clicked), NULL);
    g_signal_connect(tab, "close-clicked", G_CALLBACK(on_tab_close_clicked), NULL);
    g_signal_connect(tab, "move-to-new-window", G_CALLBACK(on_tab_move_to_new_window), NULL); /* Connect new signal */
    
    /* Connect modification and content callbacks */
    document_add_modification_callback(doc, on_document_modified, tab);
    document_add_content_callback(doc, on_document_content_changed, tab);
    document_add_external_change_callback(doc, on_document_changed_externally, view_container);
    
    /* Calculate insertion position (next to active) */
    int position = -1;
    if (win->tab_bar) {
        ViteTab *active = vite_tab_bar_get_active_tab(win->tab_bar);
        if (active) {
            GList *tabs = vite_tab_bar_get_tabs(win->tab_bar);
            int idx = g_list_index(tabs, active);
            if (idx != -1) position = idx + 1;
            g_list_free(tabs);
        }
    }
    
    vite_tab_bar_insert_tab(win->tab_bar, VITE_TAB(tab), position);
    
    /* Use on_tab_clicked to activate and update UI (Status Bar, Title, etc.) */
    on_tab_clicked(VITE_TAB(tab), win);
    
    /* Use defer_focus to safely grab focus after hierarchy is stable */
    defer_focus(editor);
}

#if 0
static void
on_close_curr_tab_clicked (GtkButton *btn, gpointer user_data G_GNUC_UNUSED)
{
    ViteWindow *win = NULL;
    if (btn) {
         GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(btn));
         win = g_object_get_data(G_OBJECT(root), "vite-window");
    }
    
    if (!win) return;
    
    ViteTab *active = vite_tab_bar_get_active_tab(win->tab_bar);
    if (active && VITE_IS_TAB(active)) {
        on_tab_close_clicked(active, NULL);
    }
}
#endif

static void
on_close_tab_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (!win || !win->tab_bar) return;
    ViteTab *active = vite_tab_bar_get_active_tab(win->tab_bar);
    if (active && VITE_IS_TAB(active)) {
        on_tab_close_clicked(active, NULL);
    }
}

static void
on_reopen_closed_tab_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (!win || !win->window) return;

    char *path = pop_recently_closed_file();
    if (!path) return;

    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(win->window));
    if (!app) {
        g_free(path);
        return;
    }

    GFile *file = g_file_new_for_path(path);
    queue_open_file(app, win, file, FALSE);
    g_object_unref(file);
    g_free(path);
}

static void
on_quit_window_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (!win || !win->window) return;
    gtk_window_close(GTK_WINDOW(win->window));
}





/* Go to Line Popover Implementation */

static void
on_goto_line_popever_closed(GtkPopover *popover, gpointer user_data G_GNUC_UNUSED)
{
    gtk_widget_unparent(GTK_WIDGET(popover));
}

static void
on_goto_perform(GtkWidget *widget, gpointer user_data)
{
    GtkWidget *popover_widget = gtk_widget_get_ancestor(widget, GTK_TYPE_POPOVER);
    if (!popover_widget) return;
    
    GtkPopover *popover = GTK_POPOVER(popover_widget);
    EditorWidget *editor = EDITOR_WIDGET(g_object_get_data(G_OBJECT(popover), "editor"));
    GtkSpinButton *spin = GTK_SPIN_BUTTON(user_data);
    
    int line = gtk_spin_button_get_value_as_int(spin);
    
    /* Convert 1-based UI to 0-based internal */
    if (line > 0) line--;
    
    editor_widget_scroll_to_line(editor, (size_t)line);
    
    /* Close popover */
    gtk_popover_popdown(popover);
    
    /* Focus editor */
    gtk_widget_grab_focus(GTK_WIDGET(editor));
}

static void
show_goto_line_popover(GtkWidget *parent_widget, EditorWidget *editor)
{
    GtkWidget *popover = gtk_popover_new();
    gtk_widget_set_parent(popover, parent_widget); /* Attach to overlay or parent */
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    
    /* Force it to appear "inside" (at top center usually) by pointing to top edge? 
       Or using valign/halign if supported? 
       Let's try pointing to a rectangle at the top center of parent. */
    int w = gtk_widget_get_width(parent_widget);
    GdkRectangle rect = {w / 2, 0, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    
    /* Set alignment/pointing to top-right or center? 
       Usually centered or near header. Let's verify existing overlay usage.
       For now, standard popover behavior. */
       
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(box, 3);
    gtk_widget_set_margin_bottom(box, 3);
    gtk_widget_set_margin_start(box, 3);
    gtk_widget_set_margin_end(box, 3);
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    
    GtkWidget *label = gtk_label_new(_("Go to Line:"));
    gtk_box_append(GTK_BOX(box), label);
    
    /* Determine max lines */
    Document *doc = editor_widget_get_document(editor);
    size_t total = document_get_line_count(doc);
    if (total == 0) total = 1;
    
    GtkWidget *spin = gtk_spin_button_new_with_range(1, (double)total, 1);
    gtk_box_append(GTK_BOX(box), spin);
    
    /* Activate default */
    gtk_widget_set_can_focus(spin, TRUE);
    gtk_popover_set_default_widget(GTK_POPOVER(popover), spin);
    
    GtkWidget *btn = gtk_button_new_with_label("Go");
    gtk_widget_add_css_class(btn, "suggested-action");
    gtk_box_append(GTK_BOX(box), btn);
    
    /* Store editor on popover */
    g_object_set_data(G_OBJECT(popover), "editor", editor);
    
    g_signal_connect(btn, "clicked", G_CALLBACK(on_goto_perform), spin);
    g_signal_connect(spin, "activate", G_CALLBACK(on_goto_perform), spin); /* Enter key triggers perform directly */
    
    g_signal_connect(popover, "closed", G_CALLBACK(on_goto_line_popever_closed), NULL);
    
    gtk_popover_popup(GTK_POPOVER(popover));
}

static void
on_filter_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    GtkWidget *active = get_active_editor(win);
    if (!active) return;

    /* Traverse up to find the root_box of the current view split */
    GtkWidget *parent = active;
    while (parent && !g_object_get_data(G_OBJECT(parent), "find_bar")) {
        parent = gtk_widget_get_parent(parent);
    }

    if (parent) {
        GtkWidget *find_bar = GTK_WIDGET(g_object_get_data(G_OBJECT(parent), "find_bar"));
        if (find_bar) {
            /* Check if we are already in filter mode and visible */
            // gboolean visible = gtk_widget_get_visible(find_bar);
            /* TODO: Check if currently in filter mode? 
               ViteFindReplaceBar doesn't expose a getter for mode easily, 
               but we can just show it. If it was find mode, it switches to filter.
               If it was hidden, it shows.
               If it was already filter mode and visible, maybe we want to hide it?
               The old logic toggled visibility.
               
               For unified bar:
               - If hidden: show in filter mode.
               - If visible:
                 - If in find mode: switch to filter mode.
                 - If in filter mode: hide.
               
               We need a way to check current mode?
               Or just always show?
               
               Let's assume standard behavior: Ctrl+Alt+F always shows filter bar. 
               If it's already focused and in filter mode, maybe toggle?
               Let's stick to simple: Show filter. Toggle if already visible?
               
               Wait, `vite_find_replace_bar_show_filter` sets mode and shows.
               
               Let's verify if we want toggle behavior.
            */
            
            /* Logic: 
               If bar is NOT visible -> Show Filter.
               If bar IS visible:
                 If we can check mode, and it is FILTER, then hide.
                 Else (FIND mode) -> Switch to Filter.
            */
            
            /* We don't have public API to check mode yet. 
               But we can just call show_filter. 
               
               However, user wants to toggle it off too? 
               Usually Esc or the button closes it. 
               Ctrl+F usually just focuses it if open.
               Let's just show/focus it for now.
            */
             
             vite_find_replace_bar_show_filter(VITE_FIND_REPLACE_BAR(find_bar));
             
             /* Pre-fill with selection */
             char *sel = editor_widget_get_selected_text(EDITOR_WIDGET(active));
             if (sel && strlen(sel) > 0) {
                 vite_find_replace_bar_set_search_text(VITE_FIND_REPLACE_BAR(find_bar), sel);
             }
             g_free(sel);
        }
    }
}

static void
on_goto_line_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    
    /* 1. Identify active editor in active tab/split */
    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    if (!tab) return;
    
    /* Get last focused child in the tab to identify the correct split */
    GtkWidget *focused_child = vite_tab_get_last_focused_child(tab);
    
    /* If no specific child tracked, fallback to finding one */
    if (!focused_child) {
        GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
        if (page) focused_child = find_first_editor_recursive(page);
        if (focused_child) {
             /* usually find_first returns editor directly, need its overlay/container? */
        }
    }
    
    /* We want the EditorWidget and its parent Overlay to attach the popover to */
    GtkWidget *editor = NULL;
    GtkWidget *attach_target = NULL;
    
    if (focused_child) {
        if (EDITOR_IS_WIDGET(focused_child)) {
            editor = focused_child;
            /* Walk up to find overlay */
            attach_target = gtk_widget_get_ancestor(editor, GTK_TYPE_OVERLAY);
        } else if (GTK_IS_OVERLAY(focused_child)) {
             attach_target = focused_child;
             editor = gtk_overlay_get_child(GTK_OVERLAY(focused_child));
             /* If child is scrolled window? */
             if (GTK_IS_SCROLLED_WINDOW(editor)) {
                 editor = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(editor));
             }
        } else {
             /* Might be a container, search down */
            editor = find_first_editor_recursive(focused_child);
            if (editor) attach_target = gtk_widget_get_ancestor(editor, GTK_TYPE_OVERLAY);
        }
    }
    
    if (editor && EDITOR_IS_WIDGET(editor) && attach_target) {
        show_goto_line_popover(attach_target, EDITOR_WIDGET(editor));
    }
}

static GtkWidget *
get_active_editor(ViteWindow *win)
{
    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    GtkWidget *page = NULL;
    
    if (tab) {
        page = g_object_get_data(G_OBJECT(tab), "page");
    } else {
        /* Fallback: internal stack state */
        if (win->stack) {
             page = gtk_stack_get_visible_child(win->stack);
        }
    }
    
    if (!page) return NULL;
    
    /* If we have a tab, check for specific focused split */
    if (tab) {
        GtkWidget *overlay = vite_tab_get_last_focused_child(tab);
        if (overlay && GTK_IS_OVERLAY(overlay)) {
            GtkWidget *child = gtk_overlay_get_child(GTK_OVERLAY(overlay));
            if (GTK_IS_SCROLLED_WINDOW(child)) {
                child = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(child));
            }
            if (EDITOR_IS_WIDGET(child)) {
                return child;
            }
        }
    }
    
    GtkWidget *result = get_editor_from_page(page);
    if (!result && win->last_active_editor && GTK_IS_WIDGET(win->last_active_editor)) {
         return win->last_active_editor;
    }
    
    /* Absolute Fallback for Single View: Find ANY editor in the page */
    if (!result && page) {
        result = find_first_editor_recursive(page);
    }
    
    return result;
}

static void
on_status_bar_file_type_changed(ViteStatusBar *bar G_GNUC_UNUSED, const char *lang_id, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    GtkWidget *active_editor = get_active_editor(win);
    
    if (tab) {
        GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
        if (page) {
            GPtrArray *editors = find_all_editors_recursive(page);
            for (guint i = 0; i < editors->len; i++) {
                editor_widget_set_language(EDITOR_WIDGET(g_ptr_array_index(editors, i)), lang_id);
            }
            g_ptr_array_unref(editors);
        }
    }

    /* Compute display name and evaluate whether it has changed relative to original loaded type */
    gboolean is_changed = FALSE;
    const char *display_name = NULL;
    if (lang_id) {
        for (int i = 0; file_types[i].name != NULL; i++) {
            if (g_strcmp0(file_types[i].id, lang_id) == 0) {
                display_name = file_types[i].name;
                break;
            }
        }
    }
    
    if (active_editor && EDITOR_IS_WIDGET(active_editor)) {
        const char *orig_lang = g_object_get_data(G_OBJECT(active_editor), "original-file-type");
        if (!orig_lang) orig_lang = "Plain Text";
        is_changed = (g_strcmp0(display_name ? display_name : "Plain Text", orig_lang) != 0);
    }
    
    if (win->status_bar) {
        if (g_strcmp0(lang_id, "plain") == 0 || !lang_id) {
            vite_status_bar_set_file_type(VITE_STATUS_BAR(win->status_bar), NULL, is_changed);
        } else if (display_name) {
            vite_status_bar_set_file_type(VITE_STATUS_BAR(win->status_bar), _(display_name), is_changed);
        }
    }

    /* Update the corresponding action state to reflect in the menu */
    if (win && win->window) {
        GAction *action = g_action_map_lookup_action(G_ACTION_MAP(GTK_WINDOW(win->window)), "set-file-type");
        if (action && G_IS_SIMPLE_ACTION(action)) {
            /* Convert NULL to "plain" for the action state */
            const char *action_lang_id = (lang_id == NULL) ? "plain" : lang_id;
            g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_string(action_lang_id));
        }
    }
    
    /* Grab focus back to editor */
    if (active_editor && GTK_IS_WIDGET(active_editor)) {
         gtk_widget_grab_focus(active_editor);
    }
}

static void
on_status_bar_line_ending_changed(ViteStatusBar *bar G_GNUC_UNUSED, const char *line_ending_id, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    GtkWidget *active_editor = get_active_editor(win);
    
    if (tab) {
        GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
        if (page) {
            GPtrArray *editors = find_all_editors_recursive(page);
            for (guint i = 0; i < editors->len; i++) {
                editor_widget_set_line_ending(EDITOR_WIDGET(g_ptr_array_index(editors, i)), line_ending_id);
            }
            g_ptr_array_unref(editors);
        }
    }
    
    gboolean is_changed = FALSE;
    if (active_editor && EDITOR_IS_WIDGET(active_editor)) {
        const char *orig_newline = g_object_get_data(G_OBJECT(active_editor), "original-newline");
        if (!orig_newline) orig_newline = "lf";
        is_changed = (g_strcmp0(line_ending_id, orig_newline) != 0);
    }
    
    if (win->status_bar) {
        vite_status_bar_set_line_ending(VITE_STATUS_BAR(win->status_bar), line_ending_id, is_changed);
    }
    
    /* Always Sync Menu Action */
    GActionMap *map = G_ACTION_MAP(GTK_WINDOW(win->window));
    GAction *act = g_action_map_lookup_action(map, "set-line-ending");
    if (act) {
        /* ID should match keys: lf, crlf, cr */
        g_simple_action_set_state(G_SIMPLE_ACTION(act), g_variant_new_string(line_ending_id));
    }
    
    /* Grab focus back to editor */
    if (active_editor && GTK_IS_WIDGET(active_editor)) {
         gtk_widget_grab_focus(active_editor);
    }
}

static void
on_status_bar_encoding_changed(ViteStatusBar *bar G_GNUC_UNUSED, const char *encoding_id, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    GtkWidget *active_editor = get_active_editor(win);
    
    if (tab) {
        GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
        if (page) {
            GPtrArray *editors = find_all_editors_recursive(page);
            for (guint i = 0; i < editors->len; i++) {
                editor_widget_set_encoding(EDITOR_WIDGET(g_ptr_array_index(editors, i)), encoding_id);
            }
            g_ptr_array_unref(editors);
        }
    }
    
    gboolean is_changed = FALSE;
    if (active_editor && EDITOR_IS_WIDGET(active_editor)) {
        const char *orig_enc = g_object_get_data(G_OBJECT(active_editor), "original-encoding");
        if (!orig_enc) orig_enc = "utf-8";
        is_changed = (g_strcmp0(encoding_id, orig_enc) != 0);
    }
    
    if (win->status_bar) {
        vite_status_bar_set_encoding(VITE_STATUS_BAR(win->status_bar), encoding_id, is_changed);
    }
    
    /* Always Sync Menu Action to match Status Bar selection */
    GActionMap *map = G_ACTION_MAP(GTK_WINDOW(win->window));
    GAction *act = g_action_map_lookup_action(map, "set-encoding");
    if (act) {
        g_simple_action_set_state(G_SIMPLE_ACTION(act), g_variant_new_string(encoding_id));
    }
    
    /* Grab focus back to editor */
    if (active_editor && GTK_IS_WIDGET(active_editor)) {
         gtk_widget_grab_focus(active_editor);
    }
}

static void
on_status_bar_indent_width_changed(ViteStatusBar *bar, int width, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    if (tab) {
        GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
        if (page) {
            GPtrArray *editors = find_all_editors_recursive(page);
            for (guint i = 0; i < editors->len; i++) {
                GtkWidget *ed = g_ptr_array_index(editors, i);
                g_object_set(ed, "tab-width", width, "indent-width", width, NULL);
            }
            if (editors->len > 0) {
                 GtkWidget *ed0 = g_ptr_array_index(editors, 0);
                 int style = 0;
                 g_object_get(ed0, "indent-style", &style, NULL);
                 
                 gpointer orig_w_ptr = g_object_get_data(G_OBJECT(ed0), "original-indent-width");
                 gpointer orig_style_ptr = g_object_get_data(G_OBJECT(ed0), "original-indent-style");
                 int orig_w = orig_w_ptr ? GPOINTER_TO_INT(orig_w_ptr) : 4;
                 int orig_style = orig_style_ptr ? GPOINTER_TO_INT(orig_style_ptr) : 0;
                 
                 gboolean indent_changed = (width != orig_w || style != orig_style);
                 vite_status_bar_set_indentation(bar, width, style == 1, indent_changed);
            }
            g_ptr_array_unref(editors);
        }
    }
    
    /* Grab focus back to editor */
    GtkWidget *active_editor = get_active_editor(win);
    if (active_editor && GTK_IS_WIDGET(active_editor)) {
         gtk_widget_grab_focus(active_editor);
    }
}

static void
on_status_bar_indent_style_changed(ViteStatusBar *bar, int style, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    if (tab) {
        GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
        if (page) {
            GPtrArray *editors = find_all_editors_recursive(page);
            for (guint i = 0; i < editors->len; i++) {
                g_object_set(g_ptr_array_index(editors, i), "indent-style", style, NULL);
            }
            if (editors->len > 0) {
                 GtkWidget *ed0 = g_ptr_array_index(editors, 0);
                 int width = 4;
                 g_object_get(ed0, "tab-width", &width, NULL);
                 
                 gpointer orig_w_ptr = g_object_get_data(G_OBJECT(ed0), "original-indent-width");
                 gpointer orig_style_ptr = g_object_get_data(G_OBJECT(ed0), "original-indent-style");
                 int orig_w = orig_w_ptr ? GPOINTER_TO_INT(orig_w_ptr) : 4;
                 int orig_style = orig_style_ptr ? GPOINTER_TO_INT(orig_style_ptr) : 0;
                 
                 gboolean indent_changed = (width != orig_w || style != orig_style);
                 vite_status_bar_set_indentation(bar, width, style == 1, indent_changed);
            }
            g_ptr_array_unref(editors);
        }
    }
    
    /* Grab focus back to editor */
    GtkWidget *active_editor = get_active_editor(win);
    if (active_editor && GTK_IS_WIDGET(active_editor)) {
         gtk_widget_grab_focus(active_editor);
    }
}

static void
update_status_bar_from_editor(ViteWindow *win, GtkWidget *editor)
{
    if (!win || !win->status_bar || !editor || !EDITOR_IS_WIDGET(editor)) return;
    
    int tab_w = 4;
    int indent_s = 0;
    g_object_get(G_OBJECT(editor), "tab-width", &tab_w, "indent-style", &indent_s, NULL);
    
    gpointer orig_w_ptr = g_object_get_data(G_OBJECT(editor), "original-indent-width");
    gpointer orig_style_ptr = g_object_get_data(G_OBJECT(editor), "original-indent-style");
    int orig_w = orig_w_ptr ? GPOINTER_TO_INT(orig_w_ptr) : 4;
    int orig_style = orig_style_ptr ? GPOINTER_TO_INT(orig_style_ptr) : 0;
    
    gboolean indent_changed = (tab_w != orig_w || indent_s != orig_style);
    vite_status_bar_set_indentation(VITE_STATUS_BAR(win->status_bar), tab_w, indent_s == 1, indent_changed);
}

static void
on_editor_notify_indentation(GObject *editor, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    
    /* Only update if this is the active editor */
    GtkWidget *active = get_active_editor(win);
    if (active == GTK_WIDGET(editor)) {
        update_status_bar_from_editor(win, GTK_WIDGET(editor));
    }
}

static void
on_fullscreen_hover_motion(GtkEventControllerMotion *controller G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (!win || !win->window || !win->titlebar_container) return;
    
    if (gtk_window_is_fullscreen(GTK_WINDOW(win->window))) {
        int h = gtk_widget_get_height(GTK_WIDGET(win->window));
        
        /* Check Top Edge */
        if (y < 5) { 
             adw_toolbar_view_set_reveal_top_bars(ADW_TOOLBAR_VIEW(win->titlebar_container), TRUE);
             /* Top edge hover should ALSO reveal status bar per requirements */
             adw_toolbar_view_set_reveal_bottom_bars(ADW_TOOLBAR_VIEW(win->titlebar_container), TRUE);
        }
        
        /* Check Bottom Edge */
        else if (y > h - 5) {
             /* Bottom edge hover reveals status bar */
             adw_toolbar_view_set_reveal_bottom_bars(ADW_TOOLBAR_VIEW(win->titlebar_container), TRUE);
        }
        
        /* Hiding Logic with Hysteresis */
        else {
            /* Hide top bar if far enough down */
            if (y > 80 && adw_toolbar_view_get_reveal_top_bars(ADW_TOOLBAR_VIEW(win->titlebar_container))) {
                 adw_toolbar_view_set_reveal_top_bars(ADW_TOOLBAR_VIEW(win->titlebar_container), FALSE);
            }
            
            /* Hide bottom bar if far enough up */
            if (y < h - 80 && adw_toolbar_view_get_reveal_bottom_bars(ADW_TOOLBAR_VIEW(win->titlebar_container))) {
                 adw_toolbar_view_set_reveal_bottom_bars(ADW_TOOLBAR_VIEW(win->titlebar_container), FALSE);
            }
        }
    }
}


static void
on_set_encoding(GSimpleAction *action G_GNUC_UNUSED, GVariant *value, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    g_simple_action_set_state(action, value);
    
    const char *encoding = g_variant_get_string(value, NULL);
    GtkWidget *editor = get_active_editor(win);
    gboolean is_changed = FALSE;
    if (editor && EDITOR_IS_WIDGET(editor)) {
        editor_widget_set_encoding(EDITOR_WIDGET(editor), encoding);
        const char *orig_enc = g_object_get_data(G_OBJECT(editor), "original-encoding");
        if (!orig_enc) orig_enc = "utf-8";
        is_changed = (g_strcmp0(encoding, orig_enc) != 0);
    }
    
    /* Pass ID directly; Status Bar handles text mapping */
    /* Update Status Bar regardless of editor presence to maintain UI consistency */
    vite_status_bar_set_encoding(VITE_STATUS_BAR(win->status_bar), encoding, is_changed);
    
    if (editor && EDITOR_IS_WIDGET(editor)) {
        gtk_widget_grab_focus(editor);
    }
}

static void
on_set_line_ending(GSimpleAction *action G_GNUC_UNUSED, GVariant *value, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    g_simple_action_set_state(action, value);
    
    const char *le_id = g_variant_get_string(value, NULL);
    GtkWidget *editor = get_active_editor(win);
    gboolean is_changed = FALSE;
    if (editor && EDITOR_IS_WIDGET(editor)) {
        editor_widget_set_line_ending(EDITOR_WIDGET(editor), le_id);
        const char *orig_nl = g_object_get_data(G_OBJECT(editor), "original-newline");
        if (!orig_nl) orig_nl = "lf";
        is_changed = (g_strcmp0(le_id, orig_nl) != 0);
    }
    
    /* Pass ID directly */
    /* Update Status Bar regardless of editor presence */
    vite_status_bar_set_line_ending(VITE_STATUS_BAR(win->status_bar), le_id, is_changed);

    if (editor && EDITOR_IS_WIDGET(editor)) {
         gtk_widget_grab_focus(editor);
    }
}

static void
on_set_file_type(GSimpleAction *action G_GNUC_UNUSED, GVariant *value, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    g_simple_action_set_state(action, value);

    const char *lang_id = g_variant_get_string(value, NULL);
    GtkWidget *editor = get_active_editor(win);
    gboolean is_changed = FALSE;
    if (editor && EDITOR_IS_WIDGET(editor)) {
        /* Convert "plain" to NULL for editor widget (which expects NULL for plain text) */
        const char *editor_lang_id = (g_strcmp0(lang_id, "plain") == 0) ? NULL : lang_id;
        editor_widget_set_language(EDITOR_WIDGET(editor), editor_lang_id);
    }

    /* Update Status Bar - need to map ID to display name */
    /* Find the display name for the language ID */
    const char *display_name = NULL;
    for (int i = 0; file_types[i].name != NULL; i++) {
        if (g_strcmp0(file_types[i].id, lang_id) == 0) {
            display_name = file_types[i].name;
            break;
        }
    }
    
    if (editor && EDITOR_IS_WIDGET(editor)) {
        const char *orig_lang = g_object_get_data(G_OBJECT(editor), "original-file-type");
        if (!orig_lang) orig_lang = "Plain Text";
        is_changed = (g_strcmp0(display_name ? display_name : "Plain Text", orig_lang) != 0);
    }
    
    if (win->status_bar) {
        if (g_strcmp0(lang_id, "plain") == 0) {
            vite_status_bar_set_file_type(VITE_STATUS_BAR(win->status_bar), NULL, is_changed);
        } else if (display_name) {
            vite_status_bar_set_file_type(VITE_STATUS_BAR(win->status_bar), _(display_name), is_changed);
        }
    }
    
    if (editor && EDITOR_IS_WIDGET(editor)) {
        gtk_widget_grab_focus(editor);
    }
}

static void
on_show_line_numbers_toggled(GSimpleAction *action G_GNUC_UNUSED, GVariant *value, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    gboolean state = g_variant_get_boolean(value);
    g_simple_action_set_state(action, value);
    
    GtkWidget *editor = get_active_editor(win);
    if (editor && EDITOR_IS_WIDGET(editor)) {
         editor_widget_set_show_line_numbers(EDITOR_WIDGET(editor), state);
         gtk_widget_grab_focus(editor);
    }
}

static void
on_enable_word_wrap_toggled(GSimpleAction *action G_GNUC_UNUSED, GVariant *value, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    gboolean state = g_variant_get_boolean(value);
    g_simple_action_set_state(action, value);
    
    GtkWidget *editor = get_active_editor(win);
    if (editor && EDITOR_IS_WIDGET(editor)) {
         editor_widget_set_word_wrap(EDITOR_WIDGET(editor), state);
         gtk_widget_grab_focus(editor);
    }
}

static void
on_show_status_bar_toggled(GSimpleAction *action G_GNUC_UNUSED, GVariant *value, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    gboolean state = g_variant_get_boolean(value);
    g_simple_action_set_state(action, value);
    
    if (win->status_bar) {
        gtk_widget_set_visible(win->status_bar, state);
    }

    GtkWidget *editor = get_active_editor(win);
    if (editor && EDITOR_IS_WIDGET(editor)) {
         gtk_widget_grab_focus(editor);
    }
}

static void
on_show_save_button_toggled(GSimpleAction *action G_GNUC_UNUSED, GVariant *value, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    gboolean state = g_variant_get_boolean(value);
    g_simple_action_set_state(action, value);

    if (win->save_button) {
        gtk_widget_set_visible(win->save_button, state);
    }

    GtkWidget *editor = get_active_editor(win);
    if (editor && EDITOR_IS_WIDGET(editor)) {
         gtk_widget_grab_focus(editor);
    }
}

/* Function to update save button visibility from preferences dialog */
void update_save_button_visibility_from_preferences(GtkWindow *window, gboolean visible) {
    if (!window) return;
    
    ViteWindow *win = g_object_get_data(G_OBJECT(window), "vite-window");
    if (win && win->save_button) {
        gtk_widget_set_visible(win->save_button, visible);
        
        /* Also update the corresponding action state */
        GAction *action = g_action_map_lookup_action(G_ACTION_MAP(window), "show-save-button");
        if (action) {
            g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(visible));
        }
    }
}

static void
on_toggle_insert_mode(GSimpleAction *action G_GNUC_UNUSED, GVariant *value G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    GtkWidget *editor = get_active_editor(win);
    if (editor && EDITOR_IS_WIDGET(editor)) {
         gboolean current = editor_widget_get_insert_mode(EDITOR_WIDGET(editor));
         editor_widget_set_insert_mode(EDITOR_WIDGET(editor), !current);
         gtk_widget_grab_focus(editor);
    }
}

static void
check_close_when_done(ViteWindow *win)
{
    if (win->loading_count == 0) {
        /* Auto-close any pending localized operation dialog */
        AdwAlertDialog *dialog = g_weak_ref_get(&win->active_dialog_ref);
        if (dialog) {
            adw_dialog_close(ADW_DIALOG(dialog));
            g_object_unref(dialog);
            g_weak_ref_set(&win->active_dialog_ref, NULL);
        }
        
        if (win->close_when_done) {
            if (win->window) {
                gtk_window_close(GTK_WINDOW(win->window));
            }
        }
    }
}

static void
on_select_all_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *value G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    GtkWidget *editor = get_active_editor(win);
    if (editor && EDITOR_IS_WIDGET(editor)) {
         editor_widget_select_all(EDITOR_WIDGET(editor));
         gtk_widget_grab_focus(editor);
    }
}

static void
on_zoom_in_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    GtkWidget *editor = get_active_editor(win);
    if (editor && EDITOR_IS_WIDGET(editor)) {
        editor_widget_zoom_in(EDITOR_WIDGET(editor));
        gtk_widget_grab_focus(editor);
    }
}

static void
on_zoom_out_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    GtkWidget *editor = get_active_editor(win);
    if (editor && EDITOR_IS_WIDGET(editor)) {
        editor_widget_zoom_out(EDITOR_WIDGET(editor));
        gtk_widget_grab_focus(editor);
    }
}

static void
on_zoom_reset_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow*)user_data;
    GtkWidget *editor = get_active_editor(win);
    if (editor && EDITOR_IS_WIDGET(editor)) {
        editor_widget_zoom_reset(EDITOR_WIDGET(editor));
        gtk_widget_grab_focus(editor);
    }
}

static GPtrArray *
collect_modified_tabs(ViteWindow *win)
{
    GPtrArray *modified = g_ptr_array_new_with_free_func(g_object_unref);
    if (!win || !win->tab_bar) return modified;

    GList *tabs = vite_tab_bar_get_tabs(win->tab_bar);
    for (GList *l = tabs; l != NULL; l = l->next) {
        ViteTab *tab = VITE_TAB(l->data);
        if (tab_has_unsaved_changes(tab)) {
            g_ptr_array_add(modified, g_object_ref(tab));
        }
    }
    g_list_free(tabs);
    return modified;
}

static void
on_unsaved_window_close_response(const char *response, SaveChangesDialogData *dialog, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (!win) return;

    if (g_strcmp0(response, "discard") == 0) {
        win->force_close_once = TRUE;
        gtk_window_close(GTK_WINDOW(win->window));
        return;
    }

    if (g_strcmp0(response, "save") == 0) {
        gboolean started_saves = FALSE;
        for (guint i = 0; i < dialog->tabs->len; i++) {
            ViteTab *tab = VITE_TAB(g_ptr_array_index(dialog->tabs, i));
            if (!save_changes_dialog_tab_selected(dialog, tab)) continue;

            Document *doc = get_tab_document(tab);
            if (!doc || !document_is_modified(doc)) continue;

            char *filename = save_changes_dialog_filename_for_tab(dialog, tab);
            const char *current_path = document_get_file_path(doc);
            char *final_path = NULL;

            if (current_path && *current_path) {
                char *current_basename = g_path_get_basename(current_path);
                if (g_strcmp0(filename, current_basename) != 0) {
                    /* Filename changed - Save As in same directory */
                    char *dir = g_path_get_dirname(current_path);
                    final_path = g_build_filename(dir, filename, NULL);
                    g_free(dir);
                } else {
                    /* Save normally */
                    final_path = g_strdup(current_path);
                }
                g_free(current_basename);
            } else {
                /* Untitled file - Save to default directory */
                char *dir = default_save_directory();
                final_path = g_build_filename(dir, filename, NULL);
                g_free(dir);
            }

            save_async_with_progress(win, tab, doc, final_path);
            g_free(final_path);
            g_free(filename);
            started_saves = TRUE;
        }

        if (!started_saves) {
            win->force_close_once = TRUE;
            gtk_window_close(GTK_WINDOW(win->window));
            return;
        }

        win->close_when_done = TRUE;
        check_close_when_done(win);
    }
}

static void
on_window_close_response(AdwAlertDialog *dialog G_GNUC_UNUSED, const char *response, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    
    /* Clear active dialog ref */
    g_weak_ref_set(&win->active_dialog_ref, NULL);
    
    if (g_strcmp0(response, "cancel-close") == 0) {
        win->close_when_done = TRUE;
        if (win->tab_bar) {
            GList *tabs = vite_tab_bar_get_tabs(win->tab_bar);
            for (GList *l = tabs; l != NULL; l = l->next) {
                ViteTab *tab = VITE_TAB(l->data);
                if (vite_tab_is_loading(tab)) vite_tab_cancel_load(tab);
            }
            g_list_free(tabs);
        }
    } else if (g_strcmp0(response, "wait-close") == 0) {
        win->close_when_done = TRUE;
        /* If for some reason finished already (race), check now */
        check_close_when_done(win);
    } else if (g_strcmp0(response, "cancel-save") == 0 || g_strcmp0(response, "cancel-load") == 0) {
        win->close_when_done = FALSE; /* User wants to stay open */
        if (win->tab_bar) {
            GList *tabs = vite_tab_bar_get_tabs(win->tab_bar);
            for (GList *l = tabs; l != NULL; l = l->next) {
                ViteTab *tab = VITE_TAB(l->data);
                if (vite_tab_is_loading(tab)) vite_tab_cancel_load(tab);
            }
            g_list_free(tabs);
        }
    }
    /* continue: do nothing */
}

static gboolean
on_window_close_request(GtkWindow *window, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;

    if (win->force_close_once) {
        win->force_close_once = FALSE;
        return FALSE;
    }
    
    if (win->loading_count > 0) {
        GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(window));
        
        gboolean any_saving = FALSE;
        if (win->tab_bar) {
             GList *tabs = vite_tab_bar_get_tabs(win->tab_bar);
             for (GList *l = tabs; l != NULL; l = l->next) {
                 if (vite_tab_get_operation_type(VITE_TAB(l->data)) == VITE_OP_SAVING) {
                     any_saving = TRUE;
                     break;
                 }
             }
             g_list_free(tabs);
        }
        
        AdwAlertDialog *dialog = NULL;
        
        if (any_saving) {
            dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Save in Progress", 
                                                         "Choose how to proceed with the active operation."));
            adw_alert_dialog_add_response(dialog, "cancel-close", "Cancel & Close");
            adw_alert_dialog_add_response(dialog, "wait-close", "Close after Save");
            adw_alert_dialog_add_response(dialog, "cancel-save", "Cancel Saving");
            adw_alert_dialog_add_response(dialog, "continue", "Continue Saving");
            
            adw_alert_dialog_set_response_appearance(dialog, "cancel-close", ADW_RESPONSE_DESTRUCTIVE);
        } else {
            /* Loading */
            dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Load in Progress", 
                                                         "Choose how to proceed with the active operation."));
            adw_alert_dialog_add_response(dialog, "cancel-close", "Cancel & Close");
            adw_alert_dialog_add_response(dialog, "cancel-load", "Cancel Loading");
            adw_alert_dialog_add_response(dialog, "continue", "Continue Loading");
            
            adw_alert_dialog_set_response_appearance(dialog, "cancel-close", ADW_RESPONSE_DESTRUCTIVE);
        }
        
        adw_alert_dialog_set_default_response(dialog, "continue");
        
        /* Track active dialog */
        g_weak_ref_set(&win->active_dialog_ref, dialog);
        
        g_signal_connect(dialog, "response", G_CALLBACK(on_window_close_response), win);
        adw_alert_dialog_choose(dialog, GTK_WIDGET(root), NULL, NULL, NULL);
        
        return TRUE; /* Prevent close */
    }

    GPtrArray *modified_tabs = collect_modified_tabs(win);
    if (modified_tabs->len > 0) {
        show_save_changes_dialog(win, modified_tabs, on_unsaved_window_close_response, win);
        g_ptr_array_unref(modified_tabs);
        return TRUE;
    }
    g_ptr_array_unref(modified_tabs);
    
    return FALSE; /* Allow close */
}

static void
on_enable_folding_toggled(GSimpleAction *action G_GNUC_UNUSED, GVariant *value, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    gboolean enabled = g_variant_get_boolean(value);
    
    GtkWidget *editor = get_active_editor(win);
    if (EDITOR_IS_WIDGET(editor)) {
         g_object_set(editor, "enable-folding", enabled, NULL);
         gtk_widget_grab_focus(editor);
    }
    
    g_simple_action_set_state(action, value);
}

static void
on_enable_minimap_toggled(GSimpleAction *action G_GNUC_UNUSED, GVariant *value, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    gboolean enabled = g_variant_get_boolean(value);

    GtkWidget *editor = get_active_editor(win);
    if (EDITOR_IS_WIDGET(editor)) {
         g_object_set(editor, "minimap-enabled", enabled, NULL);
         gtk_widget_grab_focus(editor);
    }

    g_simple_action_set_state(action, value);
}



static void
on_window_destroy(GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (!win) return;

    /* Cancel any pending auto-hide */
    if (win->auto_hide_source_id > 0) {
        g_source_remove(win->auto_hide_source_id);
        win->auto_hide_source_id = 0;
    }

    /* Clear weak refs */
    g_weak_ref_clear(&win->active_dialog_ref);
    
    /* Pointers to widgets (stack, tab_bar, etc.) don't need free as they are destroyed with window */

    g_free(win);
}

static ViteWindow *
setup_window(AdwApplicationWindow *window)
{
    ViteWindow *win = g_new0(ViteWindow, 1);
    win->window = window;
    g_weak_ref_init(&win->active_dialog_ref, NULL);
    g_signal_connect(window, "close-request", G_CALLBACK(on_window_close_request), win);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), win);
    g_signal_connect(window, "notify::fullscreened", G_CALLBACK(on_window_fullscreen_state_changed), win);
    g_object_set_data(G_OBJECT(window), "vite-window", win);
    gtk_widget_add_css_class(GTK_WIDGET(window), "vite-main-window");

    load_css();

    /* Use AdwToolbarView as main container for proper RAISED styling */
    GtkWidget *toolbar_view = adw_toolbar_view_new();
    adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(toolbar_view), ADW_TOOLBAR_RAISED);
    adw_toolbar_view_set_bottom_bar_style(ADW_TOOLBAR_VIEW(toolbar_view), ADW_TOOLBAR_RAISED);
    win->titlebar_container = toolbar_view;
    gtk_widget_add_css_class(toolbar_view, "titlebar-box");
    
    /* NOTE: AdwApplicationWindow does not support gtk_window_set_titlebar().
     * The titlebar is integrated into the main content area below. */
    
    /* Add drop target to window to accept tabs and files */
    GtkDropTarget *window_drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    GType drop_types[] = { VITE_TYPE_TAB, GDK_TYPE_FILE_LIST };
    gtk_drop_target_set_gtypes(window_drop, drop_types, G_N_ELEMENTS(drop_types));
    g_signal_connect(window_drop, "drop", G_CALLBACK(on_window_drop), win);
    gtk_widget_add_controller(GTK_WIDGET(window), GTK_EVENT_CONTROLLER(window_drop));

    /* Seperate drop target for toolbar_view area */
    GtkDropTarget *toolbar_drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    gtk_drop_target_set_gtypes(toolbar_drop, drop_types, G_N_ELEMENTS(drop_types));
    g_signal_connect(toolbar_drop, "drop", G_CALLBACK(on_window_drop), win);
    gtk_widget_add_controller(GTK_WIDGET(toolbar_view), GTK_EVENT_CONTROLLER(toolbar_drop));
    
    GtkWidget *header = adw_header_bar_new();
    win->header_bar = ADW_HEADER_BAR(header);
    /* gtk_widget_add_css_class(header, "flat"); Removed to ensure background visibility in overlay */
    gtk_widget_set_margin_bottom(header, 4);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header);
    
    /* Fullscreen Restore Button */
    GtkWidget *btn_restore = gtk_button_new_from_icon_name("view-restore-symbolic");
    gtk_widget_set_tooltip_text(btn_restore, _("Leave Fullscreen"));
    gtk_widget_set_visible(btn_restore, FALSE); /* Hidden initially */
    gtk_actionable_set_action_name(GTK_ACTIONABLE(btn_restore), "win.fullscreen");
    win->fullscreen_restore_btn = btn_restore;
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), btn_restore);
    
    /* Progress Bar (positioned below header area, outside titlebar) */
    GtkWidget *prog = gtk_progress_bar_new();
    gtk_widget_set_visible(prog, FALSE);
    gtk_widget_add_css_class(prog, "header-progress");
    gtk_widget_set_size_request(prog, -1, 2);
    win->header_progress = prog;
    
    GtkWidget *title = adw_window_title_new("ViTE", NULL);
    win->window_title = ADW_WINDOW_TITLE(title);
    
    /* Header Spinner (initially hidden) */
    /* Header Spinner (initially hidden) */
    win->header_spinner = gtk_spinner_new();
    gtk_widget_set_visible(win->header_spinner, FALSE);
    
    /* Create a box to hold Spinner + Title centered */
    GtkWidget *title_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(title_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(title_box, GTK_ALIGN_CENTER);
    
    gtk_box_append(GTK_BOX(title_box), win->header_spinner);
    gtk_box_append(GTK_BOX(title_box), title);
    
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title_box);
    
    /* ACTIONS REGISTRATION */
    /* Map of all window actions including new ones */




    const GActionEntry win_entries[] = {
        { "new-window", on_new_window_action, NULL, NULL, NULL, { 0 } },
        { "new-tab", on_new_tab_action, NULL, NULL, NULL, { 0 } },
        { "split-right", on_split_right, NULL, NULL, NULL, { 0 } },
        { "split-down", on_split_down, NULL, NULL, NULL, { 0 } },
#ifdef HAVE_VTE
        { "terminal", on_terminal_toggle, NULL, NULL, NULL, { 0 } },
#endif
        { "close-view", on_close_split_action, NULL, NULL, NULL, { 0 } },
        { "close-tab", on_close_tab_action, NULL, NULL, NULL, { 0 } },
        { "reopen-closed-tab", on_reopen_closed_tab_action, NULL, NULL, NULL, { 0 } },
        { "quit-window", on_quit_window_action, NULL, NULL, NULL, { 0 } },
        { "preferences", on_preferences_action, NULL, NULL, NULL, { 0 } },
        { "goto-line", on_goto_line_action, NULL, NULL, NULL, { 0 } },
        { "find", on_find_action, NULL, NULL, NULL, { 0 } },
        { "replace", on_replace_action, NULL, NULL, NULL, { 0 } },
        { "save", on_save_action, NULL, NULL, NULL, { 0 } },
        { "save-as", on_save_as_action, NULL, NULL, NULL, { 0 } },
        { "discard-changes", on_discard_all_action, NULL, NULL, NULL, { 0 } },
        { "filter", on_filter_action, NULL, NULL, NULL, { 0 } },
        { "print", on_print_action, NULL, NULL, NULL, { 0 } },
        { "fullscreen", on_fullscreen_action, NULL, NULL, NULL, { 0 } },
        { "shortcuts", on_shortcuts_action, NULL, NULL, NULL, { 0 } },
        { "about", on_about_action, NULL, NULL, NULL, { 0 } },
        { "zoom-in", on_zoom_in_action, NULL, NULL, NULL, { 0 } },
        { "zoom-out", on_zoom_out_action, NULL, NULL, NULL, { 0 } },
        { "zoom-reset", on_zoom_reset_action, NULL, NULL, NULL, { 0 } },
        
        /* Stateful Actions */
        { "set-encoding", NULL, "s", "'utf-8'", on_set_encoding, { 0 } },
        { "set-line-ending", NULL, "s", "'lf'", on_set_line_ending, { 0 } },
        { "set-file-type", NULL, "s", "'plain'", on_set_file_type, { 0 } },
        { "show-line-numbers", NULL, NULL, "true", on_show_line_numbers_toggled, { 0 } },
        { "enable-word-wrap", NULL, NULL, "true", on_enable_word_wrap_toggled, { 0 } },
        { "enable-folding", NULL, NULL, "false", on_enable_folding_toggled, { 0 } },
        { "enable-minimap", NULL, NULL, "false", on_enable_minimap_toggled, { 0 } },
        { "show-status-bar", NULL, NULL, "true", on_show_status_bar_toggled, { 0 } },
        { "show-save-button", NULL, NULL, "true", on_show_save_button_toggled, { 0 } },
        { "toggle-insert-mode", on_toggle_insert_mode, NULL, NULL, NULL, { 0 } },
        { "select-all", on_select_all_action, NULL, NULL, NULL, { 0 } }
    };
    g_action_map_add_action_entries(G_ACTION_MAP(window), win_entries, G_N_ELEMENTS(win_entries), win);
    {
        GAction *reload_act = g_action_map_lookup_action(G_ACTION_MAP(window), "discard-changes");
        if (reload_act && G_IS_SIMPLE_ACTION(reload_act)) {
            g_simple_action_set_enabled(G_SIMPLE_ACTION(reload_act), FALSE);
        }
    }
    
    /* KEYBOARD SHORTCUTS */
    GtkShortcutController *shortcuts = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
    gtk_shortcut_controller_set_scope(shortcuts, GTK_SHORTCUT_SCOPE_GLOBAL);

    struct { guint key; GdkModifierType mods; const char *action; } keys[] = {
        { GDK_KEY_n, GDK_CONTROL_MASK, "win.new-window" },
        { GDK_KEY_t, GDK_CONTROL_MASK, "win.new-tab" },
        { GDK_KEY_s, GDK_CONTROL_MASK, "win.save" },
        { GDK_KEY_s, GDK_CONTROL_MASK | GDK_SHIFT_MASK, "win.save-as" },
        { GDK_KEY_f, GDK_CONTROL_MASK, "win.find" },
        { GDK_KEY_h, GDK_CONTROL_MASK, "win.replace" },
        { GDK_KEY_g, GDK_CONTROL_MASK, "win.goto-line" },
        { GDK_KEY_f, GDK_CONTROL_MASK | GDK_SHIFT_MASK, "win.filter" },
        { GDK_KEY_p, GDK_CONTROL_MASK, "win.print" },
        { GDK_KEY_w, GDK_CONTROL_MASK, "win.close-tab" },
        { GDK_KEY_t, GDK_CONTROL_MASK | GDK_SHIFT_MASK, "win.reopen-closed-tab" },
        { GDK_KEY_q, GDK_CONTROL_MASK, "win.quit-window" },
        { GDK_KEY_F5, 0, "win.discard-changes" },
        { GDK_KEY_F11, 0, "win.fullscreen" },
        { GDK_KEY_question, GDK_CONTROL_MASK, "win.shortcuts" },
        { GDK_KEY_comma, GDK_CONTROL_MASK, "win.preferences" },
        { GDK_KEY_plus, GDK_CONTROL_MASK, "win.zoom-in" },
        { GDK_KEY_equal, GDK_CONTROL_MASK, "win.zoom-in" },
        { GDK_KEY_KP_Add, GDK_CONTROL_MASK, "win.zoom-in" },
        { GDK_KEY_minus, GDK_CONTROL_MASK, "win.zoom-out" },
        { GDK_KEY_KP_Subtract, GDK_CONTROL_MASK, "win.zoom-out" },
        { GDK_KEY_0, GDK_CONTROL_MASK, "win.zoom-reset" },
        { GDK_KEY_KP_0, GDK_CONTROL_MASK, "win.zoom-reset" },
#ifdef HAVE_VTE
        { GDK_KEY_grave, GDK_CONTROL_MASK, "win.terminal" },
#endif
    };

    for (int i = 0; i < (int)G_N_ELEMENTS(keys); i++) {
        GtkShortcut *sc = gtk_shortcut_new(gtk_keyval_trigger_new(keys[i].key, keys[i].mods), 
                                           gtk_named_action_new(keys[i].action));
        gtk_shortcut_controller_add_shortcut(shortcuts, sc);
    }
    gtk_widget_add_controller(GTK_WIDGET(window), GTK_EVENT_CONTROLLER(shortcuts));
     
    /* FULLSCREEN MOTION CONTROLLER */
    /* Add motion controller to main_box (not created yet) or better, to the window itself? 
       Window captures all events. 
    */
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_fullscreen_hover_motion), win);
    gtk_widget_add_controller(GTK_WIDGET(window), motion);

    
    /* Open Split Button */
    GtkWidget *split_btn = adw_split_button_new();
    adw_split_button_set_label(ADW_SPLIT_BUTTON(split_btn), _("Open"));
    g_signal_connect(split_btn, "clicked", G_CALLBACK(on_open_btn_clicked), NULL);
    
    /* Custom Popover for Recent Files and Alignment */
    GtkWidget *popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
    
    /* Align popover to the left of the split button */
    gtk_widget_set_halign(popover, GTK_ALIGN_START);
    
    /* Point to bottom-left corner of the split button */
    int button_height = gtk_widget_get_height(split_btn);
    if (button_height <= 0) button_height = 34; /* Reasonable default if not yet allocated */
    GdkRectangle rect = { 0, button_height, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    
    GtkWidget *pop_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(pop_vbox, 6);
    gtk_widget_set_margin_bottom(pop_vbox, 6);
    gtk_popover_set_child(GTK_POPOVER(popover), pop_vbox);

    /* Search Entry */
    GtkWidget *search_entry = gtk_search_entry_new();
    gtk_widget_set_margin_start(search_entry, 6);
    gtk_widget_set_margin_end(search_entry, 6);
    gtk_widget_set_margin_bottom(search_entry, 6);
    g_object_set(search_entry, "placeholder-text", _("Search documents"), NULL);
    gtk_box_append(GTK_BOX(pop_vbox), search_entry);
    
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_search_key_pressed), popover);
    gtk_widget_add_controller(search_entry, key_ctrl);

    /* Scrolled Window */
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scrolled), 360);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), 400);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);
    gtk_widget_add_css_class(scrolled, "recent-list");
    gtk_box_append(GTK_BOX(pop_vbox), scrolled);
    
    GtkWidget *recent_list = gtk_list_box_new();
    gtk_widget_add_css_class(recent_list, "recent-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), recent_list);
    
    g_signal_connect(search_entry, "search-changed", G_CALLBACK(on_search_changed), recent_list);
    
    /* Right click gesture */
    GtkGesture *right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click, "pressed", G_CALLBACK(on_recent_context_menu), recent_list);
    gtk_widget_add_controller(recent_list, GTK_EVENT_CONTROLLER(right_click));

    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(window));
    g_signal_connect(recent_list, "row-activated", G_CALLBACK(on_recent_item_activated), app);
    
    /* Update list when popover is shown */
    g_signal_connect_swapped(popover, "map", G_CALLBACK(update_recent_files_list), recent_list);
    g_signal_connect_swapped(popover, "map", G_CALLBACK(gtk_list_box_unselect_all), recent_list);
    g_signal_connect(popover, "unmap", G_CALLBACK(on_recent_popover_unmap), search_entry);
    g_object_set_data(G_OBJECT(popover), "app", app); /* For the update function */

    adw_split_button_set_popover(ADW_SPLIT_BUTTON(split_btn), GTK_POPOVER(popover));
    
    /* Styling to look like single button by default */
    /* By default AdwSplitButton looks joined. We can add specific classes if needed. */
     gtk_widget_set_tooltip_text(split_btn, _("Open File"));
     gtk_widget_add_css_class(split_btn, "open-split-btn");
    /* Set tooltip for the dropdown arrow (if accessible) or just relied on general one. 
       AdwSplitButton documentation suggests "dropdown-tooltip" property. */
    adw_split_button_set_dropdown_tooltip(ADW_SPLIT_BUTTON(split_btn), "Recent Files");

    adw_header_bar_pack_start(ADW_HEADER_BAR(header), split_btn);
    
    /* New Tab (Icon Only) */
    GtkWidget *btn_new = gtk_button_new_from_icon_name("tab-new-symbolic");
    gtk_widget_set_tooltip_text(btn_new, _("New Tab"));
    g_signal_connect(btn_new, "clicked", G_CALLBACK(on_new_tab_clicked_header), window);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), btn_new);
    
    win->loading_count = 0;
    
    /* Main Menu Construction */
    GMenu *main_menu = g_menu_new();
    
    /* Group 1: New Window, etc. */
    g_menu_append(main_menu, _("New Window"), "win.new-window");
    
    /* Group 2: File Actions */
    GMenu *s_file = g_menu_new();
    g_menu_append(s_file, _("Save"), "win.save");
    g_menu_append(s_file, _("Save As…"), "win.save-as");
    g_menu_append(s_file, _("Reload"), "win.discard-changes");
    g_menu_append_section(main_menu, NULL, G_MENU_MODEL(s_file));
    g_object_unref(s_file);
    
    /* Group 3: Search Actions */
    GMenu *s_search = g_menu_new();
    g_menu_append(s_search, _("Filter Lines"), "win.filter");
    g_menu_append(s_search, _("Find/Replace"), "win.find");
    g_menu_append(s_search, _("Go to Line…"), "win.goto-line");
    g_menu_append_section(main_menu, NULL, G_MENU_MODEL(s_search));
    g_object_unref(s_search);
    
    /* Group 4: Submenus */
    GMenu *s_subs = g_menu_new();
    
    /* View Submenu */
    GMenu *view_menu = g_menu_new();
    g_menu_append(view_menu, _("Display Line Numbers"), "win.show-line-numbers");
    g_menu_append(view_menu, _("Enable Code Folding"), "win.enable-folding");
    g_menu_append(view_menu, _("Show Overview Map"), "win.enable-minimap");
    g_menu_append(view_menu, _("Word Wrap"), "win.enable-word-wrap");
    g_menu_append(view_menu, _("Show Status Bar"), "win.show-status-bar");
    g_menu_append(view_menu, _("Show Save Button"), "win.show-save-button");
#ifdef HAVE_VTE
    g_menu_append(view_menu, _("Terminal"), "win.terminal");
#endif
    
    GMenu *split_menu = g_menu_new();
    
    if (gtk_widget_get_default_direction() == GTK_TEXT_DIR_RTL) {
        g_menu_append(split_menu, _("Split Left"), "win.split-right");
        g_menu_append(split_menu, _("Split Down"), "win.split-down");
    } else {
        g_menu_append(split_menu, _("Split Right"), "win.split-right");
        g_menu_append(split_menu, _("Split Down"), "win.split-down");
    }
    g_menu_append(split_menu, _("Close View"), "win.close-view");
    g_menu_append_section(view_menu, NULL, G_MENU_MODEL(split_menu));
    g_object_unref(split_menu);
    g_menu_append_submenu(s_subs, _("View"), G_MENU_MODEL(view_menu));
    g_object_unref(view_menu);
    
    /* Document Submenu */
    GMenu *doc_menu = g_menu_new();
    
    GMenu *enc_menu = g_menu_new();
    for (int i = 0; i < file_encoding_get_count(); i++) {
        const char *disp = file_encoding_get_display_name_at(i);
        const char *id = file_encoding_get_id_at(i);
        if (!disp || !id) continue;
        char *detailed = g_strdup_printf("win.set-encoding::%s", id);
        g_menu_append(enc_menu, disp, detailed);
        g_free(detailed);
    }
    g_menu_append_submenu(doc_menu, _("Encoding"), G_MENU_MODEL(enc_menu));
    g_object_unref(enc_menu);
    
    GMenu *le_menu = g_menu_new();
    g_menu_append(le_menu, _("Unix/Linux (LF)"), "win.set-line-ending::lf");
    g_menu_append(le_menu, _("Windows (CRLF)"), "win.set-line-ending::crlf");
    g_menu_append(le_menu, _("Legacy Mac (CR)"), "win.set-line-ending::cr");
    g_menu_append_submenu(doc_menu, _("Line Ending"), G_MENU_MODEL(le_menu));
    g_object_unref(le_menu);

    GMenu *ft_menu = g_menu_new();
    g_menu_append(ft_menu, _("Plain Text"), "win.set-file-type::plain");
    g_menu_append(ft_menu, "C", "win.set-file-type::c");
    g_menu_append(ft_menu, "C++", "win.set-file-type::cpp");
    g_menu_append(ft_menu, "Python", "win.set-file-type::python");
    g_menu_append(ft_menu, "Bash", "win.set-file-type::bash");
    g_menu_append(ft_menu, "Rust", "win.set-file-type::rust");
    g_menu_append(ft_menu, _("Header"), "win.set-file-type::h");
    g_menu_append(ft_menu, "YAML", "win.set-file-type::yaml");
    g_menu_append(ft_menu, "JSON", "win.set-file-type::json");
    g_menu_append(ft_menu, "XML", "win.set-file-type::xml");
    g_menu_append(ft_menu, "JavaScript", "win.set-file-type::javascript");
    g_menu_append(ft_menu, _("Desktop Entry"), "win.set-file-type::desktop");
    g_menu_append_submenu(doc_menu, _("File Type"), G_MENU_MODEL(ft_menu));
    g_object_unref(ft_menu);

    g_menu_append_submenu(s_subs, _("Document"), G_MENU_MODEL(doc_menu));
    g_object_unref(doc_menu);
    
    g_menu_append_section(main_menu, NULL, G_MENU_MODEL(s_subs));
    g_object_unref(s_subs);
    
    /* Group 5: Print, Fullscreen */
    GMenu *s_extra = g_menu_new();
    g_menu_append(s_extra, _("Print"), "win.print");
    g_menu_append_section(main_menu, NULL, G_MENU_MODEL(s_extra));
    g_object_unref(s_extra);
    
    GMenu *s_fs = g_menu_new();
    g_menu_append(s_fs, _("Fullscreen"), "win.fullscreen");
    g_menu_append_section(main_menu, NULL, G_MENU_MODEL(s_fs));
    g_object_unref(s_fs);
    
    /* Group 6: App Info */
    GMenu *s_app = g_menu_new();
    g_menu_append(s_app, _("Keyboard Shortcuts"), "win.shortcuts");
    g_menu_append(s_app, _("About Virtual Text Editor"), "win.about");
    g_menu_append(s_app, _("Preferences"), "win.preferences");
    g_menu_append_section(main_menu, NULL, G_MENU_MODEL(s_app));
    g_object_unref(s_app);
    
    GtkWidget *btn_menu = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(btn_menu), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(btn_menu), G_MENU_MODEL(main_menu));
    gtk_widget_set_tooltip_text(btn_menu, _("Main Menu"));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), btn_menu);
    g_object_unref(main_menu);
    /* Save Button */
    GtkWidget *btn_save = gtk_button_new_with_label(_("Save"));
    gtk_widget_set_tooltip_text(btn_save, _("Save (Ctrl+S)"));
    gtk_actionable_set_action_name(GTK_ACTIONABLE(btn_save), "win.save");
    gtk_widget_add_css_class(btn_save, "header-button"); /* Use header button style */
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), btn_save);
    win->save_button = btn_save; /* Store reference to the save button */


    /* main_tab_bar = NULL; Removed */
    win->tab_bar = VITE_TAB_BAR(vite_tab_bar_new());
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(win->titlebar_container), GTK_WIDGET(win->tab_bar));

    /* Open Tabs Button (Overflow Menu) */
    GtkWidget *btn_tabs = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(btn_tabs), "pan-down-symbolic");
    gtk_widget_set_tooltip_text(btn_tabs, _("Open Tabs"));
    gtk_widget_set_visible(btn_tabs, FALSE); /* Hidden by default until overflow */
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), btn_tabs);

    /* Popover for tabs */
    GtkWidget *tabs_popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(tabs_popover), FALSE);
    gtk_popover_set_position(GTK_POPOVER(tabs_popover), GTK_POS_BOTTOM);
    GtkWidget *tabs_pop_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(tabs_pop_vbox, 6);
    gtk_widget_set_margin_bottom(tabs_pop_vbox, 6);
    gtk_popover_set_child(GTK_POPOVER(tabs_popover), tabs_pop_vbox);

    GtkWidget *tabs_search_entry = gtk_search_entry_new();
    gtk_widget_set_margin_start(tabs_search_entry, 6);
    gtk_widget_set_margin_end(tabs_search_entry, 6);
    gtk_widget_set_margin_bottom(tabs_search_entry, 6);
    g_object_set(tabs_search_entry, "placeholder-text", _("Search documents"), NULL);
    gtk_box_append(GTK_BOX(tabs_pop_vbox), tabs_search_entry);
    GtkEventController *tabs_key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(tabs_key_ctrl, "key-pressed", G_CALLBACK(on_search_key_pressed), tabs_popover);
    gtk_widget_add_controller(tabs_search_entry, tabs_key_ctrl);

    GtkWidget *tabs_list = gtk_list_box_new();
    GtkWidget *tabs_scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(tabs_scrolled), 300);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(tabs_scrolled), 400);
    gtk_widget_add_css_class(tabs_scrolled, "recent-list");
    gtk_widget_add_css_class(tabs_list, "recent-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tabs_scrolled), tabs_list);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(tabs_scrolled), TRUE);
    gtk_box_append(GTK_BOX(tabs_pop_vbox), tabs_scrolled);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(btn_tabs), tabs_popover);
    
    /* Check for overflow logic helper */
    g_object_set_data(G_OBJECT(win->tab_bar), "tabs-btn", btn_tabs);
    g_object_set_data(G_OBJECT(tabs_popover), "list", tabs_list);
    g_object_set_data(G_OBJECT(tabs_popover), "search-entry", tabs_search_entry);
    
    g_signal_connect(win->tab_bar, "overflow-changed", G_CALLBACK(on_overflow_changed), btn_tabs);
    g_signal_connect(win->tab_bar, "tab-dropped", G_CALLBACK(on_tab_dropped), win);
    g_signal_connect(tabs_list, "row-activated", G_CALLBACK(on_popover_tab_row_activated), tabs_list);
    g_signal_connect(tabs_search_entry, "search-changed", G_CALLBACK(on_search_changed), tabs_list);
    g_signal_connect(tabs_popover, "map", G_CALLBACK(update_open_tabs_list), tabs_list);
    g_signal_connect_swapped(tabs_popover, "map", G_CALLBACK(gtk_list_box_unselect_all), tabs_list);
    g_signal_connect(tabs_popover, "unmap", G_CALLBACK(on_recent_popover_unmap), tabs_search_entry);
    
    /* Monitor tab bar visibility to update header spinner */
    g_signal_connect(win->tab_bar, "notify::visible", G_CALLBACK(on_tab_bar_visible_changed), win);
    
    /* Main Overlay as Root */
    GtkOverlay *main_overlay = GTK_OVERLAY(gtk_overlay_new());
    win->main_overlay = main_overlay;
    g_object_set_data(G_OBJECT(window), "main-overlay", main_overlay);
    
    /* Main Content Box inside overlay */
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    win->main_box = main_box;
    gtk_overlay_set_child(main_overlay, main_box);
    
    /* Progress bar at the top */
    gtk_box_append(GTK_BOX(main_box), GTK_WIDGET(win->header_progress));
    
    /* Initialize Stack */
    win->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(win->stack, GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_widget_set_vexpand(GTK_WIDGET(win->stack), TRUE);
    gtk_box_append(GTK_BOX(main_box), GTK_WIDGET(win->stack));
    
    /* Status Bar in AdwToolbarView Bottom Bar */
    win->status_bar = vite_status_bar_new();
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(win->titlebar_container), win->status_bar);
    
    /* Connect signals */
    g_signal_connect(win->status_bar, "file-type-changed", G_CALLBACK(on_status_bar_file_type_changed), win);
    g_signal_connect(win->status_bar, "line-ending-changed", G_CALLBACK(on_status_bar_line_ending_changed), win);
    g_signal_connect(win->status_bar, "encoding-changed", G_CALLBACK(on_status_bar_encoding_changed), win);
    g_signal_connect(win->status_bar, "indent-width-changed", G_CALLBACK(on_status_bar_indent_width_changed), win);
    g_signal_connect(win->status_bar, "indent-style-changed", G_CALLBACK(on_status_bar_indent_style_changed), win);
    
    /* Set main_overlay as the AdwToolbarView's content for RAISED shadow effect */
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(win->titlebar_container), GTK_WIDGET(main_overlay));
    
    /* Make the entire content area draggable (where children don't handle it) */
    GtkWidget *window_handle = gtk_window_handle_new();
    gtk_window_handle_set_child(GTK_WINDOW_HANDLE(window_handle), GTK_WIDGET(win->titlebar_container));
    
    /* Set the toolbar view (which contains header + content) as the window content */
    adw_application_window_set_content(window, window_handle);
    
    /* Create Initial Tab if needed? 
       Actually activate() might do nothing? 
       Usually we open an execution tab or untitled? 
       Previous code created active_split but no tab? 
       Ah, `create_new_tab` is called elsewhere? 
       Wait, `activate` calls `setup_window` then creates an "Untitled"? 
       Checking activate...
    */
    
    return win;
}



static void
activate(GtkApplication *app, gpointer user_data G_GNUC_UNUSED)
{
    AdwApplicationWindow *window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    ViteWindow *win = setup_window(window);
    
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    
    Document *doc = document_new(NULL);
    create_new_tab(win, "Untitled", doc);
    document_free(doc);
    
    gtk_window_present(GTK_WINDOW(window));
}





static gboolean
free_load_context_idle(gpointer user_data)
{
    g_free(user_data);
    return G_SOURCE_REMOVE;
}

static void
on_load_progress(double progress, FileEncoding encoding, NewlineType newline, void *user_data)
{
    LoadContext *ctx = user_data;
    
    /* Update Status Bar immediately if we have valid window context */
    if (ctx->gtkw_ref && ctx->window && ctx->window->status_bar) {
        const char *enc_id = file_encoding_to_id(encoding);
        vite_status_bar_set_encoding(VITE_STATUS_BAR(ctx->window->status_bar), enc_id, FALSE);
        
        const char *nl_id = "lf";
        switch (newline) {
            case NEWLINE_CRLF: nl_id = "crlf"; break;
            case NEWLINE_CR: nl_id = "cr"; break;
            default: break;
        }
        vite_status_bar_set_line_ending(VITE_STATUS_BAR(ctx->window->status_bar), nl_id, FALSE);
    }
    
    /* Update Document State so that any other UI updates (e.g. on_cursor_moved) 
       don't overwrite our status bar with stale default values */
    if (ctx->doc) {
        document_set_encoding(ctx->doc, encoding);
        document_set_newline_type(ctx->doc, newline);
    }
    /* Determine visibility based on tab count */
    gboolean show_in_header = FALSE;
    if (ctx->tab_bar) {
        GList *tabs = vite_tab_bar_get_tabs(ctx->tab_bar);
        guint n_tabs = g_list_length(tabs);
        g_list_free(tabs);
        
        show_in_header = (n_tabs <= 1);
    }
    
    /* Update Tab Progress */
    if (ctx->tab) {
        vite_tab_set_progress(ctx->tab, progress);
        /* If showing in header, maybe hide tab progress? 
           User said: "when only 1 tab ... show progress in headerbar". 
           Implies NOT in tab? Or both?
           "then when tabs appear ... ONLY show progress in the tab".
           So 1 tab -> Header (maybe Tab too?)
           2+ tabs -> Tab ONLY (Hide Header).
           
           Let's interpret as:
           1 Tab: Header (+ Tab spinner is unobtrusive, keep it?)
           2+ Tabs: Tab ONLY (Hide Header).
           
           Actually, if I hide header progress, I should make sure it's 0 or hidden.
        */
    }
        
    /* Update Header Progress */
    if (ctx->header_progress) {
         if (show_in_header) {
             gtk_widget_set_visible(ctx->header_progress, TRUE);
             gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ctx->header_progress), progress);
         } else {
             gtk_widget_set_visible(ctx->header_progress, FALSE);
         }
    }
}

static void
on_load_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
    LoadContext *ctx = user_data;
    Document *doc = (Document*)source;
    GError *err = NULL;
    
    /* CRITICAL FIX: Clear the progress callback immediately. 
       This prevents any pending progress idles (scheduled at G_PRIORITY_HIGH_IDLE) 
       from running and accessing the 'ctx' pointer which we are about to free. 
       G_PRIORITY_DEFAULT (where this callback runs) is HIGHER priority (0) 
       than G_PRIORITY_HIGH_IDLE (100).
    */
    document_set_progress_callback(doc, NULL, NULL);

    gboolean success = document_load_file_finish(doc, res, &err);
    
    if (ctx->tab) {
        vite_tab_set_loading(ctx->tab, FALSE);
        vite_tab_close_active_dialog(ctx->tab);
        
        GtkWidget *page = g_object_get_data(G_OBJECT(ctx->tab), "page");
        GPtrArray *editors = find_all_editors_recursive(page);

        for (guint i = 0; i < editors->len; i++) {
            GtkWidget *ed = g_ptr_array_index(editors, i);
            gtk_widget_set_sensitive(ed, TRUE);
            if (success) {
                SyntaxContext *syntax_ctx = editor_widget_get_syntax_context(EDITOR_WIDGET(ed));
                if (syntax_ctx) {
                    syntax_context_invalidate_all(syntax_ctx);
                }
                editor_widget_clear_search(EDITOR_WIDGET(ed));
                editor_widget_rebuild_folding(EDITOR_WIDGET(ed));

                /* Record the original values of the newly loaded file */
                FileEncoding enc = document_get_encoding(doc);
                const char *enc_id = file_encoding_to_id(enc);
                
                NewlineType nl = document_get_newline_type(doc);
                const char *nl_id = "lf";
                if (nl == NEWLINE_CRLF) nl_id = "crlf";
                else if (nl == NEWLINE_CR) nl_id = "cr";

                const char *lang_name = editor_widget_get_language_name(EDITOR_WIDGET(ed));
                if (!lang_name) lang_name = "Plain Text";

                int tab_w = 4;
                int indent_s = 0;
                g_object_get(G_OBJECT(ed), "tab-width", &tab_w, "indent-style", &indent_s, NULL);

                g_object_set_data_full(G_OBJECT(ed), "original-encoding", g_strdup(enc_id), g_free);
                g_object_set_data_full(G_OBJECT(ed), "original-newline", g_strdup(nl_id), g_free);
                g_object_set_data_full(G_OBJECT(ed), "original-file-type", g_strdup(lang_name), g_free);
                g_object_set_data(G_OBJECT(ed), "original-indent-width", GINT_TO_POINTER(tab_w));
                g_object_set_data(G_OBJECT(ed), "original-indent-style", GINT_TO_POINTER(indent_s));
            }
        }
        g_ptr_array_unref(editors);
        
        if (success) {
            const char *loaded_path = document_get_file_path(doc);
            if (loaded_path && *loaded_path) {
                char *basename = g_path_get_basename(loaded_path);
                vite_tab_set_title(ctx->tab, basename);
                g_free(basename);
            }

            if (ctx->is_reload) {
                GtkWidget *view_container = gtk_widget_get_first_child(page);
                GtkWidget *revealer = view_container ? g_object_get_data(G_OBJECT(view_container), "external_change_revealer") : NULL;
                if (revealer && GTK_IS_REVEALER(revealer)) {
                    gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
                }
            }
            update_window_title_for_tab(ctx->tab);

            /* Refresh status bar (Encoding/Line Endings known now) */
            if (vite_tab_is_active(ctx->tab)) {
                on_tab_clicked(ctx->tab, NULL);
            }
             
        } else {
            if (ctx->is_reload && ctx->doc && ctx->has_prev_format) {
                document_set_encoding(ctx->doc, ctx->prev_encoding);
                document_set_newline_type(ctx->doc, ctx->prev_newline);
            }

            if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                if (!ctx->is_reload) {
                    /* Revert title on cancellation */
                    vite_tab_restore_original_title(ctx->tab);
                    update_window_title_for_tab(ctx->tab);

                    /* Revert Encoding/Newline to defaults (since load failed/cancelled) */
                    if (ctx->doc) {
                        document_set_encoding(ctx->doc, ENCODING_UTF8);
                        document_set_newline_type(ctx->doc, NEWLINE_LF);
                    }
                }

                if (vite_tab_is_active(ctx->tab)) {
                    on_tab_clicked(ctx->tab, NULL);
                }
            } else {
                /* Show error dialog */
                GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(ctx->tab));
                if (root) {
                    const char *title = ctx->is_reload ? _("Failed to reload file") : _("Failed to open file");
                    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(title, err->message));
                    adw_alert_dialog_add_response(dialog, "ok", "OK");
                    adw_alert_dialog_choose(dialog, GTK_WIDGET(root), NULL, NULL, NULL);
                }
            }
        }
        
        vite_tab_set_operation_type(ctx->tab, VITE_OP_NONE);
        
        if (vite_tab_get_close_when_done(ctx->tab)) {
            g_signal_emit_by_name(ctx->tab, "close-clicked");
        }
    }
    
    if (ctx->header_progress) {
         gtk_widget_set_visible(ctx->header_progress, FALSE);
         g_object_remove_weak_pointer(G_OBJECT(ctx->header_progress), (gpointer *)&ctx->header_progress);
         ctx->header_progress = NULL;
    }

    if (ctx->header_spinner) {
         /* Just remove weak pointer, logic handled by window counter */
         g_object_remove_weak_pointer(G_OBJECT(ctx->header_spinner), (gpointer *)&ctx->header_spinner);
         ctx->header_spinner = NULL;
    }
    
    /* ctx->tab_bar is weak ref. */
    if (ctx->tab_bar) {
        GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(ctx->tab_bar));
        if (root && GTK_IS_WIDGET(root)) {
            ViteWindow *win = g_object_get_data(G_OBJECT(root), "vite-window");
            if (win && win->loading_count > 0) {
                win->loading_count--;
                update_header_spinner(win);
            }
        }
    }
    
    if (ctx->tab) {
        g_object_remove_weak_pointer(G_OBJECT(ctx->tab), (gpointer *)&ctx->tab);
        ctx->tab = NULL;
    }
    
    if (ctx->doc) {
        document_free(ctx->doc);
        ctx->doc = NULL;
    }
    
    if (ctx->tab_bar) {
        g_object_remove_weak_pointer(G_OBJECT(ctx->tab_bar), (gpointer *)&ctx->tab_bar);
        ctx->tab_bar = NULL;
    }
    
    if (err) g_error_free(err);
    g_free(ctx->filename);
    ctx->filename = NULL;
    
    /* Defer freeing ctx to allow pending idle progress callbacks to complete safely */
    g_idle_add(free_load_context_idle, ctx);

    /* Process queued files only for open-file operations */
    if (!ctx->is_reload) {
        trigger_process_next_file();
    }
}

static gboolean
open_file(GtkApplication *app, ViteWindow *target_window, GFile *file, gboolean allow_reuse)
{
    char *path = g_file_get_path(file);
    if (!path) return FALSE;

    /* GLOBAL CHECK: Iterate ALL windows to find if file is already open */
    GList *all_windows = gtk_application_get_windows(app);
    for (GList *w_iter = all_windows; w_iter != NULL; w_iter = w_iter->next) {
        GtkWidget *w_widget = GTK_WIDGET(w_iter->data);
        ViteWindow *check_win = g_object_get_data(G_OBJECT(w_widget), "vite-window");
        if (!check_win || !check_win->tab_bar) continue;

        GList *tabs = vite_tab_bar_get_tabs(check_win->tab_bar);
        for (GList *l = tabs; l != NULL; l = l->next) {
            ViteTab *tab = VITE_TAB(l->data);
            GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
            GtkWidget *editor = get_editor_from_page(page);
            
            if (EDITOR_IS_WIDGET(editor)) {
                Document *d = editor_widget_get_document(EDITOR_WIDGET(editor));
                const char *p = document_get_file_path(d);
                if (g_strcmp0(path, p) == 0) {
                    /* FOUND! Switch to this window and tab */
                    on_tab_clicked(tab, NULL);
                    gtk_window_present(GTK_WINDOW(check_win->window));
                    
                    /* Also add to recents to bump it up */
                    char *uri = g_file_get_uri(file);
                    add_to_local_recents(uri);
                    g_free(uri);
                            
                    g_free(path);
                    g_list_free(tabs);
                    return FALSE;
                }
            }
        }
        g_list_free(tabs);
    }

    /* Not found globally. Proceed to open in target_window. */
    if (!target_window) {
        /* Fallback: Use most active or create new if not provided */
        GtkWindow *active = gtk_application_get_active_window(app);
        if (active) {
             target_window = g_object_get_data(G_OBJECT(active), "vite-window");
        }
        
        if (!target_window && all_windows) {
             GtkWidget *w = GTK_WIDGET(g_list_last(all_windows)->data);
             target_window = g_object_get_data(G_OBJECT(w), "vite-window");
        }

        if (!target_window) {
            AdwApplicationWindow *window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
            target_window = setup_window(window);
            gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
            gtk_window_present(GTK_WINDOW(window));
        }
    }
        
    /* Ensure window is presented (raised/focused) even if reused */
    if (target_window && target_window->window) {
        gtk_window_present(GTK_WINDOW(target_window->window));
    }
        
    /* Check if we can reuse the active tab in TARGET window (Untitled & Unmodified) */
    gboolean reused = FALSE;
    ViteTab *reused_tab = NULL;
    
    if (allow_reuse) {
             ViteTab *tab = vite_tab_bar_get_active_tab(target_window->tab_bar);
             if (tab) {
                 GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
                 GtkWidget *editor = get_editor_from_page(page);
                 if (EDITOR_IS_WIDGET(editor)) {
                           Document *current_doc = editor_widget_get_document(EDITOR_WIDGET(editor));
                           /* Check if strictly untitled (no path), unmodified, and NOT loading */
                           if (!document_get_file_path(current_doc) && 
                               !document_is_modified(current_doc) && 
                               !vite_tab_is_loading(tab)) {
                               /* Reuse this tab */
                               reused = TRUE;
                               reused_tab = tab;
                           }
                 }
             }
    }

    Document *doc = NULL;
    ViteTab *tab_to_use = NULL;
    char *basename = g_file_get_basename(file); // Allocate basename here

    if (reused && reused_tab) {
        GtkWidget *page = g_object_get_data(G_OBJECT(reused_tab), "page");
        GtkWidget *view_container = gtk_widget_get_first_child(page);
        
        doc = document_new_empty();
        
        /* Disconnect old doc from tab before replacing */
        GtkWidget *editor = get_editor_from_page(page);
        if (EDITOR_IS_WIDGET(editor)) {
            Document *old_doc = editor_widget_get_document(EDITOR_WIDGET(editor));
            if (old_doc) {
                document_remove_modification_callback(old_doc, on_document_modified, reused_tab);
                document_remove_content_callback(old_doc, on_document_content_changed, reused_tab);
            }
        }
        
        page_set_document(page, doc);
        
        /* Update tab callbacks for new doc */
        document_add_modification_callback(doc, on_document_modified, reused_tab);
        document_add_content_callback(doc, on_document_content_changed, reused_tab);
        if (view_container) {
            document_add_external_change_callback(doc, on_document_changed_externally, view_container);
        }
        
        tab_to_use = reused_tab;
    } else {
        doc = document_new_empty();
        create_new_tab(target_window, "Untitled", doc);
        tab_to_use = vite_tab_bar_get_active_tab(target_window->tab_bar);
    }
    
    /* Capture original title (Untitled X) before renaming */
    if (tab_to_use) {
        vite_tab_set_operation_type(tab_to_use, VITE_OP_LOADING);
        vite_tab_set_loading(tab_to_use, TRUE);
        vite_tab_set_title(tab_to_use, basename);
    }
    
    /* Set language based on extension immediately */
    GtkWidget *page = g_object_get_data(G_OBJECT(tab_to_use), "page");
    GPtrArray *editors = find_all_editors_recursive(page);
    const char *dot = strrchr(path, '.');
    gboolean lang_set = FALSE;
    const char *selected_lang_id = NULL;  // Track which language was set
    
    if (dot) {
        const char *ext = dot + 1;
             if (g_ascii_strcasecmp(ext, "c") == 0) selected_lang_id = "c";
        else if (g_ascii_strcasecmp(ext, "cpp") == 0 || g_ascii_strcasecmp(ext, "cc") == 0) selected_lang_id = "c";
        else if (g_ascii_strcasecmp(ext, "h") == 0) selected_lang_id = "c";
        else if (g_ascii_strcasecmp(ext, "py") == 0) selected_lang_id = "python";
        else if (g_ascii_strcasecmp(ext, "sh") == 0 || g_ascii_strcasecmp(ext, "bash") == 0) selected_lang_id = "bash";
        else if (g_ascii_strcasecmp(ext, "js") == 0 || g_ascii_strcasecmp(ext, "ts") == 0) selected_lang_id = "javascript";
        else if (g_ascii_strcasecmp(ext, "json") == 0) selected_lang_id = "json";
        else if (g_ascii_strcasecmp(ext, "yaml") == 0 || g_ascii_strcasecmp(ext, "yml") == 0) selected_lang_id = "yaml";
        else if (g_ascii_strcasecmp(ext, "xml") == 0 || g_ascii_strcasecmp(ext, "html") == 0 || g_ascii_strcasecmp(ext, "svg") == 0 || g_ascii_strcasecmp(ext, "xsl") == 0) selected_lang_id = "xml";
        else if (g_ascii_strcasecmp(ext, "desktop") == 0) selected_lang_id = "desktop";
        else if (g_ascii_strcasecmp(ext, "rs") == 0) selected_lang_id = "rust";
        else if (g_ascii_strcasecmp(ext, "md") == 0 || g_ascii_strcasecmp(ext, "markdown") == 0 || g_ascii_strcasecmp(ext, "mkd") == 0) selected_lang_id = "markdown";
        
        if (selected_lang_id) lang_set = TRUE;
    }

    /* Keep unknown extensions as Plain Text; only try content heuristics
     * for files without an extension (e.g. shebang scripts). */
    if (!lang_set && !dot) {
        /* Fallback: Content Detection (First 1KB) */
        char sample[1024];
        GFileInputStream *in_stream = g_file_read(file, NULL, NULL);
        if (in_stream) {
            gssize bytes = g_input_stream_read(G_INPUT_STREAM(in_stream), sample, 1023, NULL, NULL);
            if (bytes > 0) {
                sample[bytes] = '\0';
                selected_lang_id = syntax_detect_language(sample);
                if (selected_lang_id) lang_set = TRUE;
            }
            g_object_unref(in_stream);
        }
    }

    if (lang_set && selected_lang_id) {
        for (guint i = 0; i < editors->len; i++) {
            editor_widget_set_language(EDITOR_WIDGET(g_ptr_array_index(editors, i)), selected_lang_id);
        }
    }

    /* Update the action state to reflect the selected language */
    if (lang_set && selected_lang_id && target_window && target_window->window) {
        GAction *action = g_action_map_lookup_action(G_ACTION_MAP(target_window->window), "set-file-type");
        if (action && G_IS_SIMPLE_ACTION(action)) {
            g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_string(selected_lang_id));
        }

        /* Also update the status bar to show the selected language */
        const char *display_name = "Plain Text";  // Default
        for (int i = 0; file_types[i].name != NULL; i++) {
            if (g_strcmp0(file_types[i].id, selected_lang_id) == 0) {
                display_name = file_types[i].name;
                break;
            }
        }
        if (target_window->status_bar) {
            if (g_strcmp0(display_name, "Plain Text") == 0) {
                vite_status_bar_set_file_type(VITE_STATUS_BAR(target_window->status_bar), NULL, FALSE);
            } else {
                vite_status_bar_set_file_type(VITE_STATUS_BAR(target_window->status_bar), _(display_name), FALSE);
            }
        }
    }
    
    if (lang_set && vite_tab_is_active(tab_to_use)) {
        on_tab_clicked(tab_to_use, NULL);
    }

    /* Setup Async Load */
    /* Loading set earlier to capture title */
    for (guint i = 0; i < editors->len; i++) {
        GtkWidget *ed = g_ptr_array_index(editors, i);
        gtk_widget_set_sensitive(ed, FALSE);
    }
    
    target_window->loading_count++;
    update_header_spinner(target_window);
    
    /* Force title update now that loading state is active (prevent "Virtual Text Editor" override) */
    update_window_title_for_tab(tab_to_use);
    
    LoadContext *ctx = g_new0(LoadContext, 1);
    ctx->tab = tab_to_use;
    ctx->tab_bar = target_window->tab_bar;
    /* We don't need header_progress weak ref anymore if we just use window logic, 
       BUT on_load_progress uses it. Let's keep it for progress bar, 
       but for Spinner we use window logic. */
    ctx->header_progress = target_window->header_progress;
     ctx->header_progress = target_window->header_progress;
    ctx->header_spinner = target_window->header_spinner; /* Still keep weak ref for safety in callback? */
    ctx->filename = g_strdup(path);
    ctx->window = target_window;
    ctx->gtkw_ref = GTK_WINDOW(target_window->window);
    ctx->doc = document_ref(doc);
    ctx->is_reload = FALSE;
    
    /* Weak references for safety */
    g_object_add_weak_pointer(G_OBJECT(tab_to_use), (gpointer *)&ctx->tab);
    if (ctx->tab_bar) {
        g_object_add_weak_pointer(G_OBJECT(ctx->tab_bar), (gpointer *)&ctx->tab_bar);
    }
    if (ctx->header_progress) {
        g_object_add_weak_pointer(G_OBJECT(ctx->header_progress), (gpointer *)&ctx->header_progress);
    }
     if (ctx->header_spinner) {
        g_object_add_weak_pointer(G_OBJECT(ctx->header_spinner), (gpointer *)&ctx->header_spinner);
    }
    if (ctx->gtkw_ref) {
        g_object_add_weak_pointer(G_OBJECT(ctx->gtkw_ref), (gpointer *)&ctx->gtkw_ref);
    }
    
    document_set_progress_callback(doc, on_load_progress, ctx);
    
    GCancellable *cancellable = g_cancellable_new();
    vite_tab_set_cancellable(tab_to_use, cancellable);
    
    document_load_file_async(doc, path, cancellable, on_load_complete, ctx);
    g_object_unref(cancellable);
    
    char *uri = g_file_get_uri(file);
    add_to_local_recents(uri);
    g_free(uri);
    g_free(path);
    g_free(basename);
    g_ptr_array_unref(editors);
    
    if (doc) {
        document_free(doc);
    }
    
    return TRUE;
}

static gboolean
process_next_pending_file_idle(gpointer user_data G_GNUC_UNUSED)
{
    if (!pending_open_files || g_queue_is_empty(pending_open_files)) {
        is_opening = FALSE;
        return G_SOURCE_REMOVE;
    }

    /* Process one file per idle frame */
    QueuedFile *qf = g_queue_pop_head(pending_open_files);
    gboolean async_started = FALSE;
    
    if (qf) {
        if (qf->file) {
            if (qf->app) {
                async_started = open_file(qf->app, qf->target_window, qf->file, qf->allow_reuse);
            }
            g_object_unref(qf->file);
        }
        g_free(qf);
    }

    if (!async_started) {
        /* If no async load was started, try the next file immediately */
        return G_SOURCE_CONTINUE;
    }

    /* Async load started. We will process the next file when on_load_complete fires. */
    return G_SOURCE_REMOVE;
}

static void
trigger_process_next_file(void)
{
    g_idle_add(process_next_pending_file_idle, NULL);
}

static void
queue_open_file(GtkApplication *app, ViteWindow *target_window, GFile *file, gboolean allow_reuse)
{
    if (!pending_open_files) {
        pending_open_files = g_queue_new();
    }
    
    QueuedFile *qf = g_new0(QueuedFile, 1);
    qf->app = app;
    qf->target_window = target_window;
    qf->file = g_object_ref(file);
    qf->allow_reuse = allow_reuse;
    
    g_queue_push_tail(pending_open_files, qf);
    
    if (!is_opening) {
        is_opening = TRUE;
        trigger_process_next_file();
    }
}

static void
on_open(GtkApplication *app, GFile **files, int n_files, char *hint G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    /* Ensure at least one window exists before returning, 
     * otherwise GApplication will immediately shut down since loading is async. */
    GList *windows = gtk_application_get_windows(app);
    if (!windows) {
        AdwApplicationWindow *window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
        setup_window(window);
        gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
        gtk_window_present(GTK_WINDOW(window));
    }

    for (int i = 0; i < n_files; i++) {
        queue_open_file(app, NULL, files[i], TRUE);
    }
}


static void
on_app_startup(GApplication *app, gpointer user_data)
{
    (void)app;
    (void)user_data;
    theme_manager_init();
}

int
main(int argc, char **argv)
{
    AdwApplication *app;
    int status;

    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C"); /* Prevent issues with float parsing in drivers/libs */
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    app = adw_application_new ("io.github.fastrizwaan.ViTE", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "startup", G_CALLBACK(on_app_startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
#ifdef HAVE_VTE
    if (terminal_theme_changed_id != 0) {
        theme_manager_remove_changed_callback(terminal_theme_changed_id);
        terminal_theme_changed_id = 0;
    }
    if (terminal_trackers) {
        g_ptr_array_unref(terminal_trackers);
        terminal_trackers = NULL;
    }
#endif
    theme_manager_cleanup();
    g_object_unref(app);
    return status;
}
static GtkWidget *
get_active_overlay(ViteWindow *win) {
    /* Try focused widget first */
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(win->window));
    GtkWidget *iter = focus;
    while (iter) {
        if (gtk_widget_has_css_class(iter, "view-split")) return iter;
        iter = gtk_widget_get_parent(iter);
    }
    
    /* Fallback to active tab */
    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    if (tab) {
         GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
         if (page) {
             /* Check if page itself is the view (unlikely if wrapped in TabRoot) */
             if (gtk_widget_has_css_class(page, "view-split")) return page;
             
             /* Check last focused child in tab */
             GtkWidget *last = vite_tab_get_last_focused_child(tab);
             if (last) {
                 GtkWidget *vc = last;
                 while (vc && !gtk_widget_has_css_class(vc, "view-split") && vc != page) {
                     vc = gtk_widget_get_parent(vc);
                 }
                 if (vc && gtk_widget_has_css_class(vc, "view-split")) return vc;
             }
             
             /* Scan for first view-split */
             /* We can use find_first_editor_recursive and walk up */
             GtkWidget *ed = find_first_editor_recursive(page);
             if (ed) {
                 GtkWidget *vc = gtk_widget_get_parent(gtk_widget_get_parent(ed)); /* Editor -> Overlay -> RootBox? Safe? */
                 /* Safer walking */
                 vc = gtk_widget_get_ancestor(ed, GTK_TYPE_BOX); /* Assuming RootBox is a Box */
                 while (vc && !gtk_widget_has_css_class(vc, "view-split") && vc != page) {
                     vc = gtk_widget_get_parent(vc);
                 }
                 if (vc && gtk_widget_has_css_class(vc, "view-split")) return vc;
             }
         }
    }
    return NULL;
}

static void on_find_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data) {
    ViteWindow *win = (ViteWindow *)user_data;
    GtkWidget *overlay = get_active_overlay(win);
    if (!overlay) return;
    
    GtkWidget *bar = g_object_get_data(G_OBJECT(overlay), "find_bar");
    if (bar) {
        /* Get editor and check for selection */
        GtkWidget *scrolled = get_scrolled_window_from_view(overlay);
        if (scrolled && GTK_IS_SCROLLED_WINDOW(scrolled)) {
            GtkWidget *editor = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled));
            if (EDITOR_IS_WIDGET(editor)) {
                char *selection = editor_widget_get_selected_text(EDITOR_WIDGET(editor));
                if (selection) {
                    vite_find_replace_bar_set_search_text(VITE_FIND_REPLACE_BAR(bar), selection);
                    g_free(selection);
                }
            }
        }
        vite_find_replace_bar_show(VITE_FIND_REPLACE_BAR(bar));
    }
}

static void on_replace_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data) {
    ViteWindow *win = (ViteWindow *)user_data;
    GtkWidget *overlay = get_active_overlay(win);
    if (!overlay) return;
    
    GtkWidget *bar = g_object_get_data(G_OBJECT(overlay), "find_bar");
    if (bar) {
        gboolean has_selection = FALSE;
        
        /* Get editor and check for selection */
        GtkWidget *scrolled = get_scrolled_window_from_view(overlay);
        if (scrolled && GTK_IS_SCROLLED_WINDOW(scrolled)) {
            GtkWidget *editor = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled));
            if (EDITOR_IS_WIDGET(editor)) {
                char *selection = editor_widget_get_selected_text(EDITOR_WIDGET(editor));
                if (selection) {
                    vite_find_replace_bar_set_search_text(VITE_FIND_REPLACE_BAR(bar), selection);
                    g_free(selection);
                    has_selection = TRUE;
                }
            }
        }
        
        /* Show replace bar, focus depends on whether we had selection */
        vite_find_replace_bar_show_replace(VITE_FIND_REPLACE_BAR(bar), has_selection);
    }
}

/* ============================================================================
 * Async Save Logic
 * ============================================================================ */

typedef struct {
    DocumentSaveTask *task;
    char *original_path;
    GWeakRef tab_ref; /* Weak ref to ViteTab to prevent crashes if tab is closed */
    GWeakRef window_ref; /* Weak ref to GtkWindow to prevent crashes if window is closed */
    /* Progress Reporting */
    GMainContext *context;
    GSource *idle_source;
    /* Lossy encoding conversion flag: set if any chars were replaced with '?' */
    gboolean had_lossy_conversion;
} SaveWorkerData;

/* Helpers for progress */
typedef struct {
    SaveWorkerData *data;
    double progress;
} ProgressInfo;

/* Progress callback on Main Thread */
static gboolean
on_save_progress_idle(gpointer user_data)
{
    ProgressInfo *p = user_data;
    SaveWorkerData *data = p->data;
    
    /* Update UI safely */
    ViteTab *tab = g_weak_ref_get(&data->tab_ref);
    if (tab) {
         vite_tab_set_progress(tab, p->progress);
         g_object_unref(tab);
    }
    
    GtkWindow *window = g_weak_ref_get(&data->window_ref);
    if (window) {
        ViteWindow *win = g_object_get_data(G_OBJECT(window), "vite-window");
        gboolean show_in_header = TRUE;
        
        if (win && win->tab_bar) {
             if (vite_tab_bar_get_n_tabs(win->tab_bar) > 1) {
                 show_in_header = FALSE;
             }
        }
        
        if (win && win->header_progress) {
            if (show_in_header) {
                gtk_widget_set_visible(win->header_progress, TRUE);
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(win->header_progress), p->progress);
            } else {
                gtk_widget_set_visible(win->header_progress, FALSE);
            }
        }
        g_object_unref(window);
    }
    
    g_free(p);
    return G_SOURCE_REMOVE;
}

#if 0
/* Worker Thread */
static void
save_file_worker(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    /* Unused in this implementation, wrapper below is used */
}
#endif

static void
save_worker_wrapper(GTask *task, gpointer source_object G_GNUC_UNUSED, gpointer task_data, GCancellable *cancellable)
{
    SaveWorkerData *data = task_data;
    DocumentSaveTask *save_task = data->task;
    GError *error = NULL;
    gboolean done = FALSE;
    gint64 last_report = 0;
    
    while (!done && !g_cancellable_is_cancelled(cancellable)) {
        double progress = 0.0;
        /* Step for 18ms - Slightly larger chunks for faster saves */
        done = document_save_async_step(save_task, 20 * 1000, &progress);
        
        /* Throttle: Sleep 45ms (work:idle ~ 4:10) */
        g_usleep(500); 
        
        gint64 now = g_get_monotonic_time();
        if (now - last_report > 100 * 1000) { /* 100ms updates */
            ProgressInfo *pi = g_new(ProgressInfo, 1);
            pi->data = data;
            pi->progress = progress;
            g_main_context_invoke(data->context, on_save_progress_idle, pi);
            last_report = now;
        }
    }
    
    if (g_cancellable_is_cancelled(cancellable)) {
        document_save_async_cancel(save_task);
        document_save_async_finish(save_task, NULL); /* Clean up leaked .vite_save_XXXXXX */
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled");
    } else {
        /* Query lossy BEFORE finish() frees the task */
        data->had_lossy_conversion = document_save_async_had_lossy(save_task);
        document_save_async_finish(save_task, &error);
        if (error) {
            g_task_return_error(task, error);
        } else {
            g_task_return_boolean(task, TRUE); /* Success */
        }
    }
}

static void
on_save_complete(GObject *source G_GNUC_UNUSED, GAsyncResult *res, gpointer user_data)
{
    SaveWorkerData *data = user_data;
    GError *error = NULL;
    
    g_task_propagate_boolean(G_TASK(res), &error);
    
    /* UI Cleanup */
    ViteTab *tab = g_weak_ref_get(&data->tab_ref);
    gboolean saved_tab_is_active = FALSE;
    if (tab) {
        vite_tab_set_loading(tab, FALSE);
        vite_tab_set_operation_type(tab, VITE_OP_NONE);
        vite_tab_close_active_dialog(tab);
        
        vite_tab_set_progress(tab, 0.0);
        
        GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
        GPtrArray *editors = find_all_editors_recursive(page);
        GtkWidget *first_editor = NULL;
        Document *doc = NULL;
        
        for (guint i = 0; i < editors->len; i++) {
            GtkWidget *ed = g_ptr_array_index(editors, i);
            gtk_widget_set_sensitive(ed, TRUE);
            if (i == 0) {
                first_editor = ed;
            }
            if (i == 0) {
                doc = editor_widget_get_document(EDITOR_WIDGET(ed));
            }
        }
        g_ptr_array_unref(editors);
        
        if (!error && doc) {
             const char *new_path = document_get_file_path(doc);
             if (new_path) {
                 char *basename = g_path_get_basename(new_path);
                 vite_tab_set_title(tab, basename);
                 g_free(basename);
                 g_object_set_data_full(G_OBJECT(tab), "original_title",
                                       g_path_get_basename(new_path), g_free);
                 update_window_title_for_tab(tab);
                 
                 char *uri = g_filename_to_uri(new_path, NULL, NULL);
                 if (uri) {
                     add_to_local_recents(uri);
                     g_free(uri);
                 }
             }
         }

        {
            GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(tab));
            ViteWindow *win_for_tab = root ? g_object_get_data(G_OBJECT(root), "vite-window") : NULL;
            if (win_for_tab && win_for_tab->tab_bar) {
                saved_tab_is_active = (vite_tab_bar_get_active_tab(win_for_tab->tab_bar) == tab);
                if (saved_tab_is_active) {
                    update_reload_action_enabled(win_for_tab, first_editor);
                }
            }
        }
         
         if (vite_tab_get_close_when_done(tab)) {
             if (!error) {
                 g_signal_emit_by_name(tab, "close-clicked", NULL);
             } else {
                 vite_tab_set_close_when_done(tab, FALSE);
             }
         }
         
        g_object_unref(tab);
    }
    
    GtkWindow *window = g_weak_ref_get(&data->window_ref);
    if (window) {
         ViteWindow *win = g_object_get_data(G_OBJECT(window), "vite-window");
         if (win) {
             if (win->header_progress) {
                 gtk_widget_set_visible(win->header_progress, FALSE);
             }
             if (win->loading_count > 0) {
                 win->loading_count--;
                 update_header_spinner(win);
             }
             
             /* Restore focus if the current tab is the one we saved */
             ViteTab *current_tab = vite_tab_bar_get_active_tab(win->tab_bar);
             ViteTab *saved_tab = g_weak_ref_get(&data->tab_ref);
             
                 if (saved_tab) {
                     if (current_tab == saved_tab) {
                         if (!saved_tab_is_active) {
                             GtkWidget *saved_page = g_object_get_data(G_OBJECT(saved_tab), "page");
                             GtkWidget *saved_editor = saved_page ? get_editor_from_page(saved_page) : NULL;
                             update_reload_action_enabled(win, saved_editor);
                         }
                          GtkWidget *active_editor = get_active_editor(win);
                          if (active_editor && gtk_widget_get_visible(active_editor) && gtk_widget_get_sensitive(active_editor)) {
                              gtk_widget_grab_focus(active_editor);
                          }
                 }
                 g_object_unref(saved_tab);
             }

             if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                 GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(win->window));
                 if (root) {
                     AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Save Failed"), error->message));
                     adw_alert_dialog_add_response(dialog, "ok", _("OK"));
                     adw_alert_dialog_choose(dialog, GTK_WIDGET(root), NULL, NULL, NULL);
                 }
             } else if (!error && data->had_lossy_conversion) {
                 /* File was saved, but some characters couldn't be represented in
                    the target encoding and were replaced with '?'. Warn the user. */
                 GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(win->window));
                 if (root) {
                     const char *encoding_name = "the target encoding";
                     /* Try to get encoding display name from the saved document */
                     ViteTab *saved_tab_for_enc = g_weak_ref_get(&data->tab_ref);
                     if (saved_tab_for_enc) {
                         GtkWidget *page_for_enc = g_object_get_data(G_OBJECT(saved_tab_for_enc), "page");
                         GtkWidget *ed_for_enc = find_first_editor_recursive(page_for_enc);
                         if (ed_for_enc && EDITOR_IS_WIDGET(ed_for_enc)) {
                             Document *d_for_enc = editor_widget_get_document(EDITOR_WIDGET(ed_for_enc));
                             if (d_for_enc) {
                                 FileEncoding fe = document_get_encoding(d_for_enc);
                                 const char *dn = file_encoding_to_display_name(fe);
                                 if (dn && *dn) encoding_name = dn;
                             }
                         }
                         g_object_unref(saved_tab_for_enc);
                     }
                     char *body = g_strdup_printf(
                         _("Some characters in this document cannot be represented in %s "
                           "and were replaced with \u2018?\u2019 in the saved file.\n\n"
                           "Switch to UTF-8 encoding to preserve all characters."),
                         encoding_name);
                     AdwAlertDialog *lossy_dialog = ADW_ALERT_DIALOG(
                         adw_alert_dialog_new(_("Characters Lost During Save"), body));
                     adw_alert_dialog_add_response(lossy_dialog, "ok", _("OK"));
                     adw_alert_dialog_set_response_appearance(lossy_dialog, "ok",
                                                              ADW_RESPONSE_SUGGESTED);
                     adw_alert_dialog_choose(lossy_dialog, GTK_WIDGET(root), NULL, NULL, NULL);
                     g_free(body);
                 }
             }
         }
         g_object_unref(window);
    }
    
    if (error) g_error_free(error);
    
    /* Clean up data */
    g_weak_ref_clear(&data->tab_ref);
    g_weak_ref_clear(&data->window_ref);
    g_free(data->original_path);
    g_main_context_unref(data->context);
    g_free(data);
}

/* Internal: immediately start the async save (encoding already decided) */
static void
do_save_with_progress(ViteWindow *win, ViteTab *tab, Document *doc, const char *path)
{
    if (!doc || !path) return;
    
    DocumentSaveTask *stask = document_save_async_start(doc, path);
    if (!stask) {
        AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
            _("Save Failed"),
            _("Could not start save operation. Check permissions or disk space.")));
        adw_alert_dialog_add_response(dialog, "ok", _("OK"));
        adw_alert_dialog_choose(dialog, GTK_WIDGET(win->window), NULL, NULL, NULL);
        return;
    }
    
    /* Setup progress UI */
    if (tab) {
        vite_tab_set_loading(tab, TRUE);
        vite_tab_set_operation_type(tab, VITE_OP_SAVING);
        GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
        GtkWidget *editor = get_editor_from_page(page);
        GPtrArray *editors = find_all_editors_recursive(page);
        for (guint i = 0; i < editors->len; i++) {
            gtk_widget_set_sensitive(GTK_WIDGET(g_ptr_array_index(editors, i)), FALSE);
        }
        g_ptr_array_unref(editors);
        update_reload_action_enabled(win, editor);
    }
    
    if (win) {
         win->loading_count++;
         update_header_spinner(win);
    }
    
    SaveWorkerData *data = g_new0(SaveWorkerData, 1);
    data->task = stask;
    g_weak_ref_init(&data->tab_ref, tab);
    g_weak_ref_init(&data->window_ref, win ? win->window : NULL);
    data->original_path = g_strdup(path);
    data->context = g_main_context_ref(g_main_context_default());
    
    GCancellable *cancellable = NULL;
    if (tab) {
        cancellable = vite_tab_get_cancellable(tab);
        g_cancellable_reset(cancellable);
    }
    
    GTask *task = g_task_new(NULL, cancellable, on_save_complete, data);
    g_task_set_task_data(task, data, NULL);
    g_task_run_in_thread(task, save_worker_wrapper);
    g_object_unref(task);
}

/* Context for the pre-save lossy encoding dialog response */
typedef struct {
    ViteWindow *win;
    ViteTab    *tab;
    Document   *doc;
    char       *path;
} PreSaveLossyCtx;

static void
on_presave_lossy_response(GObject    *source,
                          GAsyncResult *res,
                          gpointer      user_data)
{
    PreSaveLossyCtx *ctx = user_data;
    const char *response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), res);
    
    if (g_strcmp0(response, "save-utf8") == 0) {
        /* Switch document encoding to UTF-8 then save cleanly */
        document_set_encoding(ctx->doc, ENCODING_UTF8);
        do_save_with_progress(ctx->win, ctx->tab, ctx->doc, ctx->path);
    } else if (g_strcmp0(response, "save-anyway") == 0) {
        /* User accepts character loss — save with '?' substitution */
        do_save_with_progress(ctx->win, ctx->tab, ctx->doc, ctx->path);
    }
    /* else "cancel" — do nothing */
    
    g_object_unref(ctx->doc);
    g_free(ctx->path);
    g_free(ctx);
}

/* Public entry point – check for encoding loss first, then save (or prompt) */
static void
save_async_with_progress(ViteWindow *win, ViteTab *tab, Document *doc, const char *path)
{
    if (!doc || !path) return;
    
    /* Quick pre-save probe: will any characters be lost? */
    if (document_check_encoding_lossy(doc)) {
        const char *encoding_name = "the target encoding";
        FileEncoding fe = document_get_encoding(doc);
        const char *dn = file_encoding_to_display_name(fe);
        if (dn && *dn) encoding_name = dn;
        
        char *body = g_strdup_printf(
            _("Some characters cannot be saved in %s.\n"
              "They will be replaced if you continue."),
            encoding_name);
        
        AdwAlertDialog *dialog = ADW_ALERT_DIALOG(
            adw_alert_dialog_new(_("Unsupported Encoding"), body));
        g_free(body);
        
        adw_alert_dialog_add_response(dialog, "cancel",     _("Cancel"));
        adw_alert_dialog_add_response(dialog, "save-anyway", _("Save Anyway"));
        adw_alert_dialog_add_response(dialog, "save-utf8",   _("Save as UTF-8"));
        
        adw_alert_dialog_set_response_appearance(dialog, "cancel",     ADW_RESPONSE_DEFAULT);
        adw_alert_dialog_set_response_appearance(dialog, "save-anyway", ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_response_appearance(dialog, "save-utf8",   ADW_RESPONSE_SUGGESTED);
        
        adw_alert_dialog_set_default_response(dialog, "save-utf8");
        adw_alert_dialog_set_close_response(dialog, "cancel");
        
        PreSaveLossyCtx *ctx = g_new0(PreSaveLossyCtx, 1);
        ctx->win  = win;
        ctx->tab  = tab;
        ctx->doc  = document_ref(doc);
        ctx->path = g_strdup(path);
        
        adw_alert_dialog_choose(dialog, GTK_WIDGET(win->window),
                                NULL, on_presave_lossy_response, ctx);
        return; /* Save deferred to dialog response */
    }
    
    /* No lossy characters — save immediately */
    do_save_with_progress(win, tab, doc, path);
}

/* ============================================================================
 * Save / Save As Actions
 * ============================================================================ */

static void
on_save_as_dialog_response(GtkFileDialog *dialog, GAsyncResult *result, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    
    GFile *file = gtk_file_dialog_save_finish(dialog, result, NULL);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            /* Get the active editor's document */
            GtkWidget *editor = get_active_editor(win);
            if (EDITOR_IS_WIDGET(editor)) {
                Document *doc = editor_widget_get_document(EDITOR_WIDGET(editor));
                
                ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
                save_async_with_progress(win, tab, doc, path);
            }
            g_free(path);
        }
        g_object_unref(file);
    }
}

static void
on_save_as_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (!win) return;
    
    GtkWidget *editor = get_active_editor(win);
    if (!EDITOR_IS_WIDGET(editor)) return;
    
    Document *doc = editor_widget_get_document(EDITOR_WIDGET(editor));
    const char *current_path = document_get_file_path(doc);
    
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, _("Save As"));
    
    /* Set initial file/folder if document has path */
    if (current_path) {
        GFile *file = g_file_new_for_path(current_path);
        GFile *parent = g_file_get_parent(file);
        if (parent) {
            gtk_file_dialog_set_initial_folder(dialog, parent);
            g_object_unref(parent);
        }
        char *basename = g_file_get_basename(file);
        if (basename) {
            gtk_file_dialog_set_initial_name(dialog, basename);
            g_free(basename);
        }
        g_object_unref(file);
    } else {
        ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
        if (tab) {
            const char *current_title = vite_tab_get_title(tab);
            gtk_file_dialog_set_initial_name(dialog, current_title);
        }
    }
    
    gtk_file_dialog_save(dialog, GTK_WINDOW(win->window), NULL, 
                        (GAsyncReadyCallback)on_save_as_dialog_response, win);
    g_object_unref(dialog);
}

static void
on_save_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (!win) return;
    
    GtkWidget *editor = get_active_editor(win);
    if (!EDITOR_IS_WIDGET(editor)) return;
    
    Document *doc = editor_widget_get_document(EDITOR_WIDGET(editor));
    /* If we have a path, save directly ASYNC. Else, trigger Save As */
    const char *current_path = document_get_file_path(doc);
    
    if (current_path && strlen(current_path) > 0) {
        ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
        save_async_with_progress(win, tab, doc, current_path);
    } else {
         /* Redirect to Save As */
         on_save_as_action(action, parameter, user_data);
    }
}
