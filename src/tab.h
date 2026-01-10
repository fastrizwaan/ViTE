#pragma once

#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

#define VITE_TYPE_TAB (vite_tab_get_type())
G_DECLARE_FINAL_TYPE(ViteTab, vite_tab, VITE, TAB, GtkBox)

GtkWidget *vite_tab_new(const char *title);
const char *vite_tab_get_title(ViteTab *self);
void vite_tab_set_title(ViteTab *self, const char *title);
void vite_tab_set_active (ViteTab *self, gboolean active);
void vite_tab_set_tab_bar (ViteTab *self, gpointer tab_bar);
void vite_tab_set_separator_visible (ViteTab *self, gboolean visible);
gboolean vite_tab_is_active (ViteTab *self);
void vite_tab_set_anim_offset_x (ViteTab *self, double offset);
double vite_tab_get_anim_offset_x (ViteTab *self);
gboolean vite_tab_is_hovered (ViteTab *self);
void vite_tab_set_modified(ViteTab *self, gboolean modified);

G_END_DECLS
