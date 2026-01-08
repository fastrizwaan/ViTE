#include "tab.h"

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
"box.chrome-tab {"
"    background: @headerbar_bg_color;"
"    color: alpha(@window_fg_color, 0.85);"
"    min-height: 32px;"
"    padding: 0;"
"    border-radius: 8px 8px 8px 8px;"
"    margin-left: 0px;"
"    margin-bottom: 0px;"
"}"
"box.chrome-tab label {"
"    padding: 0;"
"    margin-top: 1px;"
"    opacity: 0.9;"
"    font-weight: normal;"
"}"
"box.chrome-tab:hover {"
"    color: @window_fg_color;"
"    background: mix(@headerbar_bg_color, @window_fg_color, 0.1);"
"}"
"box.chrome-tab.active {"
"    background: mix(@headerbar_bg_color, @window_fg_color, 0.15);"
"    color: @window_fg_color;"
"}"
"box.chrome-tab.active label {"
"    font-weight: normal;"
"    opacity: 1;"
"}"
"box.chrome-tab.dragging {"
"    opacity: 0.5;"
"}"
".chrome-tab-fade {"
"    background: linear-gradient(to right, transparent 30%, @headerbar_bg_color 100%);"
"    min-width: 15px;"
"    opacity: 1;"
"    transition: opacity 0.1s;"
"}"
"box.chrome-tab:hover .chrome-tab-fade {"
"    background: linear-gradient(to right, transparent 0%, mix(@headerbar_bg_color, @window_fg_color, 0.1) 60%, mix(@headerbar_bg_color, @window_fg_color, 0.1) 100%);"
"}"
"box.chrome-tab.active .chrome-tab-fade {"
"    background: linear-gradient(to right, transparent 0%, mix(@headerbar_bg_color, @window_fg_color, 0.15) 60%, mix(@headerbar_bg_color, @window_fg_color, 0.15) 100%);"
"}"
".chrome-tab-close-button {"
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
".chrome-tab-close-button:hover {"
"    background-color: alpha(@window_fg_color, 0.1);"
"}"
".chrome-tab.active .chrome-tab-close-button {"
"    background-color: transparent;"
"}"
".chrome-tab.active:hover .chrome-tab-close-button {"
"    background-color: transparent;"
"}"
".chrome-tab.active .chrome-tab-close-button:hover {"
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
".chrome-tab-separator {"
"    min-width: 1px;"
"    margin-left: 0px;"
"    margin-right: 0px;"
"    background-color: alpha(@window_fg_color, 0.3);"
"    margin-top: 4px;"
"    margin-bottom: 4px;"
"}"
".chrome-tab.has-drop-target {"
"    border-left: 4px solid #62a0ea;"
"    border-radius: 0 8px 8px 0;"
"}"
".chrome-tab.drop-target-end {"
"    border-right: 4px solid #62a0ea;"
"    border-radius: 8px 0 0 8px;"
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
    /* Use G_TYPE_OBJECT or VITE_TYPE_TAB? 
       gdk_content_provider_new_typed requires a GType and instance. 
       This passes the object pointer. */
    return gdk_content_provider_new_typed(VITE_TYPE_TAB, self);
}
void
vite_tab_set_tab_bar (ViteTab *self, gpointer tab_bar)
{
    /* Store tab bar reference if needed, or just use it for dnd/signals */
    g_object_set_data(G_OBJECT(self), "tab-bar", tab_bar);
}

static void
on_drag_begin (GtkDragSource *source, GdkDrag *drag, ViteTab *self)
{
    GtkWidget *widget = GTK_WIDGET(self);
    GdkPaintable *paintable = gtk_widget_paintable_new(widget);
    gtk_drag_source_set_icon(source, paintable, 0, 0);
    g_object_unref(paintable);
    gtk_widget_add_css_class(widget, "dragging");
}

static void
on_drag_end (GtkDragSource *source, GdkDrag *drag, gboolean delete_data, ViteTab *self)
{
    gtk_widget_remove_css_class(GTK_WIDGET(self), "dragging");
}

static void
on_close_clicked (GtkButton *btn, ViteTab *self)
{
    if (self->loading) return;
    g_signal_emit(self, signals[SIGNAL_CLOSE_CLICKED], 0);
}

static void
on_click_released (GtkGestureClick *gesture, int n_press, double x, double y, ViteTab *self)
{
    /* If we clicked the close button, the button's 'clicked' signal handled it. 
       Use coordinates or something? 
       Actually, GtkButton stops propagation if handled? 
       If GtkButton handles it, this might still fire depending on phase.
    */
    /* Simple check: If close button is hovered/active? */
    /* Let's rely on GtkButton eating the event if it's clicked. */
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
}

static void
on_leave (GtkEventControllerMotion *controller, ViteTab *self)
{
    self->is_hovered = FALSE;
    update_close_button_state(self);
}

static void
vite_tab_init (ViteTab *self)
{
    gtk_widget_add_css_class(GTK_WIDGET(self), "chrome-tab");
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_size_request(GTK_WIDGET(self), 150, 32); 
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE); 
    
    /* Separator (Left) */
    self->separator = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(self->separator, "chrome-tab-separator");
    gtk_widget_set_size_request(self->separator, 1, 20);
    gtk_widget_set_valign(self->separator, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(self->separator, 0);
    gtk_box_append(GTK_BOX(self), self->separator);
    
    /* Overlay */
    self->overlay = GTK_OVERLAY(gtk_overlay_new());
    gtk_widget_set_hexpand(GTK_WIDGET(self->overlay), TRUE);
    gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->overlay));
    
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
    gtk_widget_add_css_class(self->fade_overlay, "chrome-tab-fade");
    gtk_widget_set_can_target(self->fade_overlay, FALSE);
    gtk_overlay_add_overlay(self->overlay, self->fade_overlay);
    
    /* Close Button */
    self->close_button = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(self->close_button, "chrome-tab-close-button");
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
    g_signal_connect(click, "released", G_CALLBACK(on_click_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
    
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_enter), self);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), motion);
}

static void
vite_tab_class_init (ViteTabClass *class)
{
    GObjectClass *object_class = G_OBJECT_CLASS(class);
    object_class->finalize = vite_tab_finalize;
    
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
        
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, TAB_CSS);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
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
        gtk_widget_set_visible(self->separator, visible);
        gtk_widget_set_opacity(self->separator, visible ? 1.0 : 0.0);
    }
}

void
vite_tab_set_drop_indicator (ViteTab *self, gboolean is_target)
{
    if (is_target) {
        gtk_widget_add_css_class(GTK_WIDGET(self), "has-drop-target");
    } else {
        gtk_widget_remove_css_class(GTK_WIDGET(self), "has-drop-target");
    }
}
