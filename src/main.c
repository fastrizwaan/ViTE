#include <gtk/gtk.h>
#include <adwaita.h>
#include "editor-widget.h"
#include "document.h"
#include "preferences.h"

static void open_file(GtkApplication *app, GFile *file);

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
    GtkWindow *win = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn)));
    GtkApplication *app = gtk_window_get_application(win);
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_open(dialog, win, NULL, on_file_opened, app);
    g_object_unref(dialog);
}

static void
on_prefs_btn_clicked(GtkButton *btn, gpointer user_data)
{
    GtkWindow *win = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn)));
    /* We need to get the editor widget. We stored doc, but did we store editor? 
       Actually, setup_window doesn't store editor.
       We can traverse children of scrolled window?
       Or traversing via window -> child (scrolled) -> child (editor).
    */
    GtkWidget *scrolled = gtk_window_get_child(win);
    GtkWidget *editor = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled));
    
    show_preferences_dialog(win, EDITOR_WIDGET(editor));
}

static void
setup_window(GtkWindow *window)
{
    GtkWidget *header = adw_header_bar_new();
    gtk_window_set_titlebar(window, header);
    
    GtkWidget *title = adw_window_title_new("Virtual Text Editor", NULL);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
    g_object_set_data(G_OBJECT(window), "window_title", title);
    
    GtkWidget *btn = gtk_button_new_with_label("Open");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_open_btn_clicked), NULL);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), btn);

    GtkWidget *btn_prefs = gtk_button_new_from_icon_name("emblem-system-symbolic"); /* Gear icon */
    g_signal_connect(btn_prefs, "clicked", G_CALLBACK(on_prefs_btn_clicked), NULL);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), btn_prefs);
}

static void
activate(GtkApplication *app, gpointer user_data)
{
    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    
    setup_window(GTK_WINDOW(window));
    
    AdwWindowTitle *title = ADW_WINDOW_TITLE(g_object_get_data(G_OBJECT(window), "window_title"));
    adw_window_title_set_title(title, "Untitled");
    adw_window_title_set_subtitle(title, NULL);

    /* Create a new empty document */
    Document *doc = document_new(NULL);
    
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_window_set_child(GTK_WINDOW(window), scrolled);

    GtkWidget *editor = editor_widget_new();
    editor_widget_set_document(EDITOR_WIDGET(editor), doc);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), editor);
    
    g_object_set_data_full(G_OBJECT(window), "document", doc, (GDestroyNotify)document_free);
    
    /* Focus the editor so user can start typing immediately */
    gtk_widget_grab_focus(editor);
    
    gtk_window_present(GTK_WINDOW(window));
}

static void
open_file(GtkApplication *app, GFile *file)
{
    char *path = g_file_get_path(file);
    if (!path) return;

    Document *doc = document_new(path);
    if (!doc) {
        g_warning("Failed to open %s", path);
        g_free(path);
        return;
    }

    GtkWidget *window = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    setup_window(GTK_WINDOW(window));
    
    AdwWindowTitle *title = ADW_WINDOW_TITLE(g_object_get_data(G_OBJECT(window), "window_title"));
    
    char *name = g_file_get_basename(file);
    char *dirname = g_path_get_dirname(path);
    
    adw_window_title_set_title(title, name);
    adw_window_title_set_subtitle(title, dirname);
    
    g_free(name);
    g_free(dirname);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_window_set_child(GTK_WINDOW(window), scrolled);

    GtkWidget *editor = editor_widget_new();
    editor_widget_set_document(EDITOR_WIDGET(editor), doc);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), editor);
    
    /* Detect language from extension */
    char *dot = strrchr(path, '.');
    if (dot) {
        editor_widget_set_language(EDITOR_WIDGET(editor), dot + 1);
    } else {
        /* No extension, maybe check filename for Makefile? or assume none */
    }

    g_object_set_data_full(G_OBJECT(window), "document", doc, (GDestroyNotify)document_free);

    gtk_window_present(GTK_WINDOW(window));
    g_free(path);
}

static void
on_open(GtkApplication *app, GFile **files, int n_files, char *hint, gpointer user_data)
{
    for (int i = 0; i < n_files; i++) {
        open_file(app, files[i]);
    }
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
