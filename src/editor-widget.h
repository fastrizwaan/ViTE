#ifndef EDITOR_WIDGET_H
#define EDITOR_WIDGET_H

#include <gtk/gtk.h>
#include "document.h"

#define EDITOR_TYPE_WIDGET (editor_widget_get_type())
G_DECLARE_FINAL_TYPE(EditorWidget, editor_widget, EDITOR, WIDGET, GtkWidget)

GtkWidget *editor_widget_new(void);
void editor_widget_set_document(EditorWidget *self, Document *doc);
void editor_widget_set_language(EditorWidget *self, const char *lang);

#endif
