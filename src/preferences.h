#ifndef PREFERENCES_H
#define PREFERENCES_H

#include <gtk/gtk.h>
#include "editor-widget.h"

void show_preferences_dialog(GtkWindow *parent, EditorWidget *editor);
void update_save_button_visibility_from_preferences(GtkWindow *window, gboolean visible);

#endif
