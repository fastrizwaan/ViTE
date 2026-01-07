#include "tab-bar.h"

struct _ViteTabBar {
    GtkBox parent_instance;
    GtkWidget *scroller;
    GtkWidget *flowbox;
    
    GList *tabs;
    
    GtkDropTarget *drop_target;
    int drop_indicator_position;
    
    int last_allocated_width;
    int cached_cols;
    gboolean is_overflowing;
};

G_DEFINE_TYPE(ViteTabBar, vite_tab_bar, GTK_TYPE_BOX)

enum {
    SIGNAL_OVERFLOW_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

static const char *TAB_BAR_CSS = 
".chrome-tab-bar-container:drop(active), .chrome-tab-bar:drop(active) {"
"    border: none;"
"    box-shadow: none;"
"    background: none;"
"    outline: none;"
"}"
".chrome-tab-bar-container {"
"    margin-top: 1px;"
"    padding: 0;"
"    margin-bottom: 0px;"
"}"
".chrome-tab-bar {"
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
"}";




static void
vite_tab_bar_finalize (GObject *object)
{
    ViteTabBar *self = VITE_TAB_BAR(object);
    g_list_free(self->tabs);
    G_OBJECT_CLASS(vite_tab_bar_parent_class)->finalize(object);
}

static void
update_separators (ViteTabBar *self)
{
    if (!self->tabs) return;
    
    int cols = self->cached_cols;
    if (cols < 1) cols = 1;
    
    int active_index = -1;
    int idx = 0;
    for (GList *l = self->tabs; l != NULL; l = l->next) {
        ViteTab *t = VITE_TAB(l->data);
        if (gtk_widget_has_css_class(GTK_WIDGET(t), "active")) {
            active_index = idx;
        }
        idx++;
    }
    
    idx = 0;
    for (GList *l = self->tabs; l != NULL; l = l->next) {
        ViteTab *t = VITE_TAB(l->data);
        if (!GTK_IS_WIDGET(t)) { idx++; continue; }
        
        gboolean visible = TRUE;
        
        if (idx % cols == 0) visible = FALSE;
        if (idx == active_index) visible = FALSE;
        if (idx == active_index + 1) visible = FALSE;
        
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
        gtk_widget_set_size_request(tab, 50, 32);
        gtk_widget_set_hexpand(tab, TRUE);
        gtk_widget_set_halign(tab, GTK_ALIGN_FILL);
        
        /* Ensure FlowBoxChild wrapper also expands */
        GtkWidget *parent = gtk_widget_get_parent(tab);
        if (parent && GTK_IS_FLOW_BOX_CHILD(parent)) {
             gtk_widget_set_hexpand(parent, TRUE);
             gtk_widget_set_halign(parent, GTK_ALIGN_FILL);
        }
    }
    
    update_separators(self);
}

static void
check_overflow (ViteTabBar *self)
{
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroller));
    double upper = gtk_adjustment_get_upper(adj);
    double page_size = gtk_adjustment_get_page_size(adj);
    
    gboolean overflowing = (upper > page_size + 0.1); /* Epsilon for float layout */
    
    if (overflowing != self->is_overflowing) {
        self->is_overflowing = overflowing;
        g_signal_emit(self, signals[SIGNAL_OVERFLOW_CHANGED], 0, overflowing);
    }
}





static int
calculate_drop_position (ViteTabBar *self, double x, double y)
{
    int best_index = -1;
    double min_dist = 1e9;
    int insert_after = 0;
       
    int i = 0;
    for (GList *l = self->tabs; l != NULL; l = l->next) {
        GtkWidget *tab = GTK_WIDGET(l->data);
        if (!gtk_widget_get_visible(tab)) { i++; continue; }
        
        graphene_rect_t bounds;
        if (gtk_widget_compute_bounds(tab, GTK_WIDGET(self), &bounds)) {
            /* Check Row Match */
            if (y >= bounds.origin.y && y <= bounds.origin.y + bounds.size.height) {
                 double cx = bounds.origin.x + bounds.size.width / 2.0;
                 double dist = fabs(x - cx);
                 if (dist < min_dist) {
                     min_dist = dist;
                     best_index = i;
                     insert_after = (x > cx) ? 1 : 0;
                 }
            }
        }
        i++;
    }
    
    if (best_index != -1) {
        return best_index + insert_after;
    }
    
    return g_list_length(self->tabs);
}


static void
clear_drop_targets (ViteTabBar *self)
{
    for (GList *l = self->tabs; l != NULL; l = l->next) {
        ViteTab *t = VITE_TAB(l->data);
        if (!GTK_IS_WIDGET(t)) continue;
        vite_tab_set_drop_indicator(t, FALSE);
        gtk_widget_remove_css_class(GTK_WIDGET(t), "drop-target-end");
    }
    self->drop_indicator_position = -1;
}

static void
set_drop_target_at (ViteTabBar *self, int position)
{
    if (position == self->drop_indicator_position) return;
    
    clear_drop_targets(self);
    self->drop_indicator_position = position;
    
    int num_tabs = g_list_length(self->tabs);
    
    if (position >= num_tabs) {
        /* Dropping at the end - highlight right edge of last tab */
        ViteTab *last_tab = g_list_nth_data(self->tabs, num_tabs - 1);
        if (last_tab && GTK_IS_WIDGET(last_tab)) {
            gtk_widget_add_css_class(GTK_WIDGET(last_tab), "drop-target-end");
        }
    } else {
        /* Highlight separator of the tab at the drop position */
        ViteTab *target_tab = g_list_nth_data(self->tabs, position);
        if (target_tab) {
            vite_tab_set_drop_indicator(target_tab, TRUE);
        }
    }
}

static GdkDragAction
on_drag_motion (GtkDropTarget *target, double x, double y, ViteTabBar *self)
{
    const GValue *value = gtk_drop_target_get_value(target);
    if (!value || !G_VALUE_HOLDS(value, VITE_TYPE_TAB)) return 0;
    
    int pos = calculate_drop_position(self, x, y);
    set_drop_target_at(self, pos);
    return GDK_ACTION_MOVE;
}

static void on_drag_leave (GtkDropTarget *t, ViteTabBar *self)
{
    clear_drop_targets(self);
}

static gboolean
on_drag_drop (GtkDropTarget *target, const GValue *value, double x, double y, ViteTabBar *self)
{
    clear_drop_targets(self);
    if (!value || !G_VALUE_HOLDS(value, VITE_TYPE_TAB)) return FALSE;
    
    ViteTab *tab = VITE_TAB(g_value_get_object(value));
    if (!GTK_IS_WIDGET(tab)) return FALSE;
    
    int pos = calculate_drop_position(self, x, y);
    int old_pos = g_list_index(self->tabs, tab);
    
    /* Calculate the actual insertion position after removal */
    int actual_new_pos = pos;
    if (old_pos != -1 && old_pos < pos) {
        actual_new_pos = pos - 1;
    }
    
    g_print("Drag Drop: old=%d new=%d (actual=%d)\n", old_pos, pos, actual_new_pos);
    
    /* Only move if position actually changes */
    if (old_pos != -1 && old_pos != actual_new_pos) {
        /* Update internal list */
        self->tabs = g_list_remove(self->tabs, tab);
        self->tabs = g_list_insert(self->tabs, tab, actual_new_pos);
        
        /* Safely move tab in flowbox */
        g_object_ref(tab);
        
        GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(tab));
        if (parent && GTK_IS_FLOW_BOX_CHILD(parent)) {
            gtk_widget_unparent(GTK_WIDGET(tab));
            gtk_flow_box_remove(GTK_FLOW_BOX(self->flowbox), parent);
        }
        
        gtk_flow_box_insert(GTK_FLOW_BOX(self->flowbox), GTK_WIDGET(tab), actual_new_pos);
        g_object_unref(tab);
        
        update_separators(self);
    }
    
    return TRUE;
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
vite_tab_bar_init (ViteTabBar *self)
{
    gtk_widget_add_css_class(GTK_WIDGET(self), "chrome-tab-bar-container");
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    
    self->scroller = gtk_scrolled_window_new();
    /* EXTERNAL policy hides the scrollbar but keeps the adjustment active for wheel scrolling */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scroller), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    
    /* Connect adjustment monitoring */
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(self->scroller));
    g_signal_connect_swapped(adj, "changed", G_CALLBACK(check_overflow), self);
    g_signal_connect_swapped(adj, "notify::upper", G_CALLBACK(check_overflow), self);
    g_signal_connect_swapped(adj, "notify::page-size", G_CALLBACK(check_overflow), self);
    
    gtk_widget_set_hexpand(self->scroller, TRUE);
    gtk_box_append(GTK_BOX(self), self->scroller);
    
    self->flowbox = gtk_flow_box_new();
    gtk_widget_add_css_class(self->flowbox, "chrome-tab-bar");
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->flowbox), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->flowbox), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->flowbox), 1000);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->flowbox), 3);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->flowbox), 3);
    gtk_widget_set_hexpand(self->flowbox, TRUE);
    gtk_widget_set_halign(self->flowbox, GTK_ALIGN_FILL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroller), self->flowbox);
    
    self->drop_target = gtk_drop_target_new(VITE_TYPE_TAB, GDK_ACTION_MOVE);
    gtk_drop_target_set_preload(self->drop_target, TRUE);
    g_signal_connect(self->drop_target, "motion", G_CALLBACK(on_drag_motion), self);
    g_signal_connect(self->drop_target, "leave", G_CALLBACK(on_drag_leave), self);
    g_signal_connect(self->drop_target, "drop", G_CALLBACK(on_drag_drop), self);
    g_signal_connect(self->drop_target, "drop", G_CALLBACK(on_drag_drop), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(self->drop_target));
    
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
    
    gtk_widget_set_visible(GTK_WIDGET(self), g_list_length(self->tabs) > 1);
    
    update_tab_sizes(self);
    update_separators(self);
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
    gtk_flow_box_remove(GTK_FLOW_BOX(self->flowbox), GTK_WIDGET(tab));
    
    if (was_active && sibling) {
        ViteTab *next_tab = VITE_TAB(sibling->data);
        g_signal_emit_by_name(next_tab, "clicked");
    }
    
    update_separators(self);
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
