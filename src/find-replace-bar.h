#ifndef FIND_REPLACE_BAR_H
#define FIND_REPLACE_BAR_H

#include <gtk/gtk.h>
#include "editor-widget.h"

#define VITE_TYPE_FIND_REPLACE_BAR (vite_find_replace_bar_get_type())
G_DECLARE_FINAL_TYPE(ViteFindReplaceBar, vite_find_replace_bar, VITE, FIND_REPLACE_BAR, GtkBox)

GtkWidget *vite_find_replace_bar_new(EditorWidget *editor);
void vite_find_replace_bar_toggle_replace(ViteFindReplaceBar *bar);
void vite_find_replace_bar_show(ViteFindReplaceBar *bar);
void vite_find_replace_bar_close(ViteFindReplaceBar *bar);

#endif
