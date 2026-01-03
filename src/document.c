#include "document.h"

struct _Document {
    PieceTable *pt;
    UndoStack *undo_stack;
};

Document *
document_new(const char *filename)
{
    Document *doc = malloc(sizeof(Document));
    doc->pt = piece_table_new(filename);
    doc->undo_stack = undo_stack_new();
    return doc;
}

void
document_free(Document *doc)
{
    piece_table_free(doc->pt);
    undo_stack_free(doc->undo_stack);
    free(doc);
}

char *
document_get_line(Document *doc, size_t line_index, size_t *len)
{
    return piece_table_get_line(doc->pt, line_index, len);
}

size_t
document_get_line_length(Document *doc, size_t line_index)
{
    if (!doc) return 0;
    return piece_table_get_line_length(doc->pt, line_index);
}

void
document_foreach_line(Document *doc, void (*func)(size_t line_len, void *user_data), void *user_data)
{
    if (!doc) return;
    piece_table_foreach_line(doc->pt, func, user_data);
}

size_t
document_get_line_count(Document *doc)
{
    return piece_table_get_line_count(doc->pt);
}

size_t
document_get_length(Document *doc)
{
    return piece_table_get_length(doc->pt);
}

char *
document_get_text_range(Document *doc, size_t offset, size_t len)
{
    return piece_table_get_text_range(doc->pt, offset, len);
}

size_t
document_get_line_of_offset(Document *doc, size_t offset)
{
    return piece_table_get_line_of_offset(doc->pt, offset);
}

size_t
document_get_offset_of_line(Document *doc, size_t line_index)
{
    return piece_table_get_offset_of_line(doc->pt, line_index);
}

void
document_insert(Document *doc, size_t offset, const char *text, size_t len)
{
    if (len == 0) return;
    undo_stack_push_insert(doc->undo_stack, offset, text, len);
    piece_table_insert(doc->pt, offset, text, len);
}

void
document_delete(Document *doc, size_t offset, size_t len)
{
    if (len == 0) return;
    char *deleted = piece_table_get_text_range(doc->pt, offset, len);
    undo_stack_push_delete(doc->undo_stack, offset, deleted, len);
    g_free(deleted);
    
    piece_table_delete(doc->pt, offset, len);
}

/* Proxy UndoInfo type manually to avoid cyclic dep header hell if needed, 
   but we can include undo.h in document.h or forward declare. 
   For now, strictly include undo.h in document.h */

UndoInfo
document_undo(Document *doc)
{
    return undo_stack_undo(doc->undo_stack, doc->pt);
}

UndoInfo
document_redo(Document *doc)
{
    return undo_stack_redo(doc->undo_stack, doc->pt);
}

void
document_begin_undo_group(Document *doc)
{
    undo_stack_begin_group(doc->undo_stack);
}

void
document_end_undo_group(Document *doc)
{
    undo_stack_end_group(doc->undo_stack);
}
