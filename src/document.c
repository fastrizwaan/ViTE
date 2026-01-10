#include "document.h"

struct _Document {
    PieceTable *pt;
    UndoStack *undo_stack;
    char *file_path;
    
    /* Modification State */
    void *saved_command; /* Pointer to the undo command representing the saved state */
    void (*mod_callback)(Document *doc, gboolean modified, void *user_data);
    void *mod_user_data;

    /* Content Observation */
    void (*content_callback)(Document *doc, void *user_data);
    void *content_user_data;
};

static void
check_modification_state(Document *doc)
{
    void *current = undo_stack_peek(doc->undo_stack);
    gboolean modified = (current != doc->saved_command);
    
    if (doc->mod_callback) {
        doc->mod_callback(doc, modified, doc->mod_user_data);
    }
}

Document *
document_new(const char *filename)
{
    Document *doc = malloc(sizeof(Document));
    doc->pt = piece_table_new(filename);
    doc->undo_stack = undo_stack_new();
    doc->file_path = filename ? g_strdup(filename) : NULL;
    doc->saved_command = NULL;
    doc->mod_callback = NULL;
    doc->mod_user_data = NULL;
    doc->content_callback = NULL;
    doc->content_user_data = NULL;
    return doc;
}

void
document_free(Document *doc)
{
    piece_table_free(doc->pt);
    undo_stack_free(doc->undo_stack);
    g_free(doc->file_path);
    free(doc);
}

const char *
document_get_file_path(Document *doc)
{
    return doc->file_path;
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
    check_modification_state(doc);
    if (doc->content_callback) doc->content_callback(doc, doc->content_user_data);
}

void
document_delete(Document *doc, size_t offset, size_t len)
{
    if (len == 0) return;
    char *deleted = piece_table_get_text_range(doc->pt, offset, len);
    undo_stack_push_delete(doc->undo_stack, offset, deleted, len);
    g_free(deleted);
    
    piece_table_delete(doc->pt, offset, len);
    check_modification_state(doc);
    if (doc->content_callback) doc->content_callback(doc, doc->content_user_data);
}

/* Proxy UndoInfo type manually to avoid cyclic dep header hell if needed, 
   but we can include undo.h in document.h or forward declare. 
   For now, strictly include undo.h in document.h */

UndoInfo
document_undo(Document *doc)
{
    UndoInfo info = undo_stack_undo(doc->undo_stack, doc->pt);
    check_modification_state(doc);
    if (doc->content_callback) doc->content_callback(doc, doc->content_user_data);
    return info;
}

UndoInfo
document_redo(Document *doc)
{
    UndoInfo info = undo_stack_redo(doc->undo_stack, doc->pt);
    check_modification_state(doc);
    if (doc->content_callback) doc->content_callback(doc, doc->content_user_data);
    return info;
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
    check_modification_state(doc);
}

void
document_set_undo_group_selection(Document *doc, size_t start, size_t end)
{
    undo_stack_set_group_selection(doc->undo_stack, start, end);
}

void
document_set_redo_group_selection(Document *doc, size_t start, size_t end)
{
    undo_stack_set_group_selection_after(doc->undo_stack, start, end);
}

gboolean
document_is_modified(Document *doc)
{
    void *current = undo_stack_peek(doc->undo_stack);
    return (current != doc->saved_command);
}

void
document_mark_saved(Document *doc)
{
    doc->saved_command = undo_stack_peek(doc->undo_stack);
    check_modification_state(doc);
}

void
document_set_modification_callback(Document *doc, void (*func)(Document *doc, gboolean modified, void *user_data), void *user_data)
{
    doc->mod_callback = func;
    doc->mod_user_data = user_data;
}

void
document_set_content_callback(Document *doc, DocumentContentCallback callback, void *user_data)
{
    doc->content_callback = callback;
    doc->content_user_data = user_data;
}
