#include <gtk/gtk.h>
#include <adwaita.h>
#include "editor-widget.h"
#include "document.h"
#include "preferences.h"
#include "tab-bar.h"
#include "tab.h"

static GtkWindow *main_window = NULL;
static ViteTabBar *main_tab_bar = NULL;
static GtkStack *main_stack = NULL;

static void open_file(GtkApplication *app, GFile *file);
static void create_new_tab (GtkApplication *app, const char *title, Document *doc);

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
on_tab_clicked (ViteTab *tab, gpointer user_data)
{
    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (page) {
        gtk_stack_set_visible_child(main_stack, page);
        vite_tab_bar_set_active_tab(main_tab_bar, tab);
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
    "    padding-right: 3px;"
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
    "    padding-left: 3px;"
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
}

static void
setup_window(GtkWindow *window)
{
    load_css();

    GtkWidget *titlebar_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(titlebar_container, "titlebar-box");
    gtk_window_set_titlebar(window, titlebar_container);
    
    GtkWidget *header = adw_header_bar_new();
    gtk_widget_add_css_class(header, "flat");
    gtk_box_append(GTK_BOX(titlebar_container), header);
    
    GtkWidget *title = adw_window_title_new("ViTE", NULL);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
    
    /* Open Split Button */
    GSimpleAction *act_open = g_simple_action_new("open-file", NULL);
    g_signal_connect_swapped(act_open, "activate", G_CALLBACK(on_open_btn_clicked), NULL);
    g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(act_open));
    
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Open File", "win.open-file");
    g_menu_append(menu, "New Document", "win.new-tab"); /* Reusing or new? */
    
    GtkWidget *split_btn = adw_split_button_new();
    adw_split_button_set_label(ADW_SPLIT_BUTTON(split_btn), "Open");
    adw_split_button_set_menu_model(ADW_SPLIT_BUTTON(split_btn), G_MENU_MODEL(menu));
    g_signal_connect(split_btn, "clicked", G_CALLBACK(on_open_btn_clicked), NULL);
    
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
    
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(window), vbox);
    
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
    Document *doc = document_new(path);
    if (!doc) {
        g_warning("Failed to open %s", path);
        g_free(path);
        return;
    }
    char *name = g_file_get_basename(file);
    create_new_tab(app, name, doc);
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
