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
const char *editor_widget_get_language_name(EditorWidget *self);

/* Search Integration */
void editor_widget_set_search_results(EditorWidget *self, GArray *matches);
void editor_widget_next_match(EditorWidget *self);
void editor_widget_prev_match(EditorWidget *self);
void editor_widget_replace_current(EditorWidget *self, const char *replacement, gboolean regex, const char *regex_text);
int editor_widget_get_current_match_index(EditorWidget *self);
void editor_widget_get_visible_line_range(EditorWidget *self, size_t *start, size_t *end);
void editor_widget_get_visible_offset_range(EditorWidget *self, size_t *start_offset, size_t *end_offset);
GtkAdjustment *editor_widget_get_vadjustment(EditorWidget *self);
void editor_widget_refresh_syntax(EditorWidget *self);
void editor_widget_reset_cursor_to_start(EditorWidget *self);
char *editor_widget_get_selected_text(EditorWidget *self);
void editor_widget_scroll_to_line(EditorWidget *self, size_t line);
void editor_widget_get_cursor_position(EditorWidget *self, size_t *line, size_t *col);
gboolean editor_widget_get_insert_mode(EditorWidget *self);
void editor_widget_set_language(EditorWidget *self, const char *lang);
void editor_widget_set_line_ending(EditorWidget *self, const char *line_ending_id);
void editor_widget_set_encoding(EditorWidget *self, const char *encoding_id);
void editor_widget_set_insert_mode(EditorWidget *self, gboolean insert);

void editor_widget_set_show_line_numbers(EditorWidget *self, gboolean show);
gboolean editor_widget_get_show_line_numbers(EditorWidget *self);

void editor_widget_set_word_wrap(EditorWidget *self, gboolean wrap);
gboolean editor_widget_get_word_wrap(EditorWidget *self);

#endif
