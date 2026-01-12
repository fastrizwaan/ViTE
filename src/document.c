#include "document.h"

struct _Document {
    PieceTable *pt;
    UndoStack *undo_stack;
    char *file_path;
    
    /* Modification State */
    /* Modification State Listeners */
    void *saved_command; /* Pointer to the undo command representing the saved state */
    GList *mod_callbacks; /* List of struct { func, user_data } */

    /* Content Observation Listeners */
    GList *content_callbacks; /* List of struct { func, user_data } */
};

typedef struct {
    void (*func)(Document *doc, gboolean modified, void *user_data);
    void *user_data;
} ModCallbackData;

typedef struct {
    void (*func)(Document *doc, void *user_data);
    void *user_data;
} ContentCallbackData;

static void
check_modification_state(Document *doc)
{
    void *current = undo_stack_peek(doc->undo_stack);
    gboolean modified = (current != doc->saved_command);
    for (GList *l = doc->mod_callbacks; l != NULL; l = l->next) {
        ModCallbackData *cb = l->data;
        cb->func(doc, modified, cb->user_data);
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
    doc->mod_callbacks = NULL;
    doc->content_callbacks = NULL;
    return doc;
}

void
document_free(Document *doc)
{
    piece_table_free(doc->pt);
    undo_stack_free(doc->undo_stack);
    g_free(doc->file_path);
    /* Free callback lists */
    g_list_free_full(doc->mod_callbacks, g_free);
    g_list_free_full(doc->content_callbacks, g_free);
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
    check_modification_state(doc);
    for (GList *l = doc->content_callbacks; l != NULL; l = l->next) {
        ContentCallbackData *cb = l->data;
        cb->func(doc, cb->user_data);
    }
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
    check_modification_state(doc);
    for (GList *l = doc->content_callbacks; l != NULL; l = l->next) {
        ContentCallbackData *cb = l->data;
        cb->func(doc, cb->user_data);
    }
}

/* Proxy UndoInfo type manually to avoid cyclic dep header hell if needed, 
   but we can include undo.h in document.h or forward declare. 
   For now, strictly include undo.h in document.h */

UndoInfo
document_undo(Document *doc)
{
    UndoInfo info = undo_stack_undo(doc->undo_stack, doc->pt);
    check_modification_state(doc);
    check_modification_state(doc);
    for (GList *l = doc->content_callbacks; l != NULL; l = l->next) {
        ContentCallbackData *cb = l->data;
        cb->func(doc, cb->user_data);
    }
    return info;
}

UndoInfo
document_redo(Document *doc)
{
    UndoInfo info = undo_stack_redo(doc->undo_stack, doc->pt);
    check_modification_state(doc);
    check_modification_state(doc);
    for (GList *l = doc->content_callbacks; l != NULL; l = l->next) {
        ContentCallbackData *cb = l->data;
        cb->func(doc, cb->user_data);
    }
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
document_add_modification_callback(Document *doc, void (*func)(Document *doc, gboolean modified, void *user_data), void *user_data)
{
    ModCallbackData *cb = g_new(ModCallbackData, 1);
    cb->func = func;
    cb->user_data = user_data;
    doc->mod_callbacks = g_list_append(doc->mod_callbacks, cb);
}

void
document_remove_modification_callback(Document *doc, void (*func)(Document *doc, gboolean modified, void *user_data), void *user_data)
{
    for (GList *l = doc->mod_callbacks; l != NULL; l = l->next) {
        ModCallbackData *cb = l->data;
        if (cb->func == func && cb->user_data == user_data) {
            doc->mod_callbacks = g_list_delete_link(doc->mod_callbacks, l);
            g_free(cb);
            return;
        }
    }
}

void
document_add_content_callback(Document *doc, DocumentContentCallback callback, void *user_data)
{
    ContentCallbackData *cb = g_new(ContentCallbackData, 1);
    cb->func = callback;
    cb->user_data = user_data;
    doc->content_callbacks = g_list_append(doc->content_callbacks, cb);
}

void
document_remove_content_callback(Document *doc, DocumentContentCallback callback, void *user_data)
{
    for (GList *l = doc->content_callbacks; l != NULL; l = l->next) {
        ContentCallbackData *cb = l->data;
        if (cb->func == callback && cb->user_data == user_data) {
            doc->content_callbacks = g_list_delete_link(doc->content_callbacks, l);
            g_free(cb);
            return;
        }
    }
}
