#include "tab.h"
#include "tab-bar.h"
#include <math.h>

static void on_context_menu (GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);

struct _ViteTab {
    GtkBox parent_instance;
    
    /* UI Components */
    GtkWidget *separator;
    GtkOverlay *overlay;
    GtkWidget *scroll_wrapper;
    GtkWidget *label;
    GtkWidget *close_button;
    GtkWidget *fade_overlay;
    GtkWidget *spinner;
    GtkWidget *progress_bar;
    
    char *title;
    gboolean is_hovered;
    gboolean is_active;
    gboolean is_modified;
    gboolean loading;
    double anim_offset_x; /* For smooth reorder animation */
    double drag_start_x;  /* Drag start position for ghost icon positioning */
    double drag_start_y;
    
    /* Hybrid drag approach: invisible GTK icon + custom visual overlay */
    GtkWidget *visual_overlay;     /* Custom visual ghost (Y-constrained) */
    GtkWidget *visual_picture;     /* Picture widget inside overlay */
    guint visual_tick_id;          /* Tick callback for updating visual position */
    double cursor_start_y;         /* Cursor Y at drag start (for threshold) */
    double tab_bar_y;              /* Tab bar Y in root coords (for locking) */
    double initial_overlay_y;      /* Initial overlay Y in titlebar coords */
    gboolean is_detached;          /* Whether >20px from start */
};

G_DEFINE_TYPE(ViteTab, vite_tab, GTK_TYPE_BOX)

enum {
    SIGNAL_CLOSE_CLICKED,
    SIGNAL_CLICKED,
    N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

/* CSS ported from vitetab.py + Separator */
static const char *TAB_CSS = 
"box.vite-tab {"
"    background: @headerbar_bg_color;"
"    color: alpha(@window_fg_color, 0.95);"
"    min-height: 32px;"
"    padding: 0;"
"    border-radius: 9px 9px 9px 9px;"
"    margin-left: 0px;"
"    margin-bottom: 0px;"
"}"
"box.vite-tab:drop(active) {"
"    border: none;"
"    box-shadow: none;"
"    outline: none;"
"}"
"flowboxchild:drop(active) {"
"    border: none;"
"    box-shadow: none;"
"    outline: none;"
"}"
"box.vite-tab label {"
"    padding: 0;"
"    margin-top: 1px;"
"    opacity: 0.9;"
"    font-weight: normal;"
"}"
"box.vite-tab:hover {"
"    color: @window_fg_color;"
"    background: mix(@headerbar_bg_color, @window_fg_color, 0.1);"
"}"
"box.vite-tab.active {"
"    background: mix(@headerbar_bg_color, @window_fg_color, 0.15);"
"    color: @window_fg_color;"
"}"
"box.vite-tab.active label {"
"    font-weight: normal;"
"    opacity: 1;"
"}"
"box.vite-tab.dragging {"
"    opacity: 0;"
"}"
".vite-tab-fade {"
"    background: linear-gradient(to right, transparent 30%, @headerbar_bg_color 100%);"
"    min-width: 15px;"
"    opacity: 1;"
"    transition: opacity 0.1s;"
"}"
"box.vite-tab:hover .vite-tab-fade {"
"    background: linear-gradient(to right, transparent 0%, mix(@headerbar_bg_color, @window_fg_color, 0.1) 60%, mix(@headerbar_bg_color, @window_fg_color, 0.1) 100%);"
"}"
"box.vite-tab.active .vite-tab-fade {"
"    background: linear-gradient(to right, transparent 0%, mix(@headerbar_bg_color, @window_fg_color, 0.15) 60%, mix(@headerbar_bg_color, @window_fg_color, 0.15) 100%);"
"}"
".vite-tab-close-button {"
"    min-width: 20px;"
"    min-height: 20px;"
"    padding: 2px;"
"    margin: 0;"
"    margin-right: 2px;"
"    opacity: 1.0;"
"    border-radius: 50%;"
"    background-color: transparent;"
"    color: @window_fg_color;"
"}"
".vite-tab-close-button:hover {"
"    background-color: alpha(@window_fg_color, 0.1);"
"}"
".vite-tab.active .vite-tab-close-button {"
"    background-color: transparent;"
"}"
".vite-tab.active:hover .vite-tab-close-button {"
"    background-color: transparent;"
"}"
".vite-tab.active .vite-tab-close-button:hover {"
"    background-color: alpha(@window_fg_color, 0.1);"
"}"
".progress-bar {"
"    min-height: 2px;"
"    margin-top: 30px;"
"}"
".progress-bar trough {"
"    min-height: 2px;"
"    background: transparent;"
"    border: none;"
"}"
".progress-bar progress {"
"    min-height: 2px;"
"    background-color: #62a0ea;"
"    border-radius: 0;"
"}"
".vite-tab-separator {"
"    min-width: 1px;"
"    margin-left: 0px;"
"    margin-right: 0px;"
"    background-color: alpha(@window_fg_color, 0.18);"
"    margin-top: 2px;"
"    margin-bottom: 2px;"
"}"
".drag-ghost {"
"    opacity: 0.9;"
"    box-shadow: 0 4px 12px rgba(0, 0, 0, 0.3);"
"}";


static void
vite_tab_finalize (GObject *object)
{
    ViteTab *self = VITE_TAB(object);
    g_free(self->title);
    G_OBJECT_CLASS(vite_tab_parent_class)->finalize(object);
}

static GdkContentProvider *
on_drag_prepare (GtkDragSource *source, double x, double y, ViteTab *self)
{
    /* Store drag start position for use in on_drag_begin */
    self->drag_start_x = x;
    self->drag_start_y = y;
    
    return gdk_content_provider_new_typed(VITE_TYPE_TAB, self);
}
void
vite_tab_set_tab_bar (ViteTab *self, gpointer tab_bar)
{
    /* Store tab bar reference if needed, or just use it for dnd/signals */
    g_object_set_data(G_OBJECT(self), "tab-bar", tab_bar);
}

/* Tick callback for visual overlay - applies Y constraints */
static gboolean
on_visual_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    ViteTab *self = VITE_TAB(user_data);
    
    if (!self->visual_overlay) {
        return G_SOURCE_REMOVE;
    }
    
    GtkWidget *root = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));
    if (!root) return G_SOURCE_CONTINUE;
    
    GtkWidget *titlebar_overlay = gtk_window_get_titlebar(GTK_WINDOW(root));
    if (!titlebar_overlay) return G_SOURCE_CONTINUE;
    
    /* Get cursor position in root coords */
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(root));
    GdkDevice *device = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_surface_get_display(surface)));
    double cursor_x, cursor_y;
    gboolean inside = gdk_surface_get_device_position(surface, device, &cursor_x, &cursor_y, NULL);
    
    if (!inside) {
        self->is_detached = TRUE;
        /* If outside, we can't really update the visual overlay position relative to window easily, 
           but that's fine, the cursor has the drag icon. 
           We just mark it detached so drop triggers new window. */
        return G_SOURCE_CONTINUE;
    }
    
    /* Check threshold for detachment */
    double v_offset = fabs(cursor_y - self->cursor_start_y);
    
    /* Also check if outside window bounds */
    int win_width = gtk_widget_get_width(root);
    int win_height = gtk_widget_get_height(root);
    
    if (v_offset > 20.0 || 
        cursor_x < 0 || cursor_x > win_width || 
        cursor_y < 0 || cursor_y > win_height) {
        self->is_detached = TRUE;
    } else if (fabs(cursor_y - self->tab_bar_y) < 5.0) {
        /* Re-attach if cursor comes back near tab bar */
        self->is_detached = FALSE;
    }
    
    /* Convert cursor to titlebar overlay coords */
    graphene_point_t cursor_in_root = GRAPHENE_POINT_INIT(cursor_x, cursor_y);
    graphene_point_t cursor_in_titlebar;
    if (!gtk_widget_compute_point(root, titlebar_overlay, &cursor_in_root, &cursor_in_titlebar)) {
        return G_SOURCE_CONTINUE;
    }
    
    /* Apply Y constraint */
    double final_x = cursor_in_titlebar.x - self->drag_start_x;
    double final_y;
    
    if (self->is_detached) {
        /* Free movement */
        final_y = cursor_in_titlebar.y - self->drag_start_y;
    } else {
        /* Locked to tab bar line */
        final_y = self->initial_overlay_y;
    }
    
    /* Update visual overlay position */
    gtk_fixed_move(GTK_FIXED(gtk_widget_get_parent(self->visual_overlay)),
                   self->visual_overlay, final_x, final_y);
    
    return G_SOURCE_CONTINUE;
}

static void
on_drag_begin (GtkDragSource *source, GdkDrag *drag, ViteTab *self)
{
    /* Activate this tab immediately when starting to drag */
    g_signal_emit(self, signals[SIGNAL_CLICKED], 0);
    
    g_print("[DRAG] Starting drag\n");
    
    GtkWidget *widget = GTK_WIDGET(self);
    GtkWidget *root = GTK_WIDGET(gtk_widget_get_root(widget));
    
    /* Create the drag icon snapshot before hiding the tab */
    GtkWidgetPaintable *widget_paintable = GTK_WIDGET_PAINTABLE(gtk_widget_paintable_new(widget));
    GdkPaintable *static_paintable = gdk_paintable_get_current_image(GDK_PAINTABLE(widget_paintable));
    
    /* Use real GTK drag icon (for drop targets) with CSS for visual Y-constraint */
    GdkPaintable *icon_paintable = gdk_paintable_get_current_image(GDK_PAINTABLE(widget_paintable));
    
    /* Get the drag icon widget that GTK will create */
    GtkWidget *drag_icon = gtk_drag_icon_get_for_drag(drag);
    if (drag_icon) {
        /* Add CSS class for transform-based Y constraint */
        gtk_widget_add_css_class(drag_icon, "tab-drag-constrained");
    }
    
    gtk_drag_source_set_icon(source, icon_paintable, self->drag_start_x, self->drag_start_y);
    
    g_object_unref(icon_paintable);
    
    g_object_unref(static_paintable);
    g_object_unref(widget_paintable);
    
    /* Notify tab bar that this tab is being dragged */
    ViteTabBar *tab_bar = g_object_get_data(G_OBJECT(self), "tab-bar");
    if (tab_bar) {
        vite_tab_bar_set_dragging_tab(tab_bar, self);
    }
    
    /* Hide the tab content with CSS (wrapper stays visible = shows empty space) */
    gtk_widget_add_css_class(widget, "dragging");
}

static void
on_drag_end (GtkDragSource *source, GdkDrag *drag, gboolean delete_data, ViteTab *self)
{
    g_print("[DRAG] Drag ended, delete_data=%d\n", delete_data);
    /* Remove visual overlay */
    if (self->visual_tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->visual_tick_id);
        self->visual_tick_id = 0;
    }
    
    if (self->visual_overlay) {
        GtkWidget *parent = gtk_widget_get_parent(self->visual_overlay);
        if (parent) {
            gtk_fixed_remove(GTK_FIXED(parent), self->visual_overlay);
        }
        self->visual_overlay = NULL;
        self->visual_picture = NULL;
    }
    
    /* Notify tab bar that drag ended */
    ViteTabBar *tab_bar = g_object_get_data(G_OBJECT(self), "tab-bar");
    if (tab_bar) {
        gboolean moved_to_new = FALSE;
        
        /* Robustness Check: Verify cursor position right now */
        GtkWidget *root = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(self)));
        if (root) {
             GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(root));
             GdkDevice *device = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_surface_get_display(surface)));
             double cursor_x, cursor_y;
             gboolean inside = gdk_surface_get_device_position(surface, device, &cursor_x, &cursor_y, NULL);
             
             if (!inside) {
                 g_print("[DRAG] Drag end: Cursor outside surface -> Force Detach\n");
                 self->is_detached = TRUE;
             } else {
                 /* Check bounds manually too */
                 int w = gtk_widget_get_width(root);
                 int h = gtk_widget_get_height(root);
                 if (cursor_x < 0 || cursor_x > w || cursor_y < 0 || cursor_y > h) {
                     g_print("[DRAG] Drag end: Cursor outside bounds (%.1f, %.1f) -> Force Detach\n", cursor_x, cursor_y);
                     self->is_detached = TRUE;
                 }
             }
        }
    
        /* If drag was not accepted (delete_data is FALSE) AND we are detached > 20px */
        if (!delete_data && self->is_detached) {
            g_print("[DRAG] Drag ended outside - triggering move-to-new-window\n");
            g_signal_emit_by_name(self, "move-to-new-window");
            moved_to_new = TRUE;
        }
    
        vite_tab_bar_clear_dragging_tab(tab_bar, delete_data || moved_to_new);
    }
    
    self->is_detached = FALSE;

    /* Remove the dragging CSS class */
    gtk_widget_remove_css_class(GTK_WIDGET(self), "dragging");
}

static void
on_close_clicked (GtkButton *btn, ViteTab *self)
{
    if (self->loading) return;
    g_signal_emit(self, signals[SIGNAL_CLOSE_CLICKED], 0);
}

static void
on_click_pressed (GtkGestureClick *gesture, int n_press, double x, double y, ViteTab *self)
{
    /* Activate tab immediately on mouse button press */
    g_signal_emit(self, signals[SIGNAL_CLICKED], 0);
}

static void
update_close_button_state (ViteTab *self)
{
    if (self->is_active) {
        gtk_widget_set_opacity(self->close_button, 1.0);
        return;
    }
    
    if (self->is_hovered) {
        gtk_widget_set_opacity(self->close_button, self->is_modified ? 1.0 : 0.9);
    } else {
        gtk_widget_set_opacity(self->close_button, 0.0);
    }
}

static void
on_enter (GtkEventControllerMotion *controller, double x, double y, ViteTab *self)
{
    self->is_hovered = TRUE;
    update_close_button_state(self);
    
    // Update separators to hide adjacent ones
    ViteTabBar *tab_bar = g_object_get_data(G_OBJECT(self), "tab-bar");
    if (tab_bar) {
        vite_tab_bar_update_separators(tab_bar);
    }
}

static void
on_leave (GtkEventControllerMotion *controller, ViteTab *self)
{
    self->is_hovered = FALSE;
    update_close_button_state(self);
    
    // Restore separators
    ViteTabBar *tab_bar = g_object_get_data(G_OBJECT(self), "tab-bar");
    if (tab_bar) {
        vite_tab_bar_update_separators(tab_bar);
    }
}

/* Drop target handlers for tab reordering */
static GdkDragAction
on_drop_enter (GtkDropTarget *target, double x, double y, ViteTab *self)
{
    g_print("[TAB DROP] Enter on tab\n");
    return GDK_ACTION_MOVE;
}

static GdkDragAction
on_drop_motion (GtkDropTarget *target, double x, double y, ViteTab *self)
{
    const GValue *value = gtk_drop_target_get_value(target);
    if (!value || !G_VALUE_HOLDS(value, VITE_TYPE_TAB)) return 0;
    
    ViteTab *dragged_tab = VITE_TAB(g_value_get_object(value));
    if (!GTK_IS_WIDGET(dragged_tab) || dragged_tab == self) return GDK_ACTION_MOVE;
    
    ViteTabBar *tab_bar = g_object_get_data(G_OBJECT(self), "tab-bar");
    if (!tab_bar) return 0;
    
    /* Get my position in the tab bar */
    GList *tabs = vite_tab_bar_get_tabs(tab_bar);
    int my_pos = g_list_index(tabs, self);
    g_list_free(tabs);
    
    if (my_pos >= 0) {
        /* Reorder the dragged tab to this tab's position */
        vite_tab_bar_reorder_tab_to(tab_bar, dragged_tab, my_pos);
    }
    
    /* Handle edge scrolling - check position in tab bar widget */
    if (vite_tab_bar_is_overflowing(tab_bar)) {
        graphene_rect_t bounds;
        if (gtk_widget_compute_bounds(GTK_WIDGET(self), GTK_WIDGET(tab_bar), &bounds)) {
            double tab_x = bounds.origin.x + x;
            int bar_width = gtk_widget_get_width(GTK_WIDGET(tab_bar));
            int edge_zone = 40;
            
            if (tab_x < edge_zone) {
                vite_tab_bar_start_edge_scroll(tab_bar, -1);
            } else if (tab_x > bar_width - edge_zone) {
                vite_tab_bar_start_edge_scroll(tab_bar, 1);
            } else {
                vite_tab_bar_stop_edge_scroll(tab_bar);
            }
        }
    }
    
    return GDK_ACTION_MOVE;
}

static void
on_drop_leave (GtkDropTarget *target, ViteTab *self)
{
    ViteTabBar *tab_bar = g_object_get_data(G_OBJECT(self), "tab-bar");
    if (tab_bar) {
        vite_tab_bar_stop_edge_scroll(tab_bar);
    }
}

static gboolean
on_drop_drop (GtkDropTarget *target, const GValue *value, double x, double y, ViteTab *self)
{
    g_print("\n[DROP] ========== DROP OCCURRED ==========\n");
    g_print("[DROP] x=%.1f y=%.1f value=%p\n", x, y, value);
    
    /* Stop edge scrolling */
    ViteTabBar *tab_bar = g_object_get_data(G_OBJECT(self), "tab-bar");
    if (tab_bar) {
        g_print("[DROP] Stopping edge scroll\n");
        vite_tab_bar_stop_edge_scroll(tab_bar);
    }
    
    /* The tab was already reordered during motion, just return success */
    if (!value || !G_VALUE_HOLDS(value, VITE_TYPE_TAB)) {
        g_print("[DROP] Invalid value, returning FALSE\n");
        return FALSE;
    }
    
    if (tab_bar) {
        /* Check if tab is foreign by looking for it in my tab bar's list */
        ViteTab *dragged_tab = VITE_TAB(g_value_get_object(value));
        GList *tabs = vite_tab_bar_get_tabs(tab_bar);
        gboolean is_local = (g_list_find(tabs, dragged_tab) != NULL);
        
        if (!is_local) {
             /* Foreign drop! Emit signal on my tab bar */
             /* Get my position to drop *before* or *after* me?
                Usually drop on left half = before, right half = after.
                For simplicity, let's insert at my position (pushing me right) */
             int my_pos = g_list_index(tabs, self);
             g_print("[TAB DROP] Foreign tab dropped on non-empty bar at pos %d\n", my_pos);
             vite_tab_bar_drop_foreign_tab(tab_bar, dragged_tab, my_pos);
             g_list_free(tabs);
             return TRUE;
        }
        g_list_free(tabs);
    
        vite_tab_bar_notify_drop_done(tab_bar);
    }
    
    g_print("[DROP] Returning TRUE - drop accepted\n");
    g_print("[DROP] ========== DROP COMPLETE ==========\n\n");
    return TRUE;
}

static void
vite_tab_init (ViteTab *self)
{
    gtk_widget_add_css_class(GTK_WIDGET(self), "vite-tab");
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_size_request(GTK_WIDGET(self), 150, 32); 
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE); 
    
    /* Overlay */
    self->overlay = GTK_OVERLAY(gtk_overlay_new());
    gtk_widget_set_hexpand(GTK_WIDGET(self->overlay), TRUE);
    gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->overlay));
    
    /* Separator (Right) - Appended AFTER overlay so it appears on the right */
    self->separator = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(self->separator, "vite-tab-separator");
    gtk_widget_set_size_request(self->separator, 1, 20);
    gtk_widget_set_valign(self->separator, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(self->separator, 0);
    gtk_box_append(GTK_BOX(self), self->separator);
    
    /* Scrolled Wrapper for Label */
    self->scroll_wrapper = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scroll_wrapper), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_has_frame(GTK_SCROLLED_WINDOW(self->scroll_wrapper), FALSE);
    gtk_widget_set_hexpand(self->scroll_wrapper, TRUE);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(self->scroll_wrapper), FALSE);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(self->scroll_wrapper), 1);
    gtk_widget_set_can_target(self->scroll_wrapper, FALSE); 
    gtk_widget_set_margin_start(self->scroll_wrapper, 8);
    gtk_widget_set_margin_end(self->scroll_wrapper, 8);
    
    self->label = gtk_label_new("Untitled");
    gtk_label_set_ellipsize(GTK_LABEL(self->label), PANGO_ELLIPSIZE_NONE);
    gtk_label_set_single_line_mode(GTK_LABEL(self->label), TRUE);
    gtk_widget_set_hexpand(self->label, TRUE);
    gtk_widget_set_halign(self->label, GTK_ALIGN_CENTER);
    
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroll_wrapper), self->label);
    gtk_overlay_set_child(self->overlay, self->scroll_wrapper);
    
    /* Fade Overlay */
    self->fade_overlay = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(self->fade_overlay, GTK_ALIGN_END);
    gtk_widget_set_hexpand(self->fade_overlay, FALSE);
    gtk_widget_set_size_request(self->fade_overlay, 30, -1);
    gtk_widget_add_css_class(self->fade_overlay, "vite-tab-fade");
    gtk_widget_set_can_target(self->fade_overlay, FALSE);
    gtk_overlay_add_overlay(self->overlay, self->fade_overlay);
    
    /* Close Button */
    self->close_button = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(self->close_button, "vite-tab-close-button");
    gtk_widget_set_halign(self->close_button, GTK_ALIGN_END);
    gtk_widget_set_valign(self->close_button, GTK_ALIGN_CENTER);
    gtk_widget_set_opacity(self->close_button, 0);
    g_signal_connect(self->close_button, "clicked", G_CALLBACK(on_close_clicked), self);
    gtk_overlay_add_overlay(self->overlay, self->close_button);
    gtk_overlay_set_measure_overlay(self->overlay, self->close_button, FALSE);
    
    /* Spinner */
    self->spinner = gtk_spinner_new();
    gtk_widget_set_halign(self->spinner, GTK_ALIGN_START);
    gtk_widget_set_valign(self->spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(self->spinner, 6);
    gtk_overlay_add_overlay(self->overlay, self->spinner);
    gtk_overlay_set_measure_overlay(self->overlay, self->spinner, FALSE);
    
    /* Progress Bar */
    self->progress_bar = gtk_progress_bar_new();
    gtk_widget_set_valign(self->progress_bar, GTK_ALIGN_END);
    gtk_widget_add_css_class(self->progress_bar, "progress-bar");
    gtk_widget_set_visible(self->progress_bar, FALSE);
    gtk_overlay_add_overlay(self->overlay, self->progress_bar);
    gtk_overlay_set_measure_overlay(self->overlay, self->progress_bar, FALSE);
    
    GtkDragSource *drag_source = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag_source, GDK_ACTION_MOVE);
    g_signal_connect(drag_source, "prepare", G_CALLBACK(on_drag_prepare), self);
    g_signal_connect(drag_source, "drag-begin", G_CALLBACK(on_drag_begin), self);
    g_signal_connect(drag_source, "drag-end", G_CALLBACK(on_drag_end), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drag_source));
    
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
    
    GtkGesture *right_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect(right_click, "pressed", G_CALLBACK(on_context_menu), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(right_click));
    
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_enter), self);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), motion);
    
    /* Drop target for receiving dragged tabs */
    GtkDropTarget *drop_target = gtk_drop_target_new(VITE_TYPE_TAB, GDK_ACTION_MOVE);
    gtk_drop_target_set_preload(drop_target, TRUE);
    g_signal_connect(drop_target, "enter", G_CALLBACK(on_drop_enter), self);
    g_signal_connect(drop_target, "motion", G_CALLBACK(on_drop_motion), self);
    g_signal_connect(drop_target, "leave", G_CALLBACK(on_drop_leave), self);
    g_signal_connect(drop_target, "drop", G_CALLBACK(on_drop_drop), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drop_target));
}

static void
vite_tab_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
    ViteTab *self = VITE_TAB(widget);
    
    if (self->anim_offset_x != 0) {
        gtk_snapshot_save(snapshot);
        gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(self->anim_offset_x, 0));
        GTK_WIDGET_CLASS(vite_tab_parent_class)->snapshot(widget, snapshot);
        gtk_snapshot_restore(snapshot);
    } else {
        GTK_WIDGET_CLASS(vite_tab_parent_class)->snapshot(widget, snapshot);
    }
}

static void
vite_tab_class_init (ViteTabClass *class)
{
    GObjectClass *object_class = G_OBJECT_CLASS(class);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);
    
    object_class->finalize = vite_tab_finalize;
    widget_class->snapshot = vite_tab_snapshot;
    
    signals[SIGNAL_CLOSE_CLICKED] = g_signal_new("close-clicked",
        G_TYPE_FROM_CLASS(class),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    signals[SIGNAL_CLICKED] = g_signal_new("clicked",
        G_TYPE_FROM_CLASS(class),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
        
    g_signal_new("move-to-new-window",
        G_TYPE_FROM_CLASS(class),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);
        
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, TAB_CSS);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static void
on_context_menu_move_left (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteTab *self = VITE_TAB(user_data);
    ViteTabBar *tab_bar = g_object_get_data(G_OBJECT(self), "tab-bar");
    if (!tab_bar) return;
    
    GList *tabs = vite_tab_bar_get_tabs(tab_bar);
    int idx = g_list_index(tabs, self);
    g_list_free(tabs);
    
    if (idx > 0) {
        vite_tab_bar_reorder_tab_to(tab_bar, self, idx - 1);
    }
}

static void
on_context_menu_move_right (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteTab *self = VITE_TAB(user_data);
    ViteTabBar *tab_bar = g_object_get_data(G_OBJECT(self), "tab-bar");
    if (!tab_bar) return;
    
    GList *tabs = vite_tab_bar_get_tabs(tab_bar);
    int idx = g_list_index(tabs, self);
    int n_tabs = g_list_length(tabs);
    g_list_free(tabs);
    
    if (idx < n_tabs - 1) {
        vite_tab_bar_reorder_tab_to(tab_bar, self, idx + 1);
    }
}

static void
on_context_menu_move_new_window (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteTab *self = VITE_TAB(user_data);
    g_signal_emit_by_name(self, "move-to-new-window");
}

static void
on_context_menu_close_left (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteTab *self = VITE_TAB(user_data);
    ViteTabBar *tab_bar = g_object_get_data(G_OBJECT(self), "tab-bar");
    if (!tab_bar) return;
    
    GList *tabs = vite_tab_bar_get_tabs(tab_bar);
    int self_idx = g_list_index(tabs, self);
    
    /* Iterate safely using a copy or careful index handling since we are removing */
    /* Remove tabs 0 to self_idx-1 */
    int i = 0;
    GList *l = tabs;
    while (l && i < self_idx) {
        ViteTab *t = VITE_TAB(l->data);
        l = l->next;
        /* Emit close-clicked signal on the tab to trigger proper cleanup in main.c */
        g_signal_emit_by_name(t, "close-clicked");
        i++;
    }
    g_list_free(tabs);
}

static void
on_context_menu_close_right (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteTab *self = VITE_TAB(user_data);
    ViteTabBar *tab_bar = g_object_get_data(G_OBJECT(self), "tab-bar");
    if (!tab_bar) return;
    
    GList *tabs = vite_tab_bar_get_tabs(tab_bar);
    int self_idx = g_list_index(tabs, self);
    
    /* Remove all tabs after self */
    GList *l = g_list_nth(tabs, self_idx + 1);
    while (l) {
        ViteTab *t = VITE_TAB(l->data);
        l = l->next; /* Get next before potential destruction */
        g_signal_emit_by_name(t, "close-clicked");
    }
    g_list_free(tabs);
}

static void
on_context_menu_close_other (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteTab *self = VITE_TAB(user_data);
    ViteTabBar *tab_bar = g_object_get_data(G_OBJECT(self), "tab-bar");
    if (!tab_bar) return;
    
    GList *tabs = vite_tab_bar_get_tabs(tab_bar);
    for (GList *l = tabs; l != NULL; l = l->next) {
        ViteTab *t = VITE_TAB(l->data);
        if (t != self) {
            g_signal_emit_by_name(t, "close-clicked");
        }
    }
    g_list_free(tabs);
}

static void
on_context_menu_close (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteTab *self = VITE_TAB(user_data);
    g_signal_emit_by_name(self, "close-clicked");
}

static void on_context_menu (GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);

static void
on_context_menu (GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
    ViteTab *self = VITE_TAB(user_data);
    
    GMenu *menu = g_menu_new();
    GSimpleActionGroup *group = g_simple_action_group_new();
    
    /* Section 1: Movement */
    GMenu *s1 = g_menu_new();
    g_menu_append(s1, "Move Left", "ctx.move-left");
    g_menu_append(s1, "Move Right", "ctx.move-right");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s1));
    g_object_unref(s1);
    
    GSimpleAction *act_ml = g_simple_action_new("move-left", NULL);
    g_signal_connect(act_ml, "activate", G_CALLBACK(on_context_menu_move_left), self);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act_ml));
    
    GSimpleAction *act_mr = g_simple_action_new("move-right", NULL);
    g_signal_connect(act_mr, "activate", G_CALLBACK(on_context_menu_move_right), self);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act_mr));
    
    /* Section 2: Split View & New Window */
    /* Section 2: Split View & New Window */
    GMenu *s2 = g_menu_new();
    
    GMenu *s2_split = g_menu_new();
    g_menu_append(s2_split, "Split Right", "win.split-mode::right");
    g_menu_append(s2_split, "Split Down", "win.split-mode::down");
    g_menu_append(s2_split, "Close Split", "win.split-mode::none");
    
    g_menu_append_submenu(s2, "Split", G_MENU_MODEL(s2_split));
    g_object_unref(s2_split);
    
    g_menu_append(s2, "Move to New Window", "ctx.new-window");
    
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s2));
    g_object_unref(s2);
    
    /* Split actions - Handled by window actions (win.split-right/down) */

    GSimpleAction *act_nw = g_simple_action_new("new-window", NULL);
    g_signal_connect(act_nw, "activate", G_CALLBACK(on_context_menu_move_new_window), self);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act_nw));

    /* Section 3: Closing */
    GMenu *s3 = g_menu_new();
    g_menu_append(s3, "Close Tabs to Left", "ctx.close-left");
    g_menu_append(s3, "Close Tabs to Right", "ctx.close-right");
    g_menu_append(s3, "Close Other Tabs", "ctx.close-other");
    g_menu_append(s3, "Close", "ctx.close");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s3));
    g_object_unref(s3);
    
    GSimpleAction *act_cl = g_simple_action_new("close-left", NULL);
    g_signal_connect(act_cl, "activate", G_CALLBACK(on_context_menu_close_left), self);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act_cl));
    
    GSimpleAction *act_cr = g_simple_action_new("close-right", NULL);
    g_signal_connect(act_cr, "activate", G_CALLBACK(on_context_menu_close_right), self);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act_cr));
    
    GSimpleAction *act_co = g_simple_action_new("close-other", NULL);
    g_signal_connect(act_co, "activate", G_CALLBACK(on_context_menu_close_other), self);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act_co));
    
    GSimpleAction *act_c = g_simple_action_new("close", NULL);
    g_signal_connect(act_c, "activate", G_CALLBACK(on_context_menu_close), self);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(act_c));

    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_widget_set_parent(popover, GTK_WIDGET(self));
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
    
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
    
    gtk_widget_insert_action_group(popover, "ctx", G_ACTION_GROUP(group));
    gtk_popover_popup(GTK_POPOVER(popover));
    
    g_object_unref(menu);
    g_object_unref(group);
}

GtkWidget *
vite_tab_new (const char *title)
{
    ViteTab *self = g_object_new(VITE_TYPE_TAB, NULL);
    vite_tab_set_title(self, title);
    return GTK_WIDGET(self);
}

const char *
vite_tab_get_title (ViteTab *self)
{
    return self->title;
}

void
vite_tab_set_title (ViteTab *self, const char *title)
{
    g_free(self->title);
    self->title = g_strdup(title);
    gtk_label_set_text(GTK_LABEL(self->label), title);
    gtk_widget_set_tooltip_text(GTK_WIDGET(self), title);
    
    /* Re-apply modification state to update label */
    vite_tab_set_modified(self, self->is_modified);
}

gboolean
vite_tab_is_active (ViteTab *self)
{
    return self->is_active;
}

void
vite_tab_set_active (ViteTab *self, gboolean active)
{
    self->is_active = active;
    if (active) {
        gtk_widget_add_css_class(GTK_WIDGET(self), "active");
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(self), "active");
    }
    update_close_button_state(self);
}

void
vite_tab_set_separator_visible (ViteTab *self, gboolean visible)
{
    if (self->separator) {
        /* Always keep visible for layout stability, just toggle opacity */
        gtk_widget_set_opacity(self->separator, visible ? 1.0 : 0.0);
    }
}

void
vite_tab_set_anim_offset_x (ViteTab *self, double offset)
{
    if (self->anim_offset_x != offset) {
        self->anim_offset_x = offset;
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

double
vite_tab_get_anim_offset_x (ViteTab *self)
{
    return self->anim_offset_x;
}

gboolean
vite_tab_is_hovered (ViteTab *self)
{
    return self->is_hovered;
}

void
vite_tab_set_modified(ViteTab *self, gboolean modified)
{
    self->is_modified = modified;
    
    /* Update label with bullet if modified */
    if (modified) {
        char *safe_title = g_markup_escape_text(self->title, -1);
        /* Use smaller font size for the dot U+25CF */
        char *markup = g_strdup_printf("<span size='smaller'>●</span> %s", safe_title);
        gtk_label_set_markup(GTK_LABEL(self->label), markup);
        g_free(markup);
        g_free(safe_title);
    } else {
        gtk_label_set_text(GTK_LABEL(self->label), self->title);
    }
    
    update_close_button_state(self);
}
