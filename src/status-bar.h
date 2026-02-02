#ifndef VITE_STATUS_BAR_H
#define VITE_STATUS_BAR_H

#include <gtk/gtk.h>

#define VITE_TYPE_STATUS_BAR (vite_status_bar_get_type())
G_DECLARE_FINAL_TYPE(ViteStatusBar, vite_status_bar, VITE, STATUS_BAR, GtkWidget)

GtkWidget *vite_status_bar_new(void);

/* Update methods */
void vite_status_bar_set_cursor_position(ViteStatusBar *self, int line, int col);
void vite_status_bar_set_file_type(ViteStatusBar *self, const char *file_type);
void vite_status_bar_set_encoding(ViteStatusBar *self, const char *encoding_id);
void vite_status_bar_set_line_ending(ViteStatusBar *self, const char *line_ending_id);
void vite_status_bar_set_indentation(ViteStatusBar *self, int width, gboolean use_tabs);
void vite_status_bar_set_insert_mode(ViteStatusBar *self, gboolean insert);

#endif
