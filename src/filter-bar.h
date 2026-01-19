#ifndef FILTER_BAR_H
#define FILTER_BAR_H

#include <gtk/gtk.h>
#include "editor-widget.h"

#define VITE_TYPE_FILTER_BAR (vite_filter_bar_get_type())
G_DECLARE_FINAL_TYPE(ViteFilterBar, vite_filter_bar, VITE, FILTER_BAR, GtkBox)

GtkWidget *vite_filter_bar_new(EditorWidget *editor);
void vite_filter_bar_show(ViteFilterBar *bar);
void vite_filter_bar_close(ViteFilterBar *bar);
void vite_filter_bar_apply_filter(ViteFilterBar *bar);
void vite_filter_bar_clear_filter(ViteFilterBar *bar);
void vite_filter_bar_set_text(ViteFilterBar *bar, const char *text);

#endif