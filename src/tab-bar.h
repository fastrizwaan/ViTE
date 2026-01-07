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

G_END_DECLS
