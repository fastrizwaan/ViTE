#pragma once

#include <gtk/gtk.h>
#include <adwaita.h>
#include "tab.h"

G_BEGIN_DECLS

#define VITE_TYPE_TAB_BAR (vite_tab_bar_get_type())
G_DECLARE_FINAL_TYPE(ViteTabBar, vite_tab_bar, VITE, TAB_BAR, GtkBox)

GtkWidget *vite_tab_bar_new(void);
void vite_tab_bar_add_tab(ViteTabBar *self, ViteTab *tab);
void vite_tab_bar_remove_tab(ViteTabBar *self, ViteTab *tab);
int vite_tab_bar_get_n_tabs(ViteTabBar *self);
void vite_tab_bar_set_active_tab(ViteTabBar *self, ViteTab *tab);
ViteTab *vite_tab_bar_get_active_tab(ViteTabBar *self);
GList *vite_tab_bar_get_tabs(ViteTabBar *self);
void vite_tab_bar_update_separators(ViteTabBar *self);

/* Tab reordering during drag */
void vite_tab_bar_reorder_tab_to(ViteTabBar *self, ViteTab *tab, int new_position);
void vite_tab_bar_start_edge_scroll(ViteTabBar *self, int direction);
void vite_tab_bar_stop_edge_scroll(ViteTabBar *self);
gboolean vite_tab_bar_is_overflowing(ViteTabBar *self);

/* Drag state management */
void vite_tab_bar_set_dragging_tab(ViteTabBar *self, ViteTab *tab);
void vite_tab_bar_clear_dragging_tab(ViteTabBar *self);
void vite_tab_bar_notify_drop_done(ViteTabBar *self);

G_END_DECLS
