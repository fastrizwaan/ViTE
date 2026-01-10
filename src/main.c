#include <gtk/gtk.h>
#include <adwaita.h>
#include <glib/gstdio.h>
#include "editor-widget.h"
#include "document.h"
#include "preferences.h"
#include "tab-bar.h"
#include "tab.h"

static GtkWindow *main_window = NULL;
static ViteTabBar *main_tab_bar = NULL;
static GtkStack *main_stack = NULL;
static AdwWindowTitle *main_window_title = NULL;

static void open_file(GtkApplication *app, GFile *file);
static void create_new_tab (GtkApplication *app, const char *title, Document *doc);
static void update_recent_files_list(GtkListBox *list_box, GtkApplication *app, GtkPopover *popover);
static void on_action_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data);
static gboolean filter_recent_items(GtkListBoxRow *row, gpointer user_data);
static void on_recent_context_menu(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static void add_to_local_recents(const char *uri);
static GList* load_local_recents(void);
static void save_local_recents(GList *uris);
static void on_close_recent_btn_clicked(GtkButton *btn, gpointer user_data);
static void remove_recent_item(GtkListBoxRow *row);

static void
on_file_opened (GObject* source_object, GAsyncResult* res, gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GtkApplication *app = GTK_APPLICATION(user_data);
    GFile *file = gtk_file_dialog_open_finish(dialog, res, NULL);
    if (file) {
        open_file(app, file);
        g_object_unref(file);
    }
}

static void
on_open_btn_clicked(GtkButton *btn, gpointer user_data)
{
    GtkWindow *win = btn ? GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn))) : main_window;
    GtkFileChooserNative *file_chooser_dialog; // Renamed to avoid conflict
    GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_OPEN;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    GtkApplication *app = gtk_window_get_application(win); // Moved app declaration here
    gtk_file_dialog_open(dialog, win, NULL, on_file_opened, app);
    g_object_unref(dialog);
}

static void
on_prefs_btn_clicked(GtkButton *btn, gpointer user_data)
{
    GtkWindow *win = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn)));
    if (!main_stack) return;
    GtkWidget *scrolled = gtk_stack_get_visible_child(main_stack);
    if (!scrolled) return;
    GtkWidget *editor = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled));
    show_preferences_dialog(win, EDITOR_WIDGET(editor));
}

static void
update_window_title(Document *doc)
{
    if (!main_window_title) return;
    
    if (!doc) {
        adw_window_title_set_title(main_window_title, "ViTE");
        adw_window_title_set_subtitle(main_window_title, NULL);
        return;
    }

    const char *path = document_get_file_path(doc);
    if (!path) {
        adw_window_title_set_title(main_window_title, "Untitled");
        adw_window_title_set_subtitle(main_window_title, NULL);
        return;
    }

    char *display_name = g_path_get_basename(path);
    adw_window_title_set_title(main_window_title, display_name);
    g_free(display_name);

    char *dir = g_path_get_dirname(path);
    const char *home = g_get_home_dir();
    char *subtitle = NULL;

    if (g_str_has_prefix(dir, home)) {
        subtitle = g_strconcat("~", dir + strlen(home), NULL);
    } else {
        subtitle = g_strdup(dir);
    }
    g_free(dir);

    adw_window_title_set_subtitle(main_window_title, subtitle);
    g_free(subtitle);
}

static void
on_tab_clicked (ViteTab *tab, gpointer user_data)
{
    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (page) {
        gtk_stack_set_visible_child(main_stack, page);
        vite_tab_bar_set_active_tab(main_tab_bar, tab);
        
        GtkWidget *editor = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(page));
        if (editor) {
            /* Update title */
            if (EDITOR_IS_WIDGET(editor)) {
                Document *doc = editor_widget_get_document(EDITOR_WIDGET(editor));
                update_window_title(doc);
            }

            /* Use idle to ensure focus sticks after stack transition */
            g_idle_add_once((GSourceOnceFunc)gtk_widget_grab_focus, editor);
        }
    }
}

static void
load_css(void)
{
    const char *css = 
    ".titlebar-box {"
    "    background: @headerbar_bg_color;"
    "    color: @headerbar_fg_color;"
    "    padding-bottom: 4px;"
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
    ".open-split-btn button {"
    "    border: none;"
    "    box-shadow: none;"
    "    outline: none;"
    "    background: transparent;"
    "    border-top-left-radius: 10px;" 
    "    border-bottom-left-radius: 10px;" 
    "    border-top-right-radius: 0px;"
    "    border-bottom-right-radius: 0px;"
    "    margin: 0px;"
    "    padding-left: 10px;"
    "    padding-right: 6px;"
    "}"
    ".open-split-btn button:hover {"
    "    background: alpha(@window_fg_color, 0.12);"
    "    border-top-left-radius: 10px;" 
    "    border-bottom-left-radius: 10px;" 
    "    border-top-right-radius: 0px;"
    "    border-bottom-right-radius: 0px;"
    "}"

    ".open-split-btn menubutton:last-child > button {"
    "    border-top-left-radius: 0px; "
    "    border-bottom-left-radius: 0px;" 
    "    border-top-right-radius: 10px; "
    "    border-bottom-right-radius: 10px;"
    "    padding-left: 4px;"
    "    padding-right: 6px;"
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
    ".recent-list {"
    "    background: transparent;"
    "    margin-right: -10px;"
    "}"
    ".recent-list row {"
    "    border-radius: 10px;"
    "    margin-right: 16px;"
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
    "}";
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void
on_tab_close_clicked (ViteTab *tab, gpointer user_data)
{
    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (page && GTK_IS_WIDGET(page)) {
        GtkWidget *parent = gtk_widget_get_parent(page);
        if (parent == GTK_WIDGET(main_stack)) {
             gtk_stack_remove(main_stack, page);
        }
    }
    vite_tab_bar_remove_tab(main_tab_bar, tab);
    
    if (vite_tab_bar_get_n_tabs(main_tab_bar) == 0) { 
        if (main_window) {
             GtkApplication *app = gtk_window_get_application(main_window);
             Document *doc = document_new(NULL);
             create_new_tab(app, "Untitled", doc);
        }
    }
}

static void
on_new_tab_clicked_header (GtkButton *btn, gpointer user_data)
{
    GtkWindow *win = GTK_WINDOW(user_data);
    GtkApplication *app = gtk_window_get_application(win);
    Document *doc = document_new(NULL);
    create_new_tab(app, "Untitled", doc);
}

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

static void
on_recent_item_activated(GtkListBox *list_box, GtkListBoxRow *row, gpointer user_data)
{
    GtkApplication *app = GTK_APPLICATION(user_data);
    const char *uri = g_object_get_data(G_OBJECT(row), "uri");
    if (uri) {
        GFile *file = g_file_new_for_uri(uri);
        open_file(app, file);
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
    
    /* Limit to 50 items */
    while (g_list_length(uris) > 50) {
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
    g_free(u);
}

static void
on_overflow_changed (ViteTabBar *bar, gboolean overflowing, gpointer user_data)
{
    GtkWidget *btn = GTK_WIDGET(user_data);
    
    OverflowUpdate *u = g_new(OverflowUpdate, 1);
    u->btn = btn;
    u->overflowing = overflowing;
    g_idle_add_once(update_overflow_idle, u);
}

static void
on_popover_tab_row_activated (GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    ViteTab *tab = g_object_get_data(G_OBJECT(row), "tab");
    if (tab) {
        g_signal_emit_by_name(tab, "clicked");
        
        GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(list), GTK_TYPE_POPOVER);
        if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
    }
}

static void
update_open_tabs_list (GtkWidget *popover, gpointer user_data)
{
    GtkListBox *list = GTK_LIST_BOX(user_data);
    if (!list) list = g_object_get_data(G_OBJECT(popover), "list");
    if (!list) return;

    /* Clear */
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list)))) {
        gtk_list_box_remove(list, child);
    }
    
    if (!main_tab_bar) return;
    
    GList *tabs = vite_tab_bar_get_tabs(main_tab_bar);
    for (GList *l = tabs; l != NULL; l = l->next) {
        ViteTab *tab = VITE_TAB(l->data);
        const char *title = vite_tab_get_title(tab);
        
        GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_start(row_box, 12);
        gtk_widget_set_margin_end(row_box, 12);
        gtk_widget_set_margin_top(row_box, 8);
        gtk_widget_set_margin_bottom(row_box, 8);
        
        GtkWidget *label = gtk_label_new(title);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(row_box), label);
        
        if (vite_tab_is_active(tab)) {
            GtkWidget *icon = gtk_image_new_from_icon_name("object-select-symbolic");
            gtk_box_append(GTK_BOX(row_box), icon);
        }
        
        gtk_list_box_insert(list, row_box, -1);
        
        /* Store tab pointer */
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(gtk_widget_get_parent(row_box))));
        g_object_set_data(G_OBJECT(row), "tab", tab);
    }
    g_list_free(tabs);
    
    /* Connect activation */
    g_signal_handlers_disconnect_by_func(list, on_popover_tab_row_activated, NULL);
    g_signal_connect(list, "row-activated", G_CALLBACK(on_popover_tab_row_activated), NULL);
}

static void
update_recent_files_list(GtkListBox *list_box, GtkApplication *app, GtkPopover *popover)
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
    
    for (GList *l = uris; l != NULL && count < 20; l = l->next) {
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
        gtk_widget_set_margin_end(row, 6);
        gtk_widget_set_margin_top(row, 6);
        gtk_widget_set_margin_bottom(row, 6);
        
        /* Try to get icon for file */
        GIcon *gicon = NULL;
        GFileInfo *info = g_file_query_info(file, "standard::icon", 0, NULL, NULL);
        if (info) {
            gicon = g_file_info_get_icon(info);
            if (gicon) g_object_ref(gicon);
            g_object_unref(info);
        }
        
        GIcon *symbolic = NULL;
        if (gicon && G_IS_THEMED_ICON(gicon)) {
            const char * const *names = g_themed_icon_get_names(G_THEMED_ICON(gicon));
            if (names && names[0]) {
                char *sym_name = g_strconcat(names[0], "-symbolic", NULL);
                symbolic = g_themed_icon_new_with_default_fallbacks(sym_name);
                g_free(sym_name);
            }
        }
        
        GtkWidget *icon_widget;
        if (symbolic) {
            icon_widget = gtk_image_new_from_gicon(symbolic);
            g_object_unref(symbolic);
        } else {
            icon_widget = gtk_image_new_from_icon_name("text-x-generic-symbolic");
        }
        if (gicon) g_object_unref(gicon);
        
        gtk_widget_set_valign(icon_widget, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), icon_widget);

        /* Text container (Title + Subtitle) */
        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(row), vbox);

        GtkWidget *title_label = gtk_label_new(display_name);
        gtk_widget_set_halign(title_label, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(title_label), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(vbox), title_label);

        if (subtitle) {
            GtkWidget *subtitle_label = gtk_label_new(subtitle);
            gtk_widget_set_halign(subtitle_label, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(subtitle_label), PANGO_ELLIPSIZE_END);
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
on_remove_recent_clicked(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    remove_recent_item(GTK_LIST_BOX_ROW(user_data));
}

static void
on_close_recent_btn_clicked(GtkButton *btn, gpointer user_data)
{
    GtkListBoxRow *row = GTK_LIST_BOX_ROW(user_data);
    remove_recent_item(row);
}

static void
on_clear_all_confirmed(GObject *source_object, GAsyncResult *res, gpointer user_data)
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
on_clear_all_clicked(GSimpleAction *action, GVariant *parameter, gpointer user_data)
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
on_recent_popover_unmap(GtkWidget *popover, gpointer user_data)
{
    GtkEditable *search_entry = GTK_EDITABLE(user_data);
    gtk_editable_set_text(search_entry, "");
}

static gboolean
on_search_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data)
{
    GtkPopover *popover = GTK_POPOVER(user_data);
    if (keyval == GDK_KEY_Escape) {
        gtk_popover_popdown(popover);
        return TRUE;
    }
    return FALSE;
}

static void
on_recent_context_menu(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
    GtkListBox *list = GTK_LIST_BOX(user_data);
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(list, y);
    
    GMenu *menu = g_menu_new();
    GSimpleActionGroup *group = g_simple_action_group_new();
    
    if (row) {
        g_menu_append(menu, "Remove from Recents", "context.remove");
        GSimpleAction *act_remove = g_simple_action_new("remove", NULL);
        g_signal_connect(act_remove, "activate", G_CALLBACK(on_remove_recent_clicked), row);
        g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act_remove));
    }
    
    g_menu_append(menu, "Clear All Recents", "context.clear-all");
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
on_close_curr_tab_clicked (GtkButton *btn, gpointer user_data)
{
    if (!main_stack) return;
    GtkWidget *page = gtk_stack_get_visible_child(main_stack);
    if (!page) return;
    ViteTab *tab = g_object_get_data(G_OBJECT(page), "tab");
    if (tab) {
        on_tab_close_clicked(tab, NULL);
    }
}

static void
create_new_tab (GtkApplication *app, const char *title, Document *doc)
{
    if (!main_window) return;
    
    GtkWidget *scrolled = gtk_scrolled_window_new();
    GtkWidget *editor = editor_widget_new();
    editor_widget_set_document(EDITOR_WIDGET(editor), doc);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), editor);
    
    const char *doc_path = document_get_file_path(doc);
    if (doc_path) {
        const char *dot = strrchr(doc_path, '.');
        if (dot) editor_widget_set_language(EDITOR_WIDGET(editor), dot + 1);
    }
    
    char id[32];
    sprintf(id, "page_%p", scrolled);
    gtk_stack_add_named(main_stack, scrolled, id);
    
    GtkWidget *tab = vite_tab_new(title);
    g_object_set_data(G_OBJECT(tab), "page", scrolled);
    g_object_set_data(G_OBJECT(scrolled), "tab", tab); /* Link back for close button */
    
    g_signal_connect(tab, "clicked", G_CALLBACK(on_tab_clicked), NULL);
    g_signal_connect(tab, "close-clicked", G_CALLBACK(on_tab_close_clicked), NULL);
    
    vite_tab_bar_add_tab(main_tab_bar, VITE_TAB(tab));
    vite_tab_bar_set_active_tab(main_tab_bar, VITE_TAB(tab));
    
    gtk_stack_set_visible_child(main_stack, scrolled);
    
    /* Use idle to ensure focus sticks after stack transition */
    g_idle_add_once((GSourceOnceFunc)gtk_widget_grab_focus, editor);

    update_window_title(doc);
}

static void
setup_window(GtkWindow *window)
{
    load_css();

    /* Create overlay for titlebar to support drag ghosts */
    GtkWidget *titlebar_overlay = gtk_overlay_new();
    
    GtkWidget *titlebar_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(titlebar_container, "titlebar-box");
    gtk_overlay_set_child(GTK_OVERLAY(titlebar_overlay), titlebar_container);
    
    gtk_window_set_titlebar(window, titlebar_overlay);
    
    GtkWidget *header = adw_header_bar_new();
    gtk_widget_add_css_class(header, "flat");
    gtk_box_append(GTK_BOX(titlebar_container), header);
    
    GtkWidget *title = adw_window_title_new("ViTE", NULL);
    main_window_title = ADW_WINDOW_TITLE(title);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
    
    /* Open Split Button */
    GSimpleAction *act_open = g_simple_action_new("open-file", NULL);
    g_signal_connect_swapped(act_open, "activate", G_CALLBACK(on_open_btn_clicked), NULL);
    g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(act_open));
    
    GtkWidget *split_btn = adw_split_button_new();
    adw_split_button_set_label(ADW_SPLIT_BUTTON(split_btn), "Open");
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
    g_object_set(search_entry, "placeholder-text", "Search Documents", NULL);
    gtk_box_append(GTK_BOX(pop_vbox), search_entry);
    
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_search_key_pressed), popover);
    gtk_widget_add_controller(search_entry, key_ctrl);

    /* Scrolled Window */
    GtkWidget *scrolled = gtk_scrolled_window_new();
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

    GtkApplication *app = gtk_window_get_application(window);
    g_signal_connect(recent_list, "row-activated", G_CALLBACK(on_recent_item_activated), app);
    
    /* Update list when popover is shown */
    g_signal_connect_swapped(popover, "map", G_CALLBACK(update_recent_files_list), recent_list);
    g_signal_connect_swapped(popover, "map", G_CALLBACK(gtk_list_box_unselect_all), recent_list);
    g_signal_connect(popover, "unmap", G_CALLBACK(on_recent_popover_unmap), search_entry);
    g_object_set_data(G_OBJECT(popover), "app", app); /* For the update function */

    adw_split_button_set_popover(ADW_SPLIT_BUTTON(split_btn), GTK_POPOVER(popover));
    
    /* Styling to look like single button by default */
    /* By default AdwSplitButton looks joined. We can add specific classes if needed. */
     gtk_widget_set_tooltip_text(split_btn, "Open File");
     gtk_widget_add_css_class(split_btn, "open-split-btn");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), split_btn);
    
    /* New Tab (Icon Only) */
    GtkWidget *btn_new = gtk_button_new_from_icon_name("tab-new-symbolic");
    gtk_widget_set_tooltip_text(btn_new, "New Tab");
    g_signal_connect(btn_new, "clicked", G_CALLBACK(on_new_tab_clicked_header), window);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), btn_new);
    
    GtkWidget *btn_prefs = gtk_button_new_from_icon_name("emblem-system-symbolic");
    g_signal_connect(btn_prefs, "clicked", G_CALLBACK(on_prefs_btn_clicked), NULL);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), btn_prefs);
    
    main_tab_bar = VITE_TAB_BAR(vite_tab_bar_new());
    gtk_box_append(GTK_BOX(titlebar_container), GTK_WIDGET(main_tab_bar));

    /* Open Tabs Button (Overflow Menu) */
    GtkWidget *btn_tabs = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(btn_tabs), "pan-down-symbolic");
    gtk_widget_set_tooltip_text(btn_tabs, "Open Tabs");
    gtk_widget_set_visible(btn_tabs, FALSE); /* Hidden by default until overflow */
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), btn_tabs);

    /* Popover for tabs */
    GtkWidget *tabs_popover = gtk_popover_new();
    GtkWidget *tabs_list = gtk_list_box_new();
    GtkWidget *tabs_scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(tabs_scrolled), tabs_list);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(tabs_scrolled), 300);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(tabs_scrolled), TRUE);
    gtk_popover_set_child(GTK_POPOVER(tabs_popover), tabs_scrolled);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(btn_tabs), tabs_popover);
    
    /* Check for overflow logic helper */
    g_object_set_data(G_OBJECT(main_tab_bar), "tabs-btn", btn_tabs);
    g_object_set_data(G_OBJECT(tabs_popover), "list", tabs_list);
    
    g_signal_connect(main_tab_bar, "overflow-changed", G_CALLBACK(on_overflow_changed), btn_tabs);
    g_signal_connect(tabs_popover, "map", G_CALLBACK(update_open_tabs_list), NULL); /* User_data passed via signal not ideal, let's use swap/data */
    
    /* Better signal connect for updating list */
    g_signal_connect(tabs_popover, "map", G_CALLBACK(update_open_tabs_list), tabs_list);
}

static void
activate(GtkApplication *app, gpointer user_data)
{
    if (main_window) {
        gtk_window_present(main_window);
        return;
    }

    GtkWidget *window = gtk_application_window_new(app);
    main_window = GTK_WINDOW(window);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    
    setup_window(GTK_WINDOW(window));
    
    /* Create overlay container for the window to support drag ghost */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_window_set_child(GTK_WINDOW(window), overlay);
    
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), vbox);
    
    /* Tab Bar already created in setup_window and assigned to main_tab_bar global */
    
    main_stack = GTK_STACK(gtk_stack_new());
    gtk_widget_set_vexpand(GTK_WIDGET(main_stack), TRUE);
    gtk_box_append(GTK_BOX(vbox), GTK_WIDGET(main_stack));
    
    Document *doc = document_new(NULL);
    create_new_tab(app, "Untitled", doc);
    
    gtk_window_present(GTK_WINDOW(window));
}

static void
open_file(GtkApplication *app, GFile *file)
{
    if (!main_window) activate(app, NULL);
    char *path = g_file_get_path(file);
    if (!path) return;

    /* Check if file is already open */
    if (main_tab_bar) {
        GList *tabs = vite_tab_bar_get_tabs(main_tab_bar);
        for (GList *l = tabs; l != NULL; l = l->next) {
            ViteTab *tab = VITE_TAB(l->data);
            GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
            if (page) {
                GtkWidget *scroll_child = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(page));
                if (EDITOR_IS_WIDGET(scroll_child)) {
                    Document *d = editor_widget_get_document(EDITOR_WIDGET(scroll_child));
                    const char *p = document_get_file_path(d);
                    if (g_strcmp0(path, p) == 0) {
                        /* Switch to existing tab */
                        on_tab_clicked(tab, NULL);
                        
                        g_free(path);
                        g_list_free(tabs);
                        return;
                    }
                }
            }
        }
        g_list_free(tabs);
    }
    Document *doc = document_new(path);
    if (!doc) {
        g_warning("Failed to open %s", path);
        g_free(path);
        return;
    }
    char *name = g_file_get_basename(file);
    create_new_tab(app, name, doc);
    
    /* Add to recent files */
    char *uri = g_file_get_uri(file);
    add_to_local_recents(uri);
    g_free(uri);

    g_free(name);
    g_free(path);
}

static void
on_open(GtkApplication *app, GFile **files, int n_files, char *hint, gpointer user_data)
{
    for (int i = 0; i < n_files; i++) open_file(app, files[i]);
}

int
main(int argc, char **argv)
{
    GtkApplication *app;
    int status;
    adw_init();
    app = gtk_application_new("io.github.fastrizwan.ViTE", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "open", G_CALLBACK(on_open), NULL);
    status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
