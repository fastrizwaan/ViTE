#include "tab-bar.h"

struct _ViteTabBar {
    GtkBox parent_instance;
    GtkWidget *flowbox;
    /* No internal new_tab_button (moved to header) */
    
    GList *tabs;
    
    GtkDropTarget *drop_target;
    GtkWidget *drop_indicator;
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
show_drop_indicator (ViteTabBar *self, int position)
{
    if (position == self->drop_indicator_position) return;
    self->drop_indicator_position = position;
    
    GtkWidget *parent = gtk_widget_get_parent(self->drop_indicator);
    if (parent) {
        gtk_flow_box_remove(GTK_FLOW_BOX(self->flowbox), self->drop_indicator);
    }
    
    gtk_widget_set_visible(self->drop_indicator, TRUE);
    
    /* Indices in FlowBox == Tabs indices because only tabs are children. */
    gtk_flow_box_insert(GTK_FLOW_BOX(self->flowbox), self->drop_indicator, position);
}

static void
hide_drop_indicator (ViteTabBar *self)
{
    gtk_widget_set_visible(self->drop_indicator, FALSE);
    GtkWidget *parent = gtk_widget_get_parent(self->drop_indicator);
    if (parent) {
        gtk_flow_box_remove(GTK_FLOW_BOX(self->flowbox), self->drop_indicator);
    }
    self->drop_indicator_position = -1;
}

static GdkDragAction
on_drag_motion (GtkDropTarget *target, double x, double y, ViteTabBar *self)
{
    const GValue *value = gtk_drop_target_get_value(target);
    if (!value || !G_VALUE_HOLDS(value, VITE_TYPE_TAB)) return 0;
    
    int pos = calculate_drop_position(self, x, y);
    show_drop_indicator(self, pos);
    return GDK_ACTION_MOVE;
}

static void on_drag_leave (GtkDropTarget *t, ViteTabBar *self) { hide_drop_indicator(self); }

static gboolean
on_drag_drop (GtkDropTarget *target, const GValue *value, double x, double y, ViteTabBar *self)
{
    hide_drop_indicator(self);
    if (!value || !G_VALUE_HOLDS(value, VITE_TYPE_TAB)) return FALSE;
    
    ViteTab *tab = VITE_TAB(g_value_get_object(value));
    if (!G_IS_OBJECT(tab)) return FALSE; /* Safety check */
    
    int pos = calculate_drop_position(self, x, y);
    
    int old_pos = g_list_index(self->tabs, tab);
    g_print("Drag Drop: old=%d new=%d\n", old_pos, pos);
    
    if (old_pos != -1 && old_pos != pos) {
        self->tabs = g_list_remove(self->tabs, tab);
        if (old_pos < pos) pos--; /* Adjustable because removal shifts later indices */
        
        self->tabs = g_list_insert(self->tabs, tab, pos);
        
        g_object_ref(tab);
        gtk_flow_box_remove(GTK_FLOW_BOX(self->flowbox), GTK_WIDGET(tab));
        gtk_flow_box_insert(GTK_FLOW_BOX(self->flowbox), GTK_WIDGET(tab), pos);
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
    g_signal_connect(self->drop_target, "motion", G_CALLBACK(on_drag_motion), self);
    g_signal_connect(self->drop_target, "leave", G_CALLBACK(on_drag_leave), self);
    g_signal_connect(self->drop_target, "drop", G_CALLBACK(on_drag_drop), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(self->drop_target));
    
    self->drop_indicator = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(self->drop_indicator, "tab-drop-indicator");
    gtk_widget_set_visible(self->drop_indicator, FALSE);
    
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
