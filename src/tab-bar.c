#include "tab-bar.h"

struct _ViteTabBar {
    GtkBox parent_instance;
    GtkWidget *scroller;
    GtkWidget *flowbox;
    
    GtkWidget *start_button;
    GtkWidget *end_button;
    
    GList *tabs;
    
    int last_allocated_width;
    int cached_cols;
    gboolean is_overflowing;
    
    guint drag_autoscroll_id;
    int drag_scroll_direction;
    
    ViteTab *dragging_tab;
    int drag_original_pos;
    gboolean drop_occurred;
};

G_DEFINE_TYPE(ViteTabBar, vite_tab_bar, GTK_TYPE_BOX)

enum {
    SIGNAL_OVERFLOW_CHANGED,
    SIGNAL_TAB_DROPPED,
    N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

static const char *TAB_BAR_CSS = 
".vite-tab-bar-container:drop(active), .vite-tab-bar:drop(active) {"
"    border: none;"
"    box-shadow: none;"
"    background: none;"
"    outline: none;"
"}"
".vite-tab-bar-container {"
"    margin-top: 1px;"
"    padding: 0;"
"    margin-bottom: 0px;"
"}"
".vite-tab-bar {"
"    padding-left: 6px;"
"    padding-right: 6px;"
"    padding-top: 0;"
"    padding-bottom: 0;"
"    margin: 0;"
"}"
"flowboxchild {"
"    padding: 0;"
"    margin: 0;"
"    background: none;"
"    border: none;"
"    outline: none;"
"}"
".tab-bar-nav-button {"
"    min-width: 24px;"
"    min-height: 32px;"
"    padding: 0;"
"    margin: 0;"
"    margin-bottom: 0px;"
"    background: none;"
"    border: none;"
"    border-radius: 4px;"
"    opacity: 0.7;"
"}"
".tab-bar-nav-button:hover {"
"    background: alpha(@window_fg_color, 0.1);"
"    opacity: 1.0;"
"}"
".tab-bar-nav-button:disabled {"
"    opacity: 0.3;"
"}";




static void
vite_tab_bar_finalize (GObject *object)
{
    ViteTabBar *self = VITE_TAB_BAR(object);
    
    /* Stop autoscroll timer if running */
    if (self->drag_autoscroll_id) {
        g_source_remove(self->drag_autoscroll_id);
        self->drag_autoscroll_id = 0;
    }
    
    g_list_free(self->tabs);
    G_OBJECT_CLASS(vite_tab_bar_parent_class)->finalize(object);
}

void
vite_tab_bar_update_separators (ViteTabBar *self)
{
    if (!self->tabs) return;
    
    int active_index = -1;
    int hovered_index = -1;
    int idx = 0;
    
    for (GList *l = self->tabs; l != NULL; l = l->next) {
        ViteTab *t = VITE_TAB(l->data);
        if (gtk_widget_has_css_class(GTK_WIDGET(t), "active")) {
            active_index = idx;
        }
        if (vite_tab_is_hovered(t)) {
            hovered_index = idx;
        }
        idx++;
    }
    
    int total_tabs = 0;
    for (GList *l = self->tabs; l != NULL; l = l->next) {
        total_tabs++;
    }

    idx = 0;
    for (GList *l = self->tabs; l != NULL; l = l->next) {
        ViteTab *t = VITE_TAB(l->data);
        if (!GTK_IS_WIDGET(t)) { idx++; continue; }
        
        gboolean visible = TRUE;
        
        /* Rule 1: No separator for end of last tab */
        if (idx == total_tabs - 1) visible = FALSE;
        
        /* Rule 2: Active Tab should have no separators on either side.
           Since we are controlling the RIGHT separator:
           - Hide RIGHT separator of the Active Tab.
           - Hide RIGHT separator of the Previous Tab (Active - 1). 
        */
        if (idx == active_index) visible = FALSE;
        if (idx == active_index - 1) visible = FALSE;
        
        /* Rule 3: Hovered Tab logic (same as active) */
        if (idx == hovered_index) visible = FALSE;
        if (idx == hovered_index - 1) visible = FALSE;
        
        vite_tab_set_separator_visible(t, visible);
        idx++;
    }
}

static void
update_tab_sizes (ViteTabBar *self)
{
    if (!self->tabs) return;
    
    int num_tabs = g_list_length(self->tabs);
    if (num_tabs == 0) return;
    
    /* Force single line layout */
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->flowbox), num_tabs);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->flowbox), num_tabs);
    
    for (GList *l = self->tabs; l != NULL; l = l->next) {
        GtkWidget *tab = GTK_WIDGET(l->data);
        if (!GTK_IS_WIDGET(tab)) continue;
        
        /* Set flexible size request to allow shrinking */
        gtk_widget_set_size_request(tab, 90, 32);
        gtk_widget_set_hexpand(tab, TRUE);
        gtk_widget_set_halign(tab, GTK_ALIGN_FILL);
        
        /* Ensure FlowBoxChild wrapper also expands */
        GtkWidget *parent = gtk_widget_get_parent(tab);
        if (parent && GTK_IS_FLOW_BOX_CHILD(parent)) {
             gtk_widget_set_hexpand(parent, TRUE);
             gtk_widget_set_halign(parent, GTK_ALIGN_FILL);
        }
    }
    
    vite_tab_bar_update_separators(self);
}

static gboolean
update_buttons_idle (gpointer user_data)
{
    ViteTabBar *self = VITE_TAB_BAR(user_data);
    if (!self->scroller || !self->start_button || !self->end_button) return G_SOURCE_REMOVE;
    
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroller));
    double upper = gtk_adjustment_get_upper(adj);
    double page_size = gtk_adjustment_get_page_size(adj);
    double value = gtk_adjustment_get_value(adj);
    
    gboolean overflowing = (upper > page_size + 0.1);
    
    gtk_widget_set_visible(self->start_button, overflowing);
    gtk_widget_set_visible(self->end_button, overflowing);
    
    if (overflowing) {
        gtk_widget_set_sensitive(self->start_button, value > 0.1);
        gtk_widget_set_sensitive(self->end_button, value < (upper - page_size - 0.1));
    }
    
    return G_SOURCE_REMOVE;
}

static void
update_buttons (ViteTabBar *self)
{
    /* Defer to idle to avoid layout conflicts during resize */
    g_idle_add(update_buttons_idle, self);
}

static void
check_overflow (ViteTabBar *self)
{
    update_buttons(self);
    
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroller));
    double upper = gtk_adjustment_get_upper(adj);
    double page_size = gtk_adjustment_get_page_size(adj);
    
    gboolean overflowing = (upper > page_size + 0.1); /* Epsilon */
    
    if (overflowing != self->is_overflowing) {
        self->is_overflowing = overflowing;
        g_signal_emit(self, signals[SIGNAL_OVERFLOW_CHANGED], 0, overflowing);
    }
}




static gboolean
drag_autoscroll_tick (gpointer user_data)
{
    ViteTabBar *self = VITE_TAB_BAR(user_data);
    if (!self->scroller || self->drag_scroll_direction == 0) {
        self->drag_autoscroll_id = 0;
        return G_SOURCE_REMOVE;
    }
    
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroller));
    double value = gtk_adjustment_get_value(adj);
    double step = 80.0 * self->drag_scroll_direction;
    gtk_adjustment_set_value(adj, value + step);
    
    return G_SOURCE_CONTINUE;
}


/* Public function to stop autoscroll - called by tabs */
void
vite_tab_bar_stop_edge_scroll (ViteTabBar *self)
{
    if (self->drag_autoscroll_id) {
        g_source_remove(self->drag_autoscroll_id);
        self->drag_autoscroll_id = 0;
    }
    self->drag_scroll_direction = 0;
}

/* Public function to start autoscroll - called by tabs */
void
vite_tab_bar_start_edge_scroll (ViteTabBar *self, int direction)
{
    if (self->drag_scroll_direction == direction && self->drag_autoscroll_id != 0) return;
    
    vite_tab_bar_stop_edge_scroll(self);
    self->drag_scroll_direction = direction;
    self->drag_autoscroll_id = g_timeout_add(50, drag_autoscroll_tick, self);
}

gboolean
vite_tab_bar_is_overflowing (ViteTabBar *self)
{
    return self->is_overflowing;
}

/* Drop handler for flowbox (catches drops on empty space) */
static GdkDragAction
on_flowbox_drop_enter (GtkDropTarget *target, double x, double y, ViteTabBar *self)
{
    return GDK_ACTION_MOVE;
}

static gboolean
on_flowbox_drop (GtkDropTarget *target, const GValue *value, double x, double y, ViteTabBar *self)
{
    /* Stop edge scrolling */
    vite_tab_bar_stop_edge_scroll(self);
    
    /* Just acknowledge the drop - tab was already reordered during motion */
    if (!value || !G_VALUE_HOLDS(value, VITE_TYPE_TAB)) {
        return FALSE;
    }
    
    /* Check if tab is foreign */
    ViteTab *dropped_tab = VITE_TAB(g_value_get_object(value));
    if (!g_list_find(self->tabs, dropped_tab)) {
        /* -1 means append (end) */
        vite_tab_bar_drop_foreign_tab(self, dropped_tab, -1);
    } else {
        vite_tab_bar_notify_drop_done(self);
    }
    
    return TRUE;
}

static GdkDragAction
on_tab_bar_drop_motion (GtkDropTarget *target, double x, double y, ViteTabBar *self)
{
    /* Handle edge scrolling */
    if (vite_tab_bar_is_overflowing(self)) {
        int bar_width = gtk_widget_get_width(GTK_WIDGET(self));
        int edge_zone = 40;
        
        if (x < edge_zone) {
            vite_tab_bar_start_edge_scroll(self, -1);
        } else if (x > bar_width - edge_zone) {
            vite_tab_bar_start_edge_scroll(self, 1);
        } else {
            vite_tab_bar_stop_edge_scroll(self);
        }
    }
    return GDK_ACTION_MOVE;
}

static void
on_tab_bar_drop_leave (GtkDropTarget *target, ViteTabBar *self)
{
    vite_tab_bar_stop_edge_scroll(self);
}

/* Animation data for smooth interpolation */
typedef struct {
    ViteTab *tab;
    double start_offset;
    double target_offset;  /* Always 0 */
    gint64 start_time;
    gint64 duration_us;
    guint tick_id;
} TabAnimData;

static gboolean
tab_anim_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    TabAnimData *anim = user_data;
    
    /* Check if widget is still valid and not destroyed */
    if (!GTK_IS_WIDGET(anim->tab) || gtk_widget_in_destruction(GTK_WIDGET(anim->tab))) {
        g_free(anim);
        return G_SOURCE_REMOVE;
    }
    
    gint64 now = g_get_monotonic_time();
    double progress = (double)(now - anim->start_time) / anim->duration_us;
    
    if (progress >= 1.0) {
        /* Animation complete - set final offset and cleanup */
        vite_tab_set_anim_offset_x(anim->tab, anim->target_offset);
        g_free(anim);
        return G_SOURCE_REMOVE;
    }
    
    /* Ease-out cubic: 1 - (1 - t)^3 */
    double eased = 1.0 - pow(1.0 - progress, 3);
    double current = anim->start_offset + (anim->target_offset - anim->start_offset) * eased;
    
    vite_tab_set_anim_offset_x(anim->tab, current);
    
    return G_SOURCE_CONTINUE;
}

/* Reorder tab to a new position - called by individual tabs during drag */
void
vite_tab_bar_reorder_tab_to (ViteTabBar *self, ViteTab *tab, int new_position)
{
    int old_pos = g_list_index(self->tabs, tab);
    if (old_pos == -1) return;
    
    int num_tabs = g_list_length(self->tabs);
    if (new_position < 0) new_position = 0;
    if (new_position >= num_tabs) new_position = num_tabs - 1;
    
    /* No change needed */
    if (old_pos == new_position) return;
    
    /* Determine tab width for animation offset calculation */
    int tab_width = gtk_widget_get_width(GTK_WIDGET(tab));
    if (tab_width < 50) tab_width = 150; /* Fallback */
    
    /* Calculate direction and collect affected tabs */
    gboolean moving_right = (new_position > old_pos);
    int start_idx = MIN(old_pos, new_position);
    int end_idx = MAX(old_pos, new_position);
    
    /* Collect tabs that will be displaced */
    GArray *tabs_to_animate = g_array_new(FALSE, FALSE, sizeof(ViteTab*));
    int idx = 0;
    for (GList *l = self->tabs; l != NULL; l = l->next, idx++) {
        ViteTab *t = VITE_TAB(l->data);
        if (t == tab) continue;
        if (idx >= start_idx && idx <= end_idx) {
            g_array_append_val(tabs_to_animate, t);
        }
    }
    
    /* Update internal list */
    self->tabs = g_list_remove(self->tabs, tab);
    self->tabs = g_list_insert(self->tabs, tab, new_position);
    
    /* Hold reference while reparenting */
    g_object_ref(tab);
    
    /* Check if this is the currently dragging tab */
    gboolean is_dragging = (tab == self->dragging_tab);
    
    /* Get the FlowBoxChild wrapper and unparent tab from it */
    GtkWidget *child_wrapper = gtk_widget_get_parent(GTK_WIDGET(tab));
    if (child_wrapper && GTK_IS_FLOW_BOX_CHILD(child_wrapper)) {
        gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child_wrapper), NULL);
        gtk_flow_box_remove(GTK_FLOW_BOX(self->flowbox), child_wrapper);
    }
    
    /* Re-insert at new position */
    gtk_flow_box_insert(GTK_FLOW_BOX(self->flowbox), GTK_WIDGET(tab), new_position);
    
    /* If this is the dragging tab, apply the dragging CSS class to keep it as a placeholder */
    if (is_dragging) {
        gtk_widget_add_css_class(GTK_WIDGET(tab), "dragging");
    }
    
    g_object_unref(tab);
    
    /* Apply visual offset and animate to 0 for smooth sliding effect */
    for (guint i = 0; i < tabs_to_animate->len; i++) {
        ViteTab *t = g_array_index(tabs_to_animate, ViteTab*, i);
        if (!GTK_IS_WIDGET(t)) continue;
        
        /* Calculate initial offset: 
           If moving right (dragging e.g. 0->1), tab 1 shifts left to 0.
           Visually it jumps from X+width to X.
           We want it to appear at X+width and slide to X.
           So offset should be +width (Right) relative to new position.
           
           If moving left (dragging e.g. 1->0), tab 0 shifts right to 1.
           Visually it jumps from X to X+width.
           We want it to appear at X and slide to X+width.
           So offset should be -width (Left) relative to new position. 
        */
        double offset = moving_right ? tab_width : -tab_width;
        
        /* Set initial visual offset directly */
        vite_tab_set_anim_offset_x(t, offset);
        
        /* Start animation to return offset to 0 */
        TabAnimData *anim = g_new(TabAnimData, 1);
        anim->tab = t;
        anim->start_offset = offset;
        anim->target_offset = 0;
        anim->start_time = g_get_monotonic_time();
        anim->duration_us = 250000; /* 250ms */
        anim->tick_id = gtk_widget_add_tick_callback(GTK_WIDGET(t), tab_anim_tick, anim, NULL);
    }
    
    g_array_free(tabs_to_animate, TRUE);
    vite_tab_bar_update_separators(self);
}

static gboolean
on_scroll_controller_scroll (GtkEventControllerScroll *controller,
                             double dx, double dy,
                             gpointer user_data)
{
    ViteTabBar *self = VITE_TAB_BAR(user_data);
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroller));
    
    double value = gtk_adjustment_get_value(adj);
    /* Multiply by a factor for comfortable scroll speed */
    double new_value = value + (dy * 50.0); 

    gtk_adjustment_set_value(adj, new_value);

    return TRUE; /* Stop propagation */
}



static void
on_scroll_start_clicked (GtkButton *btn, ViteTabBar *self)
{
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroller));
    double value = gtk_adjustment_get_value(adj);
    gtk_adjustment_set_value(adj, value - 150.0); /* Scroll by about one tab width */
}

static void
on_scroll_end_clicked (GtkButton *btn, ViteTabBar *self)
{
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroller));
    double value = gtk_adjustment_get_value(adj);
    gtk_adjustment_set_value(adj, value + 150.0);
}

static void
vite_tab_bar_init (ViteTabBar *self)
{
    gtk_widget_add_css_class(GTK_WIDGET(self), "vite-tab-bar-container");
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_HORIZONTAL);
    
    /* Start Button */
    self->start_button = gtk_button_new_from_icon_name("go-previous-symbolic");
    gtk_widget_add_css_class(self->start_button, "tab-bar-nav-button");
    gtk_widget_set_size_request(self->start_button, 26, 32);
    gtk_widget_set_tooltip_text(self->start_button, "Scroll Left");
    gtk_widget_set_visible(self->start_button, FALSE);
    g_signal_connect(self->start_button, "clicked", G_CALLBACK(on_scroll_start_clicked), self);
    gtk_box_append(GTK_BOX(self), self->start_button);
    
    self->scroller = gtk_scrolled_window_new();
    /* EXTERNAL policy hides the scrollbar but keeps the adjustment active for wheel scrolling */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scroller), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    
    /* Connect adjustment monitoring */
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroller));
    g_signal_connect_swapped(adj, "changed", G_CALLBACK(check_overflow), self);
    g_signal_connect_swapped(adj, "notify::upper", G_CALLBACK(check_overflow), self);
    g_signal_connect_swapped(adj, "notify::page-size", G_CALLBACK(check_overflow), self);
    g_signal_connect_swapped(adj, "value-changed", G_CALLBACK(update_buttons), self);
    
    gtk_widget_set_hexpand(self->scroller, TRUE);
    gtk_box_append(GTK_BOX(self), self->scroller);
    
    /* End Button */
    self->end_button = gtk_button_new_from_icon_name("go-next-symbolic");
    gtk_widget_add_css_class(self->end_button, "tab-bar-nav-button");
    gtk_widget_set_size_request(self->end_button, 26, 32);
    gtk_widget_set_tooltip_text(self->end_button, "Scroll Right");
    gtk_widget_set_visible(self->end_button, FALSE);
    g_signal_connect(self->end_button, "clicked", G_CALLBACK(on_scroll_end_clicked), self);
    gtk_box_append(GTK_BOX(self), self->end_button);
    
    self->flowbox = gtk_flow_box_new();
    gtk_widget_add_css_class(self->flowbox, "vite-tab-bar");
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self->flowbox), GTK_ORIENTATION_HORIZONTAL);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(self->flowbox), TRUE);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->flowbox), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->flowbox), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->flowbox), 100);
    gtk_widget_set_hexpand(self->flowbox, TRUE);
    
    /* Add drop target to flowbox for drops on empty space */
    GtkDropTarget *flowbox_drop = gtk_drop_target_new(VITE_TYPE_TAB, GDK_ACTION_MOVE);
    g_signal_connect(flowbox_drop, "enter", G_CALLBACK(on_flowbox_drop_enter), self);
    g_signal_connect(flowbox_drop, "drop", G_CALLBACK(on_flowbox_drop), self);
    gtk_widget_add_controller(self->flowbox, GTK_EVENT_CONTROLLER(flowbox_drop));
    gtk_widget_set_halign(self->flowbox, GTK_ALIGN_FILL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroller), self->flowbox);
    
    /* Add drop target to the main TabBar container to handle drags over buttons */
    GtkDropTarget *bar_drop = gtk_drop_target_new(VITE_TYPE_TAB, GDK_ACTION_MOVE);
    g_signal_connect(bar_drop, "motion", G_CALLBACK(on_tab_bar_drop_motion), self);
    g_signal_connect(bar_drop, "leave", G_CALLBACK(on_tab_bar_drop_leave), self);
    g_signal_connect(bar_drop, "drop", G_CALLBACK(on_flowbox_drop), self); /* Reuse same drop logic */
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(bar_drop));
    
    /* Drop target is now on individual tabs, not the bar */
    
    /* Scroll Controller for Map Vertical Scroll -> Horizontal */
    GtkEventController *scroll_controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll_controller, "scroll", G_CALLBACK(on_scroll_controller_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self), scroll_controller);
    
    
    /* Monitor Overflow - already connected above */
    /* Trigger initial check */
    // queue a check? The signals above handle it.
} 




static void
vite_tab_bar_class_init (ViteTabBarClass *class)
{
    GObjectClass *object_class = G_OBJECT_CLASS(class);
    object_class->finalize = vite_tab_bar_finalize;
    
    signals[SIGNAL_OVERFLOW_CHANGED] = g_signal_new("overflow-changed",
        G_TYPE_FROM_CLASS(class),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

    signals[SIGNAL_TAB_DROPPED] = g_signal_new("tab-dropped",
        G_TYPE_FROM_CLASS(class),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, VITE_TYPE_TAB, G_TYPE_INT);
    
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, TAB_BAR_CSS);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

GtkWidget *
vite_tab_bar_new (void)
{
    return g_object_new(VITE_TYPE_TAB_BAR, NULL);
}

void
vite_tab_bar_add_tab (ViteTabBar *self, ViteTab *tab)
{
    vite_tab_set_tab_bar(tab, self);
    gtk_flow_box_insert(GTK_FLOW_BOX(self->flowbox), GTK_WIDGET(tab), -1);
    self->tabs = g_list_append(self->tabs, tab);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->flowbox), 2);
    gtk_widget_set_visible(GTK_WIDGET(self), g_list_length(self->tabs) > 1);
    
    update_tab_sizes(self);
    vite_tab_bar_update_separators(self);
}



void
vite_tab_bar_insert_tab (ViteTabBar *self, ViteTab *tab, int position)
{
    vite_tab_set_tab_bar(tab, self);
    gtk_flow_box_insert(GTK_FLOW_BOX(self->flowbox), GTK_WIDGET(tab), position);
    self->tabs = g_list_insert(self->tabs, tab, position);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->flowbox), 2);
    gtk_widget_set_visible(GTK_WIDGET(self), g_list_length(self->tabs) > 1);
    
    update_tab_sizes(self);
    vite_tab_bar_update_separators(self);
}

void
vite_tab_bar_remove_tab (ViteTabBar *self, ViteTab *tab)
{
    if (!G_IS_OBJECT(tab)) return;
    GList *l = g_list_find(self->tabs, tab);
    if (!l) return;
    
    gboolean was_active = vite_tab_is_active(tab);
    GList *sibling = NULL;
    
    if (was_active) {
        if (l->next) sibling = l->next;
        else if (l->prev) sibling = l->prev;
    }
    
    self->tabs = g_list_delete_link(self->tabs, l);
    
    if (self->dragging_tab == tab) {
        self->dragging_tab = NULL;
        self->drop_occurred = FALSE;
        self->drag_original_pos = -1;
    }
    
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(tab));
    if (parent && GTK_IS_FLOW_BOX_CHILD(parent)) {
        /* Explicitly detach to ensure tab->parent is NULL immediately */
        gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(parent), NULL);
        gtk_flow_box_remove(GTK_FLOW_BOX(self->flowbox), parent);
    } else {
        /* Fallback if somehow not wrapped (unlikely) */
        gtk_flow_box_remove(GTK_FLOW_BOX(self->flowbox), GTK_WIDGET(tab));
    }
    
    if (was_active && sibling) {
        ViteTab *next_tab = VITE_TAB(sibling->data);
        g_signal_emit_by_name(next_tab, "clicked");
    }
    
    vite_tab_bar_update_separators(self);
    update_tab_sizes(self);
    gtk_widget_set_visible(GTK_WIDGET(self), g_list_length(self->tabs) > 1);
}

/* ... */



typedef struct {
    ViteTab *tab;
    int attempts;
} ScrollRetryData;

static gboolean
scroll_retry_timeout (gpointer user_data)
{
    ScrollRetryData *data = user_data;
    ViteTab *tab = data->tab;
    
    if (!GTK_IS_WIDGET(tab) || !gtk_widget_get_parent(GTK_WIDGET(tab))) {
        g_object_unref(tab);
        g_free(data);
        return G_SOURCE_REMOVE;
    }

    ViteTabBar *self = VITE_TAB_BAR(g_object_get_data(G_OBJECT(tab), "tab-bar"));
    if (!self || !self->scroller) {
        g_object_unref(tab);
        g_free(data);
        return G_SOURCE_REMOVE;
    }

    graphene_rect_t bounds;
    /* Try to compute bounds */
    if (gtk_widget_compute_bounds(GTK_WIDGET(tab), self->flowbox, &bounds)) {
        GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroller));
        double page_size = gtk_adjustment_get_page_size(adj);
        double value = gtk_adjustment_get_value(adj);
        double upper = gtk_adjustment_get_upper(adj);
        
        /* Check if the scroller knows about the new size yet. 
           If bounds are way outside upper, maybe we need to wait more? 
           But usually upper matches FlowBox allocation. */
           
        double padding = 20.0;
        double target = -1.0;
        
        if (bounds.origin.x < value) {
            target = bounds.origin.x - padding;
        } else if (bounds.origin.x + bounds.size.width > value + page_size) {
            target = bounds.origin.x + bounds.size.width - page_size + padding;
        }
        
        if (target != -1.0) {
            /* Ensure target is valid */
            if (target < 0) target = 0;
            if (target > upper - page_size) target = upper - page_size;
            
            gtk_adjustment_set_value(adj, target);
        }
        
        /* Done */
        g_object_unref(tab);
        g_free(data);
        return G_SOURCE_REMOVE;
    }
    
    data->attempts++;
    if (data->attempts > 20) { /* 400ms max */
        g_object_unref(tab);
        g_free(data);
        return G_SOURCE_REMOVE;
    }
    
    return G_SOURCE_CONTINUE;
}

void
vite_tab_bar_set_active_tab (ViteTabBar *self, ViteTab *tab)
{
    GList *l;
    for (l = self->tabs; l != NULL; l = l->next) {
        ViteTab *t = VITE_TAB(l->data);
        vite_tab_set_active(t, t == tab);
    }
    
    if (tab) {
        /* Start retry loop immediately to handle layout delays */
        ScrollRetryData *data = g_new(ScrollRetryData, 1);
        data->tab = tab;
        g_object_ref(tab);
        data->attempts = 0;
        g_timeout_add(20, scroll_retry_timeout, data);
    }
    
    vite_tab_bar_update_separators(self);
}

int
vite_tab_bar_get_n_tabs (ViteTabBar *self)
{
    return g_list_length(self->tabs);
}

ViteTab *
vite_tab_bar_get_active_tab (ViteTabBar *self)
{
    GList *l;
    for (l = self->tabs; l != NULL; l = l->next) {
        ViteTab *t = VITE_TAB(l->data);
        if (vite_tab_is_active(t)) {
            return t;
        }
    }
    return NULL;
}

GList *
vite_tab_bar_get_tabs (ViteTabBar *self)
{
    return g_list_copy(self->tabs);
}

void
vite_tab_bar_set_dragging_tab (ViteTabBar *self, ViteTab *tab)
{
    self->dragging_tab = tab;
    self->drag_original_pos = g_list_index(self->tabs, tab);
    self->drop_occurred = FALSE;
}

void
vite_tab_bar_notify_drop_done (ViteTabBar *self)
{
    self->drop_occurred = TRUE;
}

void
vite_tab_bar_clear_dragging_tab (ViteTabBar *self, gboolean success)
{
    /* If drop successful, assume handled (e.g. moved to another window). 
       Only revert if NOT success logic and NOT drop logic. */
    /* If drop didn't occur (drag cancelled/outside), revert to original position */
    if (!success && self->dragging_tab && !self->drop_occurred && self->drag_original_pos != -1) {
        /* Verify tab is still in list before reverting */
        if (g_list_find(self->tabs, self->dragging_tab)) {
            vite_tab_bar_reorder_tab_to(self, self->dragging_tab, self->drag_original_pos);
        }
    }

    self->dragging_tab = NULL;
    self->drop_occurred = FALSE;
    self->drag_original_pos = -1;
}

void
vite_tab_bar_drop_foreign_tab(ViteTabBar *self, ViteTab *tab, int position)
{
    g_signal_emit(self, signals[SIGNAL_TAB_DROPPED], 0, tab, position);
}
