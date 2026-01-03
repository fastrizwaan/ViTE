#ifndef DOCUMENT_H
#define DOCUMENT_H

#include <gtk/gtk.h>
#include "piece-table.h"
#include "undo.h"

typedef struct _Document Document;

Document *document_new(const char *filename);
void document_free(Document *doc);

/* Content Access */
char *document_get_line(Document *doc, size_t line_index, size_t *len);
size_t document_get_line_length(Document *doc, size_t line_index);
void document_foreach_line(Document *doc, void (*func)(size_t line_len, void *user_data), void *user_data);
size_t document_get_line_count(Document *doc);
size_t document_get_length(Document *doc);
char *document_get_text_range(Document *doc, size_t offset, size_t len);
size_t document_get_line_of_offset(Document *doc, size_t offset);
size_t document_get_offset_of_line(Document *doc, size_t line_index);

/* Editing */
void document_insert(Document *doc, size_t offset, const char *text, size_t len);
void document_delete(Document *doc, size_t offset, size_t len);

/* Undo/Redo */
UndoInfo document_undo(Document *doc);
UndoInfo document_redo(Document *doc);
void document_begin_undo_group(Document *doc);
void document_end_undo_group(Document *doc);

#endif
