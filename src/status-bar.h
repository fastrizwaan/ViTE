#ifndef VITE_STATUS_BAR_H
#define VITE_STATUS_BAR_H

#include <gtk/gtk.h>

#define VITE_TYPE_STATUS_BAR (vite_status_bar_get_type())
G_DECLARE_FINAL_TYPE(ViteStatusBar, vite_status_bar, VITE, STATUS_BAR, GtkWidget)

GtkWidget *vite_status_bar_new(void);

/* Update methods */
void vite_status_bar_set_cursor_position(ViteStatusBar *self, int line, int col);
void vite_status_bar_set_file_type(ViteStatusBar *self, const char *file_type);
void vite_status_bar_set_encoding(ViteStatusBar *self, const char *encoding);
void vite_status_bar_set_line_ending(ViteStatusBar *self, const char *line_ending);
void vite_status_bar_set_tab_width(ViteStatusBar *self, int width);
void vite_status_bar_set_insert_mode(ViteStatusBar *self, gboolean insert);

#endif
