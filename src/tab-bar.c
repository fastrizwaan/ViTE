#include "tab-bar.h"

struct _ViteTabBar {
    GtkBox parent_instance;
    GtkWidget *flowbox;
    
    GList *tabs;
    
    GtkDropTarget *drop_target;
    int drop_indicator_position;
    
    int last_allocated_width;
    int cached_cols;
};

G_DEFINE_TYPE(ViteTabBar, vite_tab_bar, GTK_TYPE_BOX)

static const char *TAB_BAR_CSS = 
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
"}"
".tab-drop-indicator {"
"    background: linear-gradient(to bottom, transparent 0%, rgba(0, 127, 255, 0.8) 20%, #3584e4 50%, rgba(0, 127, 255, 0.8) 80%, transparent 100%);"
"    min-width: 3px;"
"    min-height: 24px;"
"    border-radius: 2px;"
"    margin: 0;"
"}"
".end-drop-zone {"
"    min-width: 4px;"
"    min-height: 28px;"
"    background: linear-gradient(to bottom, transparent, #3584e4 20%, #3584e4 80%, transparent);"
"    border-radius: 2px;"
"    margin-left: 0px;"
"    margin-right: 0px;"
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
update_tab_sizes (ViteTabBar *self, int allocated_width)
{
    if (allocated_width <= 0) allocated_width = gtk_widget_get_width(GTK_WIDGET(self));
    if (allocated_width <= 0) return;
    
    if (!self->tabs) return;
    
    int margin_start = 6;
    int available_width = allocated_width - margin_start;
    
    int min_tab_width = 150;
    int max_tab_width = 240;
    
    int num_tabs = g_list_length(self->tabs);
    if (num_tabs == 0) return;
    
    int capacity = MAX(1, available_width / min_tab_width);
    int cols = MIN(num_tabs, capacity);
    if (cols < 1) cols = 1;
    
    self->cached_cols = cols;
    
    int final_tab_width = CLAMP(available_width / cols, min_tab_width, max_tab_width);
    
    for (GList *l = self->tabs; l != NULL; l = l->next) {
        GtkWidget *tab = GTK_WIDGET(l->data);
        if (!GTK_IS_WIDGET(tab)) continue;
        gtk_widget_set_size_request(tab, final_tab_width, 32);
    }
    
    update_separators(self);
}

static void
on_notify_width (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ViteTabBar *self = VITE_TAB_BAR(object);
    update_tab_sizes(self, gtk_widget_get_width(GTK_WIDGET(self)));
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
        vite_tab_set_separator_drop_target(t, FALSE);
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
            vite_tab_set_separator_drop_target(target_tab, TRUE);
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

static void
vite_tab_bar_init (ViteTabBar *self)
{
    gtk_widget_add_css_class(GTK_WIDGET(self), "chrome-tab-bar-container");
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    
    self->flowbox = gtk_flow_box_new();
    gtk_widget_add_css_class(self->flowbox, "chrome-tab-bar");
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->flowbox), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->flowbox), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->flowbox), 1000);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->flowbox), 3);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->flowbox), 3);
    gtk_widget_set_hexpand(self->flowbox, TRUE);
    gtk_box_append(GTK_BOX(self), self->flowbox);
    
    self->drop_target = gtk_drop_target_new(VITE_TYPE_TAB, GDK_ACTION_MOVE);
    gtk_drop_target_set_preload(self->drop_target, TRUE);
    g_signal_connect(self->drop_target, "motion", G_CALLBACK(on_drag_motion), self);
    g_signal_connect(self->drop_target, "leave", G_CALLBACK(on_drag_leave), self);
    g_signal_connect(self->drop_target, "drop", G_CALLBACK(on_drag_drop), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(self->drop_target));
    
    /* Legacy drop indicator - not used anymore */
    
    g_signal_connect(self, "notify::width", G_CALLBACK(on_notify_width), NULL);
    g_signal_connect(self, "map", G_CALLBACK(on_notify_width), NULL);
}

static void
vite_tab_bar_class_init (ViteTabBarClass *class)
{
    GObjectClass *object_class = G_OBJECT_CLASS(class);
    object_class->finalize = vite_tab_bar_finalize;
    
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
    update_tab_sizes(self, -1);
    
    update_separators(self);
    
    gtk_widget_set_visible(GTK_WIDGET(self), g_list_length(self->tabs) > 1);
}

void
vite_tab_bar_remove_tab (ViteTabBar *self, ViteTab *tab)
{
    if (!G_IS_OBJECT(tab)) return;
    GList *l = g_list_find(self->tabs, tab);
    if (!l) return;
    
    self->tabs = g_list_delete_link(self->tabs, l);
    
    gtk_flow_box_remove(GTK_FLOW_BOX(self->flowbox), GTK_WIDGET(tab));
    
    update_separators(self);
    
    gtk_widget_set_visible(GTK_WIDGET(self), g_list_length(self->tabs) > 1);
}

/* ... */



void
vite_tab_bar_set_active_tab (ViteTabBar *self, ViteTab *tab)
{
    GList *l;
    for (l = self->tabs; l != NULL; l = l->next) {
        ViteTab *t = VITE_TAB(l->data);
        vite_tab_set_active(t, t == tab);
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
    return NULL;
}
