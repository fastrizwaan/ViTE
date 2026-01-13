#ifndef EDITOR_WIDGET_H
#define EDITOR_WIDGET_H

#include <gtk/gtk.h>
#include "document.h"

#define EDITOR_TYPE_WIDGET (editor_widget_get_type())
G_DECLARE_FINAL_TYPE(EditorWidget, editor_widget, EDITOR, WIDGET, GtkWidget)

GtkWidget *editor_widget_new(void);
void editor_widget_set_document(EditorWidget *self, Document *doc);
void editor_widget_set_language(EditorWidget *self, const char *lang);

Document *editor_widget_get_document(EditorWidget *self);

/* Search Integration */
void editor_widget_set_search_results(EditorWidget *self, GArray *matches);
void editor_widget_next_match(EditorWidget *self);
void editor_widget_prev_match(EditorWidget *self);
void editor_widget_replace_current(EditorWidget *self, const char *replacement);
int editor_widget_get_current_match_index(EditorWidget *self);
void editor_widget_get_visible_line_range(EditorWidget *self, size_t *start, size_t *end);
GtkAdjustment *editor_widget_get_vadjustment(EditorWidget *self);
void editor_widget_refresh_syntax(EditorWidget *self);
void editor_widget_reset_cursor_to_start(EditorWidget *self);
char *editor_widget_get_selected_text(EditorWidget *self);

#endif
