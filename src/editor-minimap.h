#ifndef EDITOR_MINIMAP_H
#define EDITOR_MINIMAP_H

#include "editor-widget.h"
#include <gtk/gtk.h>

void editor_minimap_draw(EditorWidget *self, GtkSnapshot *snapshot, double x, double y, double w, double h);

/* Helper to share logic between renderer and input */
void editor_minimap_get_params(EditorWidget *self, double viewport_h, 
                               double *out_map_content_h, 
                               double *out_map_scroll_y, 
                               double *out_map_line_h);

#endif
