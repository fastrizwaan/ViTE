#include <gtk/gtk.h>
#include <adwaita.h>
#include <glib/gstdio.h>
#include "editor-widget.h"
#include "document.h"
#include "preferences.h"
#include "tab-bar.h"
#include "tab.h"
#include "find-replace-bar.h"

typedef struct _ViteWindow ViteWindow;

struct _ViteWindow {
    GtkWindow *window;
    ViteTabBar *tab_bar;
    GtkStack *stack;
    AdwWindowTitle *window_title;
};

/* Globals removed: main_window, main_tab_bar, main_stack, main_window_title */
static int untitled_count = 1;

static void open_file(GtkApplication *app, ViteWindow *target_window, GFile *file);
static void create_new_tab (ViteWindow *win, const char *title, Document *doc);
static ViteWindow *setup_window(GtkWindow *window);
static void activate(GtkApplication *app, gpointer user_data);
static void update_recent_files_list(GtkListBox *list_box, GtkApplication *app, GtkPopover *popover);
static void on_action_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data);
static gboolean filter_recent_items(GtkListBoxRow *row, gpointer user_data);
static void on_recent_context_menu(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static void add_to_local_recents(const char *uri);
static GList* load_local_recents(void);
static void save_local_recents(GList *uris);
static void on_close_recent_btn_clicked(GtkButton *btn, gpointer user_data);
static void on_open_dialog_response(GtkFileDialog *dialog, GAsyncResult *result, gpointer user_data);

static void move_tab_to_window(ViteWindow *target_win, ViteTab *tab, int position);


static void
on_file_opened (GObject* source_object, GAsyncResult* res, gpointer user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source_object);
    GtkApplication *app = GTK_APPLICATION(user_data);
    GFile *file = gtk_file_dialog_open_finish(dialog, res, NULL);
    if (file) {
        open_file(app, NULL, file);
        g_object_unref(file);
    }
}



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

static GtkWidget *get_editor_from_page(GtkWidget *page);
static void on_tab_clicked (ViteTab *tab, gpointer user_data);
static void on_new_tab_clicked_header(GtkButton *btn, gpointer user_data);
static void on_new_window_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_preferences_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_overflow_changed(ViteTabBar *bar, gboolean overflowing, gpointer user_data);
static void on_tab_dropped(ViteTabBar *bar, ViteTab *tab, int position, gpointer user_data);
static void update_open_tabs_list(GtkWidget *widget, gpointer user_data);
static GtkWidget *create_view_container(ViteWindow *win, GtkWidget *editor);
static void on_find_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_replace_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_split_right(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_split_down(GSimpleAction *action, GVariant *parameter, gpointer user_data);
static void on_close_split_action(GSimpleAction *action, GVariant *parameter, gpointer user_data);

static void on_document_modified(Document *doc, gboolean modified, void *user_data);
static void on_document_content_changed(Document *doc, void *user_data);
static void on_recent_item_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user_data);
static void on_recent_popover_unmap(GtkWidget *popover, gpointer user_data);
static void on_tab_close_clicked(ViteTab *tab, gpointer user_data);
static void on_tab_move_to_new_window(ViteTab *tab, gpointer user_data);
static void load_css(void);
static gboolean on_search_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
static void on_search_changed(GtkSearchEntry *entry, gpointer user_data);

static void
on_open_dialog_response(GtkFileDialog *dialog, GAsyncResult *result, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    GListModel *files = gtk_file_dialog_open_multiple_finish(dialog, result, NULL);
    if (files) {
        guint n = g_list_model_get_n_items(files);
        for (guint i = 0; i < n; i++) {
            GFile *file = g_list_model_get_item(files, i);
            open_file(gtk_window_get_application(win->window), win, file);
            g_object_unref(file);
        }
        g_object_unref(files);
    }
}

static void
on_open_btn_clicked(GtkButton *btn, gpointer user_data)
{
    ViteWindow *win = NULL;
    if (btn) {
         GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(btn));
         win = g_object_get_data(G_OBJECT(root), "vite-window");
    }
    
    /* Fallback? We shouldn't need it if triggered from UI */
    if (!win) return;
    
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_open_multiple(dialog, win->window, NULL, (GAsyncReadyCallback)on_open_dialog_response, win);
}

static void update_window_title_for_tab(ViteTab *tab);

/* ============================================================================
 * Widget Lifecycle Helper Functions
 * These provide safe, consistent patterns for GtkStack widget manipulation.
 * ============================================================================ */

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

static void close_split_view(GtkWidget *overlay);

static void
on_close_split_clicked(GtkWidget *overlay, gpointer user_data)
{
    /* Overlay is the inner overlay. Helper finds the ViewContainer */
    GtkWidget *view_container = gtk_widget_get_parent(overlay);
    if (view_container && gtk_widget_has_css_class(view_container, "view-split")) {
        close_split_view(view_container);
    }
}

static void
on_overlay_focus_leave(GtkEventControllerFocus *controller, gpointer user_data)
{
    GtkWidget *overlay = GTK_WIDGET(user_data);
    GtkWidget *btn = g_object_get_data(G_OBJECT(overlay), "close-btn");
    if (btn) gtk_widget_set_visible(btn, FALSE);
}

static void
on_overlay_focus_enter(GtkEventControllerFocus *controller, gpointer user_data)
{
    GtkWidget *overlay = GTK_WIDGET(user_data);
    
    /* Update Close Button Visibility */
    GtkWidget *btn = g_object_get_data(G_OBJECT(overlay), "close-btn");
    if (btn) {
        GtkWidget *parent = gtk_widget_get_parent(overlay); /* ViewContainer */
        GtkWidget *grandparent = parent ? gtk_widget_get_parent(parent) : NULL;
        
        /* Only show if ViewContainer is in a split (parent is Paned) */
        if (grandparent && GTK_IS_PANED(grandparent)) {
            gtk_widget_set_visible(btn, TRUE);
        } else {
            gtk_widget_set_visible(btn, FALSE);
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
    
    /* Close Button */
    GtkWidget *btn_close = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(btn_close, "flat");
    gtk_widget_set_valign(btn_close, GTK_ALIGN_START);
    gtk_widget_set_halign(btn_close, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_close, 4);
    gtk_widget_set_margin_end(btn_close, 4);
    gtk_widget_set_visible(btn_close, FALSE); /* Hidden initially */
    g_object_set_data(G_OBJECT(overlay), "close-btn", btn_close);
    
    /* We need to pass the overlay to the callback, not the button */
    g_signal_connect_swapped(btn_close, "clicked", G_CALLBACK(on_close_split_clicked), overlay);
    
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), btn_close);
    
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
    }
    
    return root_box;
}

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
on_preferences_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    /* connected with window as user_data */
    ViteWindow *win = (ViteWindow *)user_data;
    if (!win) return;
    
    /* Try to find editor from focus first */
    GtkWidget *focus = NULL;
    if (win->window && GTK_IS_WINDOW(win->window)) {
        focus = gtk_window_get_focus(win->window);
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
        show_preferences_dialog(win->window, EDITOR_WIDGET(editor));
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
        
        char *dir = g_path_get_dirname(doc_path);
        const char *home = g_get_home_dir();
        if (g_str_has_prefix(dir, home)) {
            subtitle = g_strconcat("~", dir + strlen(home), NULL);
        } else {
            subtitle = g_strdup(dir);
        }
        g_free(dir);
        g_object_unref(f);
    } else {
        title = g_strdup(vite_tab_get_title(tab));
        subtitle = g_strdup("Unsaved Document");
    }
    
    if (document_is_modified(doc)) {
        char *tmp = g_strdup_printf("• %s", title);
        g_free(title);
        title = tmp;
    } else if (!doc_path) {
        /* Check if this is the only tab */
        GList *tabs = vite_tab_bar_get_tabs(win->tab_bar);
        if (g_list_length(tabs) == 1) {
            /* Single untitled, unmodified tab -> Default Title */
            g_free(title);
            g_free(subtitle);
            title = g_strdup("Virtual Text Editor");
            subtitle = NULL;
        }
        g_list_free(tabs);
    }
    
    adw_window_title_set_title(win->window_title, title);
    adw_window_title_set_subtitle(win->window_title, subtitle);
    
    g_free(title);
    g_free(subtitle);
}

/* Backward compatibility wrapper if needed, but we should update call sites */
static void
update_window_title(Document *doc)
{
    /* Deprecated - do nothing */
}

static void
on_document_content_changed(Document *doc, void *user_data)
{
    ViteTab *tab = VITE_TAB(user_data);
    if (!document_get_file_path(doc)) {
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
    }
}

static void
on_document_modified(Document *doc, gboolean modified, void *user_data)
{
    ViteTab *tab = VITE_TAB(user_data);
    vite_tab_set_modified(tab, modified);
    
    /* Update window title if this is the active tab */
    if (vite_tab_is_active(tab)) {
        update_window_title_for_tab(tab);
    }
}

static void
on_tab_clicked (ViteTab *tab, gpointer user_data)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(tab));
    if (!root) return; 

    ViteWindow *win = g_object_get_data(G_OBJECT(root), "vite-window");
    if (!win) return;

    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (!page) return;

    /* Per-Tab Split Model: Just switch the stack to this tab's page */
    gtk_stack_set_visible_child(win->stack, page);
    vite_tab_bar_set_active_tab(win->tab_bar, tab);
    
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
             gtk_window_set_focus(win->window, NULL);
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
             GtkWidget *overlay = gtk_widget_get_ancestor(new_focus, GTK_TYPE_OVERLAY);
             if (overlay) {
                 GtkWidget *s_btn = g_object_get_data(G_OBJECT(overlay), "close-btn");
                 if (s_btn) {
                     /* Show close button if we are in a split (parent of sibling is now grandparent) */
                     GtkWidget *s_parent = gtk_widget_get_parent(sibling); /* This is the grandparent now */
                     gtk_widget_set_visible(s_btn, GTK_IS_PANED(s_parent));
                 }
            }
        }
    }
}

static void
do_split(ViteWindow *win, GtkOrientation orientation)
{
    ViteTab *tab = vite_tab_bar_get_active_tab(win->tab_bar);
    if (!tab) return;
    
    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (!page) return;
    
    GtkWidget *target_overlay = vite_tab_get_last_focused_child(tab);
    
    /* Fallback to window focus */
    if (!target_overlay) {
        GtkWidget *focus = gtk_window_get_focus(win->window);
        if (focus && gtk_widget_is_ancestor(focus, page)) {
             target_overlay = gtk_widget_get_ancestor(focus, GTK_TYPE_OVERLAY);
        }
    }
    
    /* Fallback to first editor */
    if (!target_overlay) {
        GtkWidget *ed = find_first_editor_recursive(page);
        if (ed) target_overlay = gtk_widget_get_ancestor(ed, GTK_TYPE_OVERLAY);
    }

    if (!target_overlay) return;

    /* Promote target_overlay to ViewContainer (the 'view-split' box) */
    GtkWidget *view_container = target_overlay;
    while (view_container && !gtk_widget_has_css_class(view_container, "view-split") && view_container != page) {
         view_container = gtk_widget_get_parent(view_container);
    }
    
    if (!view_container || !gtk_widget_has_css_class(view_container, "view-split")) return;
    
    /* Perform split on view_container */
    GtkWidget *parent = gtk_widget_get_parent(view_container);
    if (!parent) return;

    GtkWidget *paned = gtk_paned_new(orientation);
    
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
        return;
    }
    
    /* Start child is old view */
    gtk_paned_set_start_child(GTK_PANED(paned), view_container);
    g_object_unref(view_container);
    
    /* End child is NEW view */
    GtkWidget *old_editor = find_first_editor_recursive(view_container);
    Document *doc = editor_widget_get_document(EDITOR_WIDGET(old_editor));
    
    GtkWidget *new_scrolled = gtk_scrolled_window_new();
    GtkWidget *new_editor = editor_widget_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(new_scrolled), new_editor);
    editor_widget_set_document(EDITOR_WIDGET(new_editor), doc);
    
    const char *path = document_get_file_path(doc);
    if (path) {
         const char *dot = strrchr(path, '.');
         if (dot) editor_widget_set_language(EDITOR_WIDGET(new_editor), dot + 1);
    }
    
    GtkWidget *new_view_container = create_view_container(win, new_scrolled);
    gtk_paned_set_end_child(GTK_PANED(paned), new_view_container);
    
    int size = (orientation == GTK_ORIENTATION_HORIZONTAL) ? gtk_widget_get_width(view_container) : gtk_widget_get_height(view_container);
    if (size > 0) gtk_paned_set_position(GTK_PANED(paned), size / 2);
    
    defer_focus(new_editor);
}

static void
on_split_right(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (win) do_split(win, GTK_ORIENTATION_HORIZONTAL);
}

static void
on_split_down(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    if (win) do_split(win, GTK_ORIENTATION_VERTICAL);
}

static void
on_close_split_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
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
    const char *css = 
    "findbar {"
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
    "    transition: all 200ms ease;"
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
    /* Linked group override for findbar if needed */
    "findbar .linked > button:first-child {"
    "    border-top-left-radius: 6px;"
    "    border-bottom-left-radius: 6px;"
    "}"
    "findbar .linked > button:last-child {"
    "    border-top-right-radius: 6px;"
    "    border-bottom-right-radius: 6px;"
    "}"
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
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(tab));
    if (!root) return;
    
    ViteWindow *win = g_object_get_data(G_OBJECT(root), "vite-window");
    if (!win) return;

    /* Get the page widget associated with this tab */
    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    GtkWidget *parent = NULL;
    
    if (page && GTK_IS_WIDGET(page)) {
        g_object_ref(page);
        parent = gtk_widget_get_parent(page);
    } else {
        page = NULL;
    }
    
    /* Remove tab from tab bar (this triggers selection change) */
    vite_tab_bar_remove_tab(win->tab_bar, tab);
    
    /* Safely remove page from stack */
    if (page && parent) {
        if (GTK_IS_STACK(parent)) {
            stack_safe_remove_child(GTK_STACK(parent), page);
        } else {
            gtk_widget_unparent(page);
        }
    }
    
    if (page) {
        g_object_unref(page);
    }
    
    /* Close window if no tabs remain */
    if (vite_tab_bar_get_n_tabs(win->tab_bar) == 0) { 
         if (win->window) {
              gtk_window_close(win->window);
         }
    }
}

static void
on_new_tab_clicked_header (GtkButton *btn, gpointer user_data)
{
    /* user_data was window in signal connect? */
    /* setup_window: g_signal_connect(btn_new, "clicked", G_CALLBACK(on_new_tab_clicked_header), window); */
    GtkWindow *window = GTK_WINDOW(user_data);
    ViteWindow *win = g_object_get_data(G_OBJECT(window), "vite-window");
    
    if (win) {
        Document *doc = document_new(NULL);
        create_new_tab(win, "Untitled", doc);
    }
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
    
    ViteWindow *target_win = NULL;
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(list_box));
    if (root) {
        target_win = g_object_get_data(G_OBJECT(root), "vite-window");
    }
    
    const char *uri = g_object_get_data(G_OBJECT(row), "uri");
    if (uri) {
        GFile *file = g_file_new_for_uri(uri);
        open_file(app, target_win, file);
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
    
    ViteWindow *win = NULL;
    GtkRoot *root = gtk_widget_get_root(popover); /* Popover root might be window? No, attached to button. */
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
    
    GList *tabs = vite_tab_bar_get_tabs(win->tab_bar);
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
on_new_window_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteWindow *win = (ViteWindow *)user_data;
    GtkApplication *app = gtk_window_get_application(win->window);
    activate(app, NULL);
}
static void
on_tab_move_to_new_window (ViteTab *tab, gpointer user_data)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(tab));
    ViteWindow *current_win = g_object_get_data(G_OBJECT(root), "vite-window");
    if (!current_win) return;
    
    GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
    if (!page) return;
    
    /* 1. Create new window */
    GtkApplication *app = gtk_window_get_application(current_win->window);
    GtkWindow *w = GTK_WINDOW(gtk_application_window_new(app));
    ViteWindow *new_win = setup_window(w);
    gtk_window_set_default_size(w, 800, 600);
    
    /* 2. Move tab */
    move_tab_to_window(new_win, tab, -1);
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
    gtk_window_present(target_win->window);

    g_object_unref(tab);
    g_object_unref(page);
    
    /* Check if source window empty */
    if (vite_tab_bar_get_n_tabs(source_win->tab_bar) == 0) {
        /* Close source window if empty */
        if (source_win->window && GTK_IS_WINDOW(source_win->window)) {
            gtk_window_close(source_win->window);
        }
    }
}

static void
on_tab_dropped(ViteTabBar *tab_bar, ViteTab *tab, int position, gpointer user_data)
{
    ViteWindow *target_win = (ViteWindow *)user_data;
    move_tab_to_window(target_win, tab, position);
}

static gboolean
on_window_drop(GtkDropTarget *target, const GValue *value, double x, double y, ViteWindow *win)
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
    }
    return FALSE;
}

static void
create_new_tab (ViteWindow *win, const char *title, Document *doc)
{
    if (!win) return;
    
    GtkWidget *scrolled = gtk_scrolled_window_new();
    GtkWidget *editor = editor_widget_new();
    editor_widget_set_document(EDITOR_WIDGET(editor), doc);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), editor);
    
    const char *doc_path = document_get_file_path(doc);
    if (doc_path) {
        const char *dot = strrchr(doc_path, '.');
        if (dot) editor_widget_set_language(EDITOR_WIDGET(editor), dot + 1);
    }
    
    /* Create Container (Overlay) */
    GtkWidget *overlay = create_view_container(win, scrolled);
    
    /* Wrap in TabRoot (Box) to support splitting */
    GtkWidget *page_root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(page_root, TRUE);
    gtk_widget_set_vexpand(page_root, TRUE);
    gtk_box_append(GTK_BOX(page_root), overlay);
    
    char id[32];
    sprintf(id, "page_%p", page_root);
    
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
    /* Connect modification and content callbacks */
    document_add_modification_callback(doc, on_document_modified, tab);
    document_add_content_callback(doc, on_document_content_changed, tab);
    
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
    vite_tab_bar_set_active_tab(win->tab_bar, VITE_TAB(tab));
    
    if (win->stack) {
        gtk_stack_set_visible_child(win->stack, page_root);
    }
    
    /* Use defer_focus to safely grab focus after hierarchy is stable */
    defer_focus(editor);

    update_window_title_for_tab(VITE_TAB(tab));
}

static void

on_close_curr_tab_clicked (GtkButton *btn, gpointer user_data)
{
    ViteWindow *win = NULL;
    if (btn) {
         GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(btn));
         win = g_object_get_data(G_OBJECT(root), "vite-window");
    }
    
    if (!win) return;
    
    ViteTab *active = vite_tab_bar_get_active_tab(win->tab_bar);
    if (active) {
        on_tab_close_clicked(active, NULL);
    }
}




static ViteWindow *
setup_window(GtkWindow *window)
{
    ViteWindow *win = g_new0(ViteWindow, 1);
    win->window = window;
    g_object_set_data(G_OBJECT(window), "vite-window", win);

    load_css();

    /* Create overlay for titlebar to support drag ghosts */
    GtkWidget *titlebar_overlay = gtk_overlay_new();
    
    GtkWidget *titlebar_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(titlebar_container, "titlebar-box");
    gtk_overlay_set_child(GTK_OVERLAY(titlebar_overlay), titlebar_container);
    
    gtk_window_set_titlebar(window, titlebar_overlay);
    
    /* Add drop target to window to accept tabs */
    GtkDropTarget *window_drop = gtk_drop_target_new(VITE_TYPE_TAB, GDK_ACTION_MOVE);
    g_signal_connect(window_drop, "drop", G_CALLBACK(on_window_drop), win);
    gtk_widget_add_controller(GTK_WIDGET(window), GTK_EVENT_CONTROLLER(window_drop));
    
    GtkWidget *header = adw_header_bar_new();
    gtk_widget_add_css_class(header, "flat");
    gtk_box_append(GTK_BOX(titlebar_container), header);
    
    GtkWidget *title = adw_window_title_new("ViTE", NULL);
    win->window_title = ADW_WINDOW_TITLE(title);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
    
    GSimpleAction *pref_act = g_simple_action_new("preferences", NULL);
    g_signal_connect(pref_act, "activate", G_CALLBACK(on_preferences_action), win);
    g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(pref_act));

    GSimpleAction *find_act = g_simple_action_new("find", NULL);
    g_signal_connect(find_act, "activate", G_CALLBACK(on_find_action), win);
    g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(find_act));

    GSimpleAction *replace_act = g_simple_action_new("replace", NULL);
    g_signal_connect(replace_act, "activate", G_CALLBACK(on_replace_action), win);
    g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(replace_act));
    
    /* Shortcuts */
    GtkShortcutController *shortcuts = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
    gtk_shortcut_controller_set_scope(shortcuts, GTK_SHORTCUT_SCOPE_GLOBAL);

    GtkShortcut *find_sc = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_f, GDK_CONTROL_MASK), gtk_named_action_new("win.find"));
    gtk_shortcut_controller_add_shortcut(shortcuts, find_sc);

    GtkShortcut *repl_sc = gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_h, GDK_CONTROL_MASK), gtk_named_action_new("win.replace"));
    gtk_shortcut_controller_add_shortcut(shortcuts, repl_sc);
    
    gtk_widget_add_controller(GTK_WIDGET(window), GTK_EVENT_CONTROLLER(shortcuts));
    
    /* Open Split Button */
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
    
    /* Main Menu */
    GMenu *main_menu = g_menu_new();
    
    /* Section 1: Window Actions (View, etc) */
    GMenu *s1 = g_menu_new();
    g_menu_append(s1, "New Window", "win.new-window");
    
    /* View Submenu */
    GMenu *view_menu = g_menu_new();
    
    g_menu_append(view_menu, "Split Right", "win.split-right");
    g_menu_append(view_menu, "Split Down", "win.split-down");
    g_menu_append(view_menu, "Close View", "win.close-view");
    
    g_menu_append_submenu(s1, "View", G_MENU_MODEL(view_menu));
    g_object_unref(view_menu);
    
    g_menu_append_section(main_menu, NULL, G_MENU_MODEL(s1));
    g_object_unref(s1);
    
    /* Section 2: App Actions */
    GMenu *s2 = g_menu_new();
    g_menu_append(s2, "Preferences", "win.preferences");
    g_menu_append_section(main_menu, NULL, G_MENU_MODEL(s2));
    g_object_unref(s2);
    
    /* Actions */
    const GActionEntry win_entries[] = {
        { "new-window", on_new_window_action, NULL, NULL, NULL },
        { "split-right", on_split_right, NULL, NULL, NULL },
        { "split-down", on_split_down, NULL, NULL, NULL },
        { "close-view", on_close_split_action, NULL, NULL, NULL },
        { "preferences", on_preferences_action, NULL, NULL, NULL }
    };
    g_action_map_add_action_entries(G_ACTION_MAP(window), win_entries, G_N_ELEMENTS(win_entries), win);
    
    GtkWidget *btn_menu = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(btn_menu), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(btn_menu), G_MENU_MODEL(main_menu));
    gtk_widget_set_tooltip_text(btn_menu, "Main Menu");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), btn_menu);
    g_object_unref(main_menu);

    /* main_tab_bar = NULL; Removed */
    win->tab_bar = VITE_TAB_BAR(vite_tab_bar_new());
    gtk_box_append(GTK_BOX(titlebar_container), GTK_WIDGET(win->tab_bar));

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
    g_object_set_data(G_OBJECT(win->tab_bar), "tabs-btn", btn_tabs);
    g_object_set_data(G_OBJECT(tabs_popover), "list", tabs_list);
    
    g_signal_connect(win->tab_bar, "overflow-changed", G_CALLBACK(on_overflow_changed), btn_tabs);
    g_signal_connect(win->tab_bar, "tab-dropped", G_CALLBACK(on_tab_dropped), win);
    g_signal_connect(tabs_popover, "map", G_CALLBACK(update_open_tabs_list), tabs_list);
    
    /* Initialize Stack */
    win->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(win->stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    
    gtk_window_set_child(window, GTK_WIDGET(win->stack));
    
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
activate(GtkApplication *app, gpointer user_data)
{
    GtkWindow *window = GTK_WINDOW(gtk_application_window_new(app));
    ViteWindow *win = setup_window(window);
    
    gtk_window_set_default_size(window, 800, 600);
    
    Document *doc = document_new(NULL);
    create_new_tab(win, "Untitled", doc);
    
    gtk_window_present(window);
}



static void
open_file(GtkApplication *app, ViteWindow *target_window, GFile *file)
{
    char *path = g_file_get_path(file);
    if (!path) return;

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
                    gtk_window_present(check_win->window);
                            
                    g_free(path);
                    g_list_free(tabs);
                    return;
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
            GtkWindow *window = GTK_WINDOW(gtk_application_window_new(app));
            target_window = setup_window(window);
            gtk_window_set_default_size(window, 800, 600);
            gtk_window_present(window); /* Ensure it's presented? Or done later? */
        }
    }
        
    /* Check if we can reuse the active tab in TARGET window (Untitled & Unmodified) */
    if (TRUE) {
             ViteTab *tab = vite_tab_bar_get_active_tab(target_window->tab_bar);
             if (tab) {
                 GtkWidget *page = g_object_get_data(G_OBJECT(tab), "page");
                 GtkWidget *editor = get_editor_from_page(page);
                 if (EDITOR_IS_WIDGET(editor)) {
                           Document *current_doc = editor_widget_get_document(EDITOR_WIDGET(editor));
                           if (!document_get_file_path(current_doc) && !document_is_modified(current_doc)) {
                               /* Reuse this tab */
                               Document *doc = document_new(path);
                               if (!doc) { 
                                   g_warning("Failed to open %s", path); 
                                   g_free(path); 
                                   return; 
                               }
                               
                               /* Free old doc */
                               document_free(current_doc);
                               
                               /* Set new doc */
                               editor_widget_set_document(EDITOR_WIDGET(editor), doc);
                               
                               /* Setup callbacks */
                               /* Setup callbacks */
                               document_add_modification_callback(doc, on_document_modified, tab);
                               document_add_content_callback(doc, on_document_content_changed, tab);

                               /* Set language for syntax highlighting */
                               const char *dot = strrchr(path, '.');
                               if (dot) editor_widget_set_language(EDITOR_WIDGET(editor), dot + 1);
                               
                               /* Update title */
                               char *name = g_file_get_basename(file);
                               vite_tab_set_title(tab, name);
                               
                               /* Update original_title */
                               g_object_set_data_full(G_OBJECT(tab), "original_title", g_strdup(name), g_free);
                               g_free(name);
                               
                               /* Add to recent files */
                               char *uri = g_file_get_uri(file);
                               add_to_local_recents(uri);
                               g_free(uri);

                               g_free(path);
                               
                               update_window_title_for_tab(tab);
                               gtk_window_present(target_window->window);
                               
                               /* Ensure focus is grabbed safely */
                               defer_focus(editor);
                               
                               return;
                           }
                     }
              }
    }
    
    Document *doc = document_new(path);
    if (!doc) {
        g_warning("Failed to open %s", path);
        g_free(path);
        return;
    }
    char *name = g_file_get_basename(file);
    create_new_tab(target_window, name, doc);
    
    /* Add to recent files */
    char *uri = g_file_get_uri(file);
    add_to_local_recents(uri);
    g_free(uri);

    g_free(name);
    g_free(path);
    
    gtk_window_present(target_window->window);
}

static void
on_open(GtkApplication *app, GFile **files, int n_files, char *hint, gpointer user_data)
{
    for (int i = 0; i < n_files; i++) open_file(app, NULL, files[i]);
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
static GtkWidget *
get_active_overlay(ViteWindow *win) {
    /* Try focused widget first */
    GtkWidget *focus = gtk_window_get_focus(win->window);
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

static void on_find_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
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

static void on_replace_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
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
