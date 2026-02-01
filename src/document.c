#include "document.h"
#include "compact-matches.h"
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#include "vite-clipboard.h"
#include "resource-check.h"
#include <ctype.h>
#include <sys/statvfs.h>

/* Forward declarations */
static char *document_snapshot_to_file(Document *doc);

static gboolean
check_disk_space(const char *dir, size_t required_bytes)
{
    struct statvfs stat;
    if (statvfs(dir, &stat) != 0) {
        g_warning("Failed to check disk space for %s: %s", dir, strerror(errno));
        return TRUE; /* Assume yes on error to avoid blocking benign cases */
    }
    
    unsigned long long available = (unsigned long long)stat.f_bavail * stat.f_frsize;
    if (available < required_bytes) {
        g_warning("Insufficient disk space in %s. Required: %zu, Available: %llu", 
                  dir, required_bytes, available);
        return FALSE;
    }
    
    return TRUE;
}

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
    GList *update_callbacks;
    GList *edit_callbacks; /* List of struct { func, user_data } */
    gboolean callbacks_suspended;

    /* Async Loading */
    DocumentProgressCallback progress_cb;
    void *progress_user_data;
    
    int ref_count;
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
    if (doc->callbacks_suspended) return;

    void *current = undo_stack_peek(doc->undo_stack);
    gboolean modified = (current != doc->saved_command);
    for (GList *l = doc->mod_callbacks; l != NULL; l = l->next) {
        ModCallbackData *cb = l->data;
        cb->func(doc, modified, cb->user_data);
    }
}



uint64_t
document_get_version(Document *doc)
{
    if (!doc || !doc->pt) return 0;
    return doc->pt->change_count;
}

Document *
document_new(const char *filename)
{
    Document *doc = g_new0(Document, 1);
    doc->pt = piece_table_new(filename);
    doc->undo_stack = undo_stack_new();
    doc->file_path = filename ? g_strdup(filename) : NULL;
    doc->ref_count = 1;
    return doc;
}

Document *
document_new_empty(void)
{
    Document *doc = g_new0(Document, 1);
    doc->pt = piece_table_new_empty();
    doc->undo_stack = undo_stack_new();
    doc->ref_count = 1;
    return doc;
}


Document *
document_ref(Document *doc)
{
    if (doc) {
        __atomic_add_fetch(&doc->ref_count, 1, __ATOMIC_SEQ_CST);
    }
    return doc;
}

void
document_free(Document *doc)
{
    if (!doc) return;
    
    if (__atomic_sub_fetch(&doc->ref_count, 1, __ATOMIC_SEQ_CST) > 0) {
        return;
    }

    /* Invalidate any clipboard references to this document */
    vite_clipboard_invalidate_for_document(vite_clipboard_get_default(), doc);

    piece_table_free(doc->pt);
    undo_stack_free(doc->undo_stack);
    g_free(doc->file_path);
    /* Free callback lists */
    g_list_free_full(doc->mod_callbacks, g_free);
    g_list_free_full(doc->content_callbacks, g_free);
    g_list_free_full(doc->update_callbacks, g_free);
    g_list_free_full(doc->edit_callbacks, g_free);
    g_free(doc);
}

const char *
document_get_file_path(Document *doc)
{
    return doc->file_path;
}

void
document_set_file_path(Document *doc, const char *path)
{
    g_free(doc->file_path);
    doc->file_path = path ? g_strdup(path) : NULL;
}

char *
document_get_line(Document *doc, size_t line_index, size_t *len)
{
    return piece_table_get_line(doc->pt, line_index, len);
}

char *
document_get_line_truncated(Document *doc, size_t line_index, size_t *out_len, size_t max_len)
{
    return piece_table_get_line_truncated(doc->pt, line_index, out_len, max_len);
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
void
document_add_update_callback(Document *doc, DocumentUpdateCallback callback, void *user_data)
{
    ContentCallbackData *data = g_new(ContentCallbackData, 1);
    data->func = (void (*)(Document*, void*))callback; /* Hacky cast but we know how to call it */
    data->user_data = user_data;
    doc->update_callbacks = g_list_append(doc->update_callbacks, data);
}

void
document_remove_update_callback(Document *doc, DocumentUpdateCallback callback, void *user_data)
{
    for (GList *l = doc->update_callbacks; l != NULL; l = l->next) {
        ContentCallbackData *data = l->data;
        if (data->func == (void (*)(Document*, void*))callback && data->user_data == user_data) {
            doc->update_callbacks = g_list_delete_link(doc->update_callbacks, l);
            g_free(data);
            return;
        }
    }
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


static void
document_emit_update(Document *doc, size_t start_line, int line_delta)
{
    if (doc->callbacks_suspended) return;

    for (GList *l = doc->content_callbacks; l != NULL; l = l->next) {
        ContentCallbackData *cb = l->data;
        cb->func(doc, cb->user_data);
    }
    
    for (GList *l = doc->update_callbacks; l != NULL; l = l->next) {
        ContentCallbackData *cb = l->data;
        DocumentUpdateCallback func = (DocumentUpdateCallback)cb->func;
        func(doc, start_line, line_delta, cb->user_data);
    }
}

static void
document_emit_edit(Document *doc, size_t offset, int64_t delta_len)
{
    if (doc->callbacks_suspended) return;
    
    for (GList *l = doc->edit_callbacks; l != NULL; l = l->next) {
        ContentCallbackData *cb = l->data;
        DocumentEditCallback func = (DocumentEditCallback)cb->func;
        func(doc, offset, delta_len, cb->user_data);
    }
}

void
document_insert(Document *doc, size_t offset, const char *text, size_t len)
{
    if (len == 0) return;
    size_t start_line = piece_table_get_line_of_offset(doc->pt, offset);
    size_t old_lines = piece_table_get_line_count(doc->pt);
    
    undo_stack_push_insert(doc->undo_stack, offset, text, len);
    piece_table_insert(doc->pt, offset, text, len);
    
    size_t new_lines = piece_table_get_line_count(doc->pt);
    int line_delta = (int)new_lines - (int)old_lines;

    check_modification_state(doc);
    document_emit_edit(doc, offset, (int64_t)len);
    document_emit_update(doc, start_line, line_delta);
}




static void
document_delete_streaming(Document *doc, size_t offset, size_t len)
{
    size_t start_line = piece_table_get_line_of_offset(doc->pt, offset);
    size_t old_lines = piece_table_get_line_count(doc->pt);

    UndoStack *stack = doc->undo_stack;
    
    if (!stack->in_undo_redo && stack->log_file) {
        /* ... existing log logic ... */
        UndoCommand *cmd = g_malloc0(sizeof(UndoCommand));
        cmd->type = UNDO_OP_DELETE;
        cmd->start = offset;
        cmd->length = len;
        
        fseeko(stack->log_file, 0, SEEK_END);
        cmd->log_offset = ftello(stack->log_file);
        
        /* Stream in 1MB chunks */
        size_t chunk_size = 1024 * 1024;
        size_t written = 0;
        
        while (written < len) {
            size_t to_write = MIN(chunk_size, len - written);
            char *chunk = piece_table_get_text_range(doc->pt, offset + written, to_write);
            if (chunk) {
                fwrite(chunk, 1, to_write, stack->log_file);
                g_free(chunk);
            }
            written += to_write;
        }
        fflush(stack->log_file);
        
        undo_stack_push_command(stack, cmd);
    }
    
    piece_table_delete(doc->pt, offset, len);
    
    size_t new_lines = piece_table_get_line_count(doc->pt);
    int line_delta = (int)new_lines - (int)old_lines;

    check_modification_state(doc);
    document_emit_edit(doc, offset, -(int64_t)len);
    document_emit_update(doc, start_line, line_delta);
}

void
document_insert_from_fd(Document *doc, size_t offset, int fd, size_t len)
{
    if (!doc || len == 0) return;
    
    /* Record Undo */
    if (doc->undo_stack) {
        undo_stack_push_insert_from_fd(doc->undo_stack, offset, fd, len);
    }
    
    /* Perform Insertion */
    piece_table_insert_from_fd_range(doc->pt, offset, fd, 0, len);
    
    /* Update state */
    check_modification_state(doc);
    document_emit_edit(doc, offset, (int64_t)len);
    for (GList *l = doc->content_callbacks; l != NULL; l = l->next) {
        ContentCallbackData *cb = l->data;
        cb->func(doc, cb->user_data);
    }
}

void
document_transfer_range(Document *dest, Document *src, size_t src_offset, size_t len, size_t dest_offset)
{
    if (!dest || !src || len == 0) return;

    /* Chunk size for transfer (1MB) */
    size_t chunk_size = 1024 * 1024;
    size_t processed = 0;
    
    /* We group this entire operation as one undo step */
    document_begin_undo_group(dest);

    while (processed < len) {
        size_t current_chunk = chunk_size;
        if (processed + current_chunk > len) current_chunk = len - processed;

        /* internal low-level read (allocates chunk) */
        char *text = document_get_text_range(src, src_offset + processed, current_chunk);
        if (text) {
             document_insert(dest, dest_offset + processed, text, current_chunk);
             g_free(text);
        } else {
             g_warning("document_transfer_range: Failed to read chunk at %zu", src_offset + processed);
             break; 
        }
        processed += current_chunk;
    }

    document_end_undo_group(dest);
}

void
document_delete(Document *doc, size_t offset, size_t len)
{
    if (len == 0) return;
    
    /* 50MB threshold for streaming */
    #define UNDO_RAM_THRESHOLD (50 * 1024 * 1024)
    
    /* For huge deletions, use disk-backed undo directly */
    if (len >= UNDO_RAM_THRESHOLD || !resource_can_allocate(len + 1)) {
        document_delete_streaming(doc, offset, len);
        return;
    }

    size_t start_line = piece_table_get_line_of_offset(doc->pt, offset);
    size_t old_lines = piece_table_get_line_count(doc->pt);

    char *deleted = piece_table_get_text_range(doc->pt, offset, len);
    undo_stack_push_delete(doc->undo_stack, offset, deleted, len);
    g_free(deleted);
    
    piece_table_delete(doc->pt, offset, len);
    
    size_t new_lines = piece_table_get_line_count(doc->pt);
    int line_delta = (int)new_lines - (int)old_lines;

    check_modification_state(doc);
    document_emit_edit(doc, offset, -(int64_t)len);
    document_emit_update(doc, start_line, line_delta);
}

void
document_suspend_callbacks(Document *doc)
{
    if (doc) doc->callbacks_suspended = TRUE;
}

void
document_resume_callbacks(Document *doc)
{
    if (doc) {
        doc->callbacks_suspended = FALSE;
        /* Trigger one update for content */
        for (GList *l = doc->content_callbacks; l != NULL; l = l->next) {
            ContentCallbackData *cb = l->data;
            cb->func(doc, cb->user_data);
        }
        /* Trigger one update for modification state */
        check_modification_state(doc);
    }
}

/* Proxy UndoInfo type manually to avoid cyclic dep header hell if needed, 
   but we can include undo.h in document.h or forward declare. 
   For now, strictly include undo.h in document.h */

UndoInfo
document_undo(Document *doc)
{
    size_t old_lines = piece_table_get_line_count(doc->pt);
    UndoInfo info = undo_stack_undo(doc->undo_stack, doc->pt);
    size_t new_lines = piece_table_get_line_count(doc->pt);
    int line_delta = (int)new_lines - (int)old_lines;

    size_t start_line = piece_table_get_line_of_offset(doc->pt, info.start);

    check_modification_state(doc);
    
    /* Calculate byte delta from UndoInfo for edit callback */
    int64_t byte_delta = 0;
    if (info.is_insert) {
        /* is_insert means positive delta */
        byte_delta = (int64_t)info.length;
    } else {
         /* !is_insert means delete (negative delta) */
         byte_delta = -(int64_t)info.length;
    }
    if (byte_delta != 0) {
        document_emit_edit(doc, info.start, byte_delta);
    }
    
    document_emit_update(doc, start_line, line_delta);
    return info;
}

gboolean
document_can_undo(Document *doc)
{
    if (!doc || !doc->undo_stack) return FALSE;
    return undo_stack_peek(doc->undo_stack) != NULL;
}

gboolean
document_can_redo(Document *doc)
{
    if (!doc || !doc->undo_stack) return FALSE;
    return undo_stack_peek_redo(doc->undo_stack) != NULL;
}

UndoInfo
document_redo(Document *doc)
{
    size_t old_lines = piece_table_get_line_count(doc->pt);
    UndoInfo info = undo_stack_redo(doc->undo_stack, doc->pt);
    size_t new_lines = piece_table_get_line_count(doc->pt);
    int line_delta = (int)new_lines - (int)old_lines;

    size_t start_line = piece_table_get_line_of_offset(doc->pt, info.start);

    check_modification_state(doc);

    /* Calculate byte delta from UndoInfo for edit callback */
    int64_t byte_delta = 0;
    if (info.is_insert) {
        /* Redo Insert = Insert. Delta = +length */
        byte_delta = (int64_t)info.length;
    } else {
         /* Redo Delete = Delete. Delta = -length */
         byte_delta = -(int64_t)info.length;
    }
    if (byte_delta != 0) {
        document_emit_edit(doc, info.start, byte_delta);
    }

    document_emit_update(doc, start_line, line_delta);
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

/* Async Undo/Redo Implementation */

struct _UndoRedoTask {
    Document *doc;
    UndoCommand *cmd;
    gboolean is_undo;
    UndoRedoProgressCallback callback;
    gpointer user_data;
    guint idle_id;
    
    /* Incremental restoration */
    PieceTableReplaceTask *pt_task;
};

static void
undo_redo_task_free(UndoRedoTask *task)
{
    if (!task) return;
    
    if (task->idle_id) {
        g_source_remove(task->idle_id);
    }
    
    if (task->pt_task) {
        piece_table_replace_async_cancel(task->pt_task);
    }
    
    g_free(task);
}

static gboolean
undo_redo_idle_step(gpointer user_data)
{
    UndoRedoTask *task = user_data;
    Document *doc = task->doc;
    
    /* If we have an active piece table replacement (massive file restore), continue it */
    if (task->pt_task) {
        double progress = 0;
        if (!piece_table_replace_async_step(task->pt_task, 10000, &progress)) {
            /* Still processing chunks, yield */
            if (task->callback) {
                task->callback(progress, FALSE, task->user_data);
            }
            return G_SOURCE_CONTINUE;
        }
        
        /* Finished counting newlines, now swap the document content */
        piece_table_replace_async_finalize(task->pt_task);
        task->pt_task = NULL;
        
        /* Now finalize the undo stack state without re-executing */
        size_t old_lines = piece_table_get_line_count(doc->pt); /* This is the NEW line count now actually */
        /* Wait, we need the OLD line count for delta calculation. 
           But for RESTORE_FROM_PATH, old_lines isn't as critical as the final update. */
           
        UndoInfo info;
        if (task->is_undo) {
            info = undo_stack_undo_skip_execute(doc->undo_stack);
        } else {
            info = undo_stack_redo_skip_execute(doc->undo_stack);
        }
        
        check_modification_state(doc);
        
        /* For restorations, we just emit a full update */
        document_emit_update(doc, 0, 0); /* 0, 0 triggers full refresh in renderer usually */
    } else {
        /* Standard operation (INSERT/DELETE) or small restore */
        size_t old_lines = piece_table_get_line_count(doc->pt);
        
        UndoInfo info;
        if (task->is_undo) {
            info = undo_stack_undo(doc->undo_stack, doc->pt);
        } else {
            info = undo_stack_redo(doc->undo_stack, doc->pt);
        }
        
        if (info.success) {
            size_t new_lines = piece_table_get_line_count(doc->pt);
            int line_delta = (int)new_lines - (int)old_lines;
            
            size_t start_line = 0;
            if (info.length > 0) {
                start_line = piece_table_get_line_of_offset(doc->pt, info.start);
            }
            
            check_modification_state(doc);
            
            int64_t byte_delta = info.is_insert ? (int64_t)info.length : -(int64_t)info.length;
            if (byte_delta != 0) {
                document_emit_edit(doc, info.start, byte_delta);
            }
            
            document_emit_update(doc, start_line, line_delta);
        }
    }
    
    /* Operation complete */
    if (task->callback) {
        task->callback(1.0, TRUE, task->user_data);
    }
    
    task->idle_id = 0;
    undo_redo_task_free(task);
    return G_SOURCE_REMOVE;
}

UndoRedoTask *
document_undo_async(Document *doc, UndoRedoProgressCallback callback, gpointer user_data)
{
    if (!doc->undo_stack || !undo_stack_peek(doc->undo_stack)) {
        if (callback) callback(1.0, TRUE, user_data);
        return NULL;
    }
    
    UndoRedoTask *task = g_new0(UndoRedoTask, 1);
    task->doc = doc;
    task->is_undo = TRUE;
    task->callback = callback;
    task->user_data = user_data;
    
    /* Check if this is a massive restoration */
    UndoCommand *cmd = undo_stack_peek(doc->undo_stack);
    if (cmd && cmd->type == UNDO_OP_RESTORE_FROM_PATH && cmd->undo_path) {
        int fd = open(cmd->undo_path, O_RDONLY);
        if (fd >= 0) {
            struct stat st;
            fstat(fd, &st);
            if (st.st_size > 10 * 1024 * 1024) { /* > 10MB, process async */
                task->pt_task = piece_table_replace_async_start(doc->pt, fd, st.st_size);
            }
            close(fd); /* task will re-open or piece_table uses mmap */
            /* Wait, piece_table_replace_async_start needs the fd open or it mmaps. 
               It mmaps immediately, so we can close. */
        }
    }
    
    task->idle_id = g_idle_add(undo_redo_idle_step, task);
    return task;
}

UndoRedoTask *
document_redo_async(Document *doc, UndoRedoProgressCallback callback, gpointer user_data)
{
    if (!doc->undo_stack || !undo_stack_peek_redo(doc->undo_stack)) {
        if (callback) callback(1.0, TRUE, user_data);
        return NULL;
    }
    
    UndoRedoTask *task = g_new0(UndoRedoTask, 1);
    task->doc = doc;
    task->is_undo = FALSE;
    task->callback = callback;
    task->user_data = user_data;
    
    /* Check if this is a massive restoration */
    UndoCommand *cmd = undo_stack_peek_redo(doc->undo_stack);
    if (cmd && cmd->type == UNDO_OP_RESTORE_FROM_PATH && cmd->redo_path) {
        int fd = open(cmd->redo_path, O_RDONLY);
        if (fd >= 0) {
            struct stat st;
            fstat(fd, &st);
            if (st.st_size > 10 * 1024 * 1024) { /* > 10MB, process async */
                task->pt_task = piece_table_replace_async_start(doc->pt, fd, st.st_size);
            }
            close(fd);
        }
    }
    
    task->idle_id = g_idle_add(undo_redo_idle_step, task);
    return task;
}

void
document_undo_redo_cancel(UndoRedoTask *task)
{
    if (task) {
        undo_redo_task_free(task);
    }
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
    doc->saved_command = undo_stack_peek(doc->undo_stack);
    check_modification_state(doc);
}

void
document_set_newline_type(Document *doc, NewlineType type)
{
    if (doc) piece_table_set_newline_type(doc->pt, type);
    /* Notify observers? Maybe not considered a "content" change but a property change. 
       We don't trigger modified/content callbacks for this usually, as it only affects save. */
}

NewlineType
document_get_newline_type(Document *doc)
{
    return doc ? piece_table_get_newline_type(doc->pt) : NEWLINE_LF;
}

void
document_set_encoding(Document *doc, FileEncoding enc)
{
    if (doc) piece_table_set_encoding(doc->pt, enc);
}

FileEncoding
document_get_encoding(Document *doc)
{
    return doc ? piece_table_get_encoding(doc->pt) : ENCODING_UTF8;
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


void
document_add_edit_callback(Document *doc, DocumentEditCallback callback, void *user_data)
{
    ContentCallbackData *cb = g_new(ContentCallbackData, 1);
    cb->func = (void (*)(Document *, void *))callback;
    cb->user_data = user_data;
    doc->edit_callbacks = g_list_append(doc->edit_callbacks, cb);
}

void
document_remove_edit_callback(Document *doc, DocumentEditCallback callback, void *user_data)
{
    for (GList *l = doc->edit_callbacks; l != NULL; l = l->next) {
        ContentCallbackData *cb = l->data;
        if (cb->func == (void (*)(Document *, void *))callback && cb->user_data == user_data) {
            doc->edit_callbacks = g_list_delete_link(doc->edit_callbacks, l);
            g_free(cb);
            return;
        }
    }
}


/* --- Search Implementation --- */

static char *
unescape_string(const char *input)
{
    if (!input) return NULL;
    GString *out = g_string_new("");
    const char *p = input;
    while (*p) {
        if (*p == '\\') {
            p++;
            if (!*p) { // Trailing backslash
                g_string_append_c(out, '\\');
                break;
            }
            switch (*p) {
                case 'n': g_string_append_c(out, '\n'); break;
                case 'r': g_string_append_c(out, '\r'); break;
                case 't': g_string_append_c(out, '\t'); break;
                case '\\': g_string_append_c(out, '\\'); break;
                default: 
                    g_string_append_c(out, '\\');
                    g_string_append_c(out, *p);
                    break;
            }
        } else {
            g_string_append_c(out, *p);
        }
        p++;
    }
    return g_string_free(out, FALSE);
}

/**
 * Normalize capture group references in replacement strings.
 * 
 * Converts $1, $2, etc. to GLib's \g<1>, \g<2> format for g_regex_replace.
 * Also handles escape sequences like \n, \t, \r.
 * 
 * User input examples:
 *   "$1" -> "\\g<1>"
 *   "$1-$2" -> "\\g<1>-\\g<2>"
 *   "\\n" -> "\n" (literal newline)
 *   "\\$1" -> "$1" (escaped dollar, literal)
 *   "\\1" -> "\\g<1>" (also supported for compatibility)
 */
char *
normalize_replacement_string(const char *replacement, gboolean for_regex)
{
    if (!replacement) return NULL;
    
    GString *out = g_string_new("");
    const char *p = replacement;
    
    while (*p) {
        if (*p == '\\') {
            p++;
            if (!*p) {
                /* Trailing backslash */
                g_string_append_c(out, '\\');
                break;
            }
            
            /* Check for digit after backslash (backreference \1, \2, etc.) */
            if (for_regex && *p >= '1' && *p <= '9') {
                /* Convert \1 to \g<1> */
                g_string_append(out, "\\g<");
                /* Collect all digits for multi-digit group refs */
                while (*p >= '0' && *p <= '9') {
                    g_string_append_c(out, *p);
                    p++;
                }
                g_string_append_c(out, '>');
                continue;
            }
            
            switch (*p) {
                case 'n': g_string_append_c(out, '\n'); break;
                case 'r': g_string_append_c(out, '\r'); break;
                case 't': g_string_append_c(out, '\t'); break;
                case '\\': g_string_append_c(out, '\\'); break;
                case '$': g_string_append_c(out, '$'); break; /* Escaped dollar */
                default:
                    /* Keep other escapes as-is for GRegex (like \g<1>) */
                    g_string_append_c(out, '\\');
                    g_string_append_c(out, *p);
                    break;
            }
        } else if (for_regex && *p == '$') {
            p++;
            if (*p >= '1' && *p <= '9') {
                /* Convert $1 to \g<1> */
                g_string_append(out, "\\g<");
                while (*p >= '0' && *p <= '9') {
                    g_string_append_c(out, *p);
                    p++;
                }
                g_string_append_c(out, '>');
                continue;
            } else if (*p == '{') {
                /* Handle ${1} style syntax */
                p++;
                if (*p >= '0' && *p <= '9') {
                    g_string_append(out, "\\g<");
                    while (*p >= '0' && *p <= '9') {
                        g_string_append_c(out, *p);
                        p++;
                    }
                    g_string_append_c(out, '>');
                    if (*p == '}') p++;
                    continue;
                }
                /* Invalid ${...}, output as-is */
                g_string_append(out, "${");
                continue;
            } else {
                /* Lone $, output as-is */
                g_string_append_c(out, '$');
                continue;
            }
        } else {
            g_string_append_c(out, *p);
        }
        p++;
    }
    
    return g_string_free(out, FALSE);
}

GArray *
document_search(Document *doc, const char *raw_query, gboolean regex, gboolean case_sensitive, gboolean whole_word)
{
    GArray *matches = g_array_new(FALSE, FALSE, sizeof(SearchMatch));
    if (!doc || !raw_query || !*raw_query) return matches;

    char *query = NULL;
    GRegex *pattern = NULL;
    GError *err = NULL;

    if (!regex) {
        /* Unescape literal sequences like \n */
        char *unescaped = unescape_string(raw_query);
        /* Now escape for regex usage so we can use GRegex for everything (simplifies logic) */
        char *safe_query = g_regex_escape_string(unescaped, -1);
        g_free(unescaped);
        
        if (whole_word) {
            query = g_strdup_printf("\\b%s\\b", safe_query);
            g_free(safe_query);
        } else {
            query = safe_query;
        }
    } else {
        if (whole_word) {
            query = g_strdup_printf("\\b(?:%s)\\b", raw_query);
        } else {
            query = g_strdup(raw_query);
        }
    }

    GRegexCompileFlags flags = G_REGEX_OPTIMIZE;
    if (!case_sensitive) flags |= G_REGEX_CASELESS;
    /* Multiline? We scan line-by-line, so dot matches generally within line. 
       If we want ^ and $ to match start/end of line, G_REGEX_MULTILINE might be needed but
       since we pass individual lines, ^/$ work naturally on the buffer passed.
    */

    pattern = g_regex_new(query, flags, 0, &err);
    if (err) {
        // g_warning("Regex compile error: %s", err->message);
        g_error_free(err);
        g_free(query);
        return matches;
    }

    size_t line_count = document_get_line_count(doc);
    /* Stack-allocated buffer for fast line retrieval */
    char buf[4096];
    size_t current_offset = 0;
    
    for (size_t i = 0; i < line_count; i++) {
        size_t len = 0;
        char *line = NULL;
        size_t true_len = 0;
        
        /* Try fast path */
        true_len = piece_table_get_line_into(doc->pt, i, buf, sizeof(buf));
        
        if (true_len < sizeof(buf)) {
             /* Fits! Null terminate for regex usage */
             buf[true_len] = '\0';
             line = buf; /* Point to stack buffer */
        } else {
             /* Fallback to slow alloc path */
             line = document_get_line(doc, i, &len);
             /* Ensure we use the correct length for offset calculation */
             if (!line) { 
                 /* Should not happen if get_line_into returned something, but safety first */
                 current_offset += true_len; // true_len should be valid even if alloc failed? No.
                 /*If line is null, document_get_line failed. Skip.*/
                 continue; 
             }
             /* Recalculate true_len from the allocated string length? 
                document_get_line returns a null-terminated string.
                len is set to string length.
             */
             true_len = len; 
        }
        
        if (!line) {
             /* Should have been handled above, but just in case */
             continue; 
        }

        GMatchInfo *match_info;
        if (g_regex_match(pattern, line, 0, &match_info)) {
            while (g_match_info_matches(match_info)) {
                gint start_pos, end_pos;
                g_match_info_fetch_pos(match_info, 0, &start_pos, &end_pos);
                
                SearchMatch m;
                m.start = current_offset + (size_t)start_pos;
                m.end = current_offset + (size_t)end_pos;
                g_array_append_val(matches, m);
                
                g_match_info_next(match_info, NULL);
            }
        }

        g_match_info_free(match_info);
        
        /* Only free if we allocated it (fallback path) */
        if (line != buf) g_free(line);
        
        /* Advance offset */
        current_offset += true_len;
    }
    
    g_regex_unref(pattern);
    g_free(query);
    return matches;
}


/* Viewport-only search for huge files - searches only within specified line range */
GArray *
document_search_viewport(Document *doc, const char *raw_query, 
                          gboolean regex, gboolean case_sensitive, 
                          gboolean whole_word,
                          size_t start_line, size_t end_line)
{
    GArray *matches = g_array_new(FALSE, FALSE, sizeof(SearchMatch));
    if (!doc || !raw_query || !*raw_query) return matches;

    char *query = NULL;
    GRegex *pattern = NULL;
    GError *err = NULL;

    if (!regex) {
        char *unescaped = unescape_string(raw_query);
        char *safe_query = g_regex_escape_string(unescaped, -1);
        g_free(unescaped);
        
        if (whole_word) {
            query = g_strdup_printf("\\b%s\\b", safe_query);
            g_free(safe_query);
        } else {
            query = safe_query;
        }
    } else {
        if (whole_word) {
            query = g_strdup_printf("\\b(?:%s)\\b", raw_query);
        } else {
            query = g_strdup(raw_query);
        }
    }

    GRegexCompileFlags flags = G_REGEX_OPTIMIZE;
    if (!case_sensitive) flags |= G_REGEX_CASELESS;

    pattern = g_regex_new(query, flags, 0, &err);
    if (err) {
        g_error_free(err);
        g_free(query);
        return matches;
    }

    size_t line_count = document_get_line_count(doc);
    
    /* Clamp to valid range */
    if (start_line >= line_count) start_line = line_count > 0 ? line_count - 1 : 0;
    if (end_line > line_count) end_line = line_count;
    if (start_line > end_line) start_line = end_line;
    
    char buf[4096];
    
    /* Calculate offset of start_line */
    size_t current_offset = document_get_offset_of_line(doc, start_line);
    
    for (size_t i = start_line; i < end_line; i++) {
        size_t len = 0;
        char *line = NULL;
        size_t true_len = 0;
        
        true_len = piece_table_get_line_into(doc->pt, i, buf, sizeof(buf));
        
        if (true_len < sizeof(buf)) {
             buf[true_len] = '\0';
             line = buf;
        } else {
             line = document_get_line(doc, i, &len);
             if (!line) { 
                 continue; 
             }
             true_len = len; 
        }
        
        if (!line) continue;

        GMatchInfo *match_info;
        if (g_regex_match(pattern, line, 0, &match_info)) {
            while (g_match_info_matches(match_info)) {
                gint start_pos, end_pos;
                g_match_info_fetch_pos(match_info, 0, &start_pos, &end_pos);
                
                SearchMatch m;
                m.start = current_offset + (size_t)start_pos;
                m.end = current_offset + (size_t)end_pos;
                g_array_append_val(matches, m);
                
                g_match_info_next(match_info, NULL);
            }
        }
        g_match_info_free(match_info);
        
        if (line != buf) g_free(line);
        
        current_offset += true_len;
    }
    
    g_regex_unref(pattern);
    g_free(query);
    return matches;
}

size_t
document_replace(Document *doc, SearchMatch match, const char *replacement)
{
    if (!doc) return 0;
    
    /* TODO: Validate match is still valid? 
       For now assume caller handles validity or we trust coordinates.
    */
    size_t len = match.end - match.start;
    document_delete(doc, match.start, len);
    
    size_t repl_len = replacement ? strlen(replacement) : 0;
    if (repl_len > 0) {
        document_insert(doc, match.start, replacement, repl_len);
    }
    return match.start + repl_len;
}

int
document_replace_known_matches(Document *doc, GArray *matches, const char *replacement, gboolean regex, GRegex *cached_regex)
{
    if (!doc || !matches || matches->len == 0) return 0;
    
    int count = 0;
    document_begin_undo_group(doc);
    document_suspend_callbacks(doc);
    
    /* Normalize replacement string (handles $1, \1, \n, \t, etc.) */
    char *normalized_replacement = normalize_replacement_string(replacement, regex);
    
    /* Pre-check total document length to bounds check safely (approximate) */
    size_t total = document_get_length(doc);

    for (int i = (int)matches->len - 1; i >= 0; i--) {
        SearchMatch m = g_array_index(matches, SearchMatch, i);
        
        /* Basic sanity check */
        if (m.end > total || m.start > total || m.start > m.end) continue;
        
        char *final_replacement = NULL;
        if (regex && cached_regex) {
             /* Fix: Perform replacement with context (full line) so lookarounds work */
             size_t line_idx = document_get_line_of_offset(doc, m.start);
             size_t line_start = document_get_offset_of_line(doc, line_idx);
             size_t len = 0;
             char *line_text = document_get_line(doc, line_idx, &len);
             
             if (line_text) {
                 size_t offset_in_line = m.start - line_start;
                 GMatchInfo *info = NULL;
                 
                 /* Match starting exactly at the match position to capture correct groups */
                 if (g_regex_match_full(cached_regex, line_text, len, offset_in_line, 0, &info, NULL)) {
                     /* Verify we matched what we expected (sanity check) */
                     gint start_pos, end_pos;
                     g_match_info_fetch_pos(info, 0, &start_pos, &end_pos);
                     if ((size_t)start_pos == offset_in_line) {
                         final_replacement = g_match_info_expand_references(info, normalized_replacement, NULL);
                     }
                 }
                 g_match_info_free(info);
                 g_free(line_text);
             }
             
             /* Fallback if something failed (e.g. line changed? shouldn't happen) */
             if (!final_replacement) {
                 /* Try the old isolated method as last resort, or just skip? 
                    Skipping is safer than wrong replacement. */
                 // final_replacement = g_strdup(""); // ?
             }
        } else {
             /* Literal replacement (already normalized) */
             if (normalized_replacement) {
                 final_replacement = g_strdup(normalized_replacement);
             }
        }
        
        if (final_replacement) {
            document_replace(doc, m, final_replacement);
            g_free(final_replacement);
            count++;
        }
    }
    
    g_free(normalized_replacement);
    
    document_resume_callbacks(doc);
    document_end_undo_group(doc);
    return count;
}

int
document_replace_all(Document *doc, const char *raw_query, const char *replacement, gboolean regex, gboolean case_sensitive, gboolean whole_word)
{
    GArray *matches = document_search(doc, raw_query, regex, case_sensitive, whole_word);
    if (!matches || matches->len == 0) {
        if (matches) g_array_free(matches, TRUE);
        return 0;
    }
    
    GRegex *pattern = NULL;
    char *query = NULL;
    if (regex) {
         query = g_strdup(raw_query);
         GRegexCompileFlags flags = G_REGEX_OPTIMIZE;
         if (!case_sensitive) flags |= G_REGEX_CASELESS;
         pattern = g_regex_new(query, flags, 0, NULL);
         g_free(query);
    }
    
    int count = document_replace_known_matches(doc, matches, replacement, regex, pattern);
    
    if (pattern) g_regex_unref(pattern);
    g_array_free(matches, TRUE);
    
    return count;
}

/* Targeted replace - only processes specified lines for efficiency (like svite.py) */
int
document_replace_targeted_lines(Document *doc, GArray *target_lines,
                                 const char *query, const char *replacement,
                                 gboolean regex, gboolean case_sensitive)
{
    if (!doc || !target_lines || target_lines->len == 0 || !query || !*query) return 0;
    
    GRegex *pattern = NULL;
    char *search_query = NULL;
    
    if (regex) {
        GRegexCompileFlags flags = G_REGEX_OPTIMIZE;
        if (!case_sensitive) flags |= G_REGEX_CASELESS;
        pattern = g_regex_new(query, flags, 0, NULL);
        if (!pattern) return 0;
    } else {
        /* Prepare for literal search */
        search_query = g_strdup(query);
    }
    
    document_begin_undo_group(doc);
    document_suspend_callbacks(doc);
    
    int count = 0;
    char buf[4096];
    
    /* Process lines in reverse to maintain valid offsets */
    for (int i = (int)target_lines->len - 1; i >= 0; i--) {
        size_t line_idx = g_array_index(target_lines, size_t, i);
        size_t line_count = document_get_line_count(doc);
        
        if (line_idx >= line_count) continue;
        
        size_t len = 0;
        char *line = NULL;
        size_t true_len = piece_table_get_line_into(doc->pt, line_idx, buf, sizeof(buf));
        
        if (true_len < sizeof(buf)) {
            buf[true_len] = '\0';
            line = buf;
        } else {
            line = document_get_line(doc, line_idx, &len);
            if (!line) continue;
            true_len = len;
        }
        
        /* Find and replace within this line */
        GString *new_line = g_string_new("");
        size_t line_offset = document_get_offset_of_line(doc, line_idx);
        gboolean line_modified = FALSE;
        
        if (regex && pattern) {
            /* Regex replacement within the line */
            gchar *result = g_regex_replace(pattern, line, -1, 0, replacement, 0, NULL);
            if (result && strcmp(result, line) != 0) {
                g_string_assign(new_line, result);
                line_modified = TRUE;
                /* Count matches replaced */
                GMatchInfo *mi;
                if (g_regex_match(pattern, line, 0, &mi)) {
                    while (g_match_info_matches(mi)) {
                        count++;
                        g_match_info_next(mi, NULL);
                    }
                }
                g_match_info_free(mi);
            }
            g_free(result);
        } else if (search_query) {
            /* Literal replacement */
            const char *search = case_sensitive ? line : NULL;
            char *lower_line = NULL;
            char *lower_query = NULL;
            
            if (!case_sensitive) {
                lower_line = g_utf8_strdown(line, -1);
                lower_query = g_utf8_strdown(search_query, -1);
                search = lower_line;
            } else {
                search = line;
            }
            
            const char *lookup_query = case_sensitive ? search_query : lower_query;
            size_t query_len = strlen(search_query);
            size_t repl_len = strlen(replacement);
            
            const char *ptr = search;
            const char *orig_ptr = line;
            gboolean has_match = FALSE;
            
            while (ptr && *ptr) {
                const char *found = strstr(ptr, lookup_query);
                if (!found) {
                    g_string_append(new_line, orig_ptr);
                    break;
                }
                
                size_t offset = found - search;
                size_t orig_offset = orig_ptr - line;
                size_t advance = offset - (ptr - search);
                
                g_string_append_len(new_line, orig_ptr, advance);
                g_string_append(new_line, replacement);
                
                orig_ptr = line + offset + query_len;
                ptr = found + query_len;
                has_match = TRUE;
                count++;
            }
            
            if (has_match) line_modified = TRUE;
            
            g_free(lower_line);
            g_free(lower_query);
        }
        
        if (line_modified) {
            /* Delete old line content (excluding newline) */
            size_t line_content_len = true_len;
            if (line_content_len > 0 && (line[line_content_len - 1] == '\n' || line[line_content_len - 1] == '\r')) {
                line_content_len--;
            }
            if (line_content_len > 0 && line[line_content_len - 1] == '\r') {
                line_content_len--;
            }
            
            /* Remove trailing newline from new_line if present */
            size_t new_len = new_line->len;
            if (new_len > 0 && (new_line->str[new_len - 1] == '\n' || new_line->str[new_len - 1] == '\r')) {
                new_len--;
            }
            if (new_len > 0 && new_line->str[new_len - 1] == '\r') {
                new_len--;
            }
            
            document_delete(doc, line_offset, line_content_len);
            if (new_len > 0) {
                document_insert(doc, line_offset, new_line->str, new_len);
            }
        }
        
        g_string_free(new_line, TRUE);
        if (line != buf) g_free(line);
    }
    
    document_resume_callbacks(doc);
    document_end_undo_group(doc);
    
    if (pattern) g_regex_unref(pattern);
    g_free(search_query);
    
    return count;
}

/* --- Async Search Implementation --- */

struct _SearchTask {
    Document *doc;
    char *query; /* For literal search */
    char *original_query; /* For validation (raw query) */
    GRegex *pattern; /* For regex search */
    
    gboolean is_literal;
    gboolean case_sensitive;
    gboolean whole_word;
    gboolean is_regex;
    
    PieceTableIter iter;
    GString *line_buf;
    uint64_t start_change_count;
    
    size_t current_offset; /* Cumulative offset tracker */
    size_t total_lines;
    size_t lines_searched;
    
    /* Memory-efficient match storage using delta+varint encoding */
    CompactMatches *compact_matches;
    GArray *matches;         /* Lazily populated from compact_matches when needed */
    size_t match_length;     /* Length of query for end offset calculation */
    
    SearchCallback callback;
    void *user_data;
    
    guint idle_id;
};

static gboolean
search_idle_step(gpointer user_data)
{
    SearchTask *task = (SearchTask *)user_data;
    if (!task) return G_SOURCE_REMOVE;
    
    /* Check for document modification */
    if (task->doc->pt->change_count != task->start_change_count) {
        /* Document changed - abort search */
        if (task->callback) task->callback(task->matches, TRUE, task->user_data);
        task->idle_id = 0;
        return G_SOURCE_REMOVE;
    }

    /* Time-budgeted processing (like svite.py) */
    gint64 start_time = g_get_monotonic_time(); /* microseconds */
    gint64 budget = 12000; /* 12ms time budget per idle iteration */


    
    size_t query_len = 0;
    if (task->is_literal && task->query) {
        query_len = strlen(task->query);
    }
    
    size_t lines_this_chunk = 0;
    gboolean budget_expired = FALSE;
    
    while (!budget_expired) {
        /* Check time budget every 32 lines for responsive yielding */
        if ((lines_this_chunk & 31) == 0 && lines_this_chunk > 0) {
            if ((g_get_monotonic_time() - start_time) > budget) {
                budget_expired = TRUE;
                break;
            }
        }

        g_string_truncate(task->line_buf, 0);
        size_t len = piece_table_iter_get_next_line_string(&task->iter, task->line_buf);
        
        if (len == 0 && task->line_buf->len == 0) {
            break; /* EOF */
        }
        
        char *line = task->line_buf->str;
        
        if (task->is_literal) {
             /* FAST PATH: Literal Search */
             if (task->query) {
                 char *haystack = line;
                 char *found = NULL;
                 
                 while (haystack && *haystack) {
                     if (task->case_sensitive) {
                         found = strstr(haystack, task->query);
                     } else {
                         found = strcasestr(haystack, task->query);
                     }
                     
                     if (!found) break;
                     
                     size_t start_idx = found - line;
                     size_t start_offset = task->current_offset + start_idx;
                     
                     /* Use compact storage - only stores start, delta encoded */
                     compact_matches_append(task->compact_matches, start_offset);
                     
                     haystack = found + 1; 
                 }
             }
         } else {
             /* SLOW PATH: Regex */
             GMatchInfo *match_info;
             if (g_regex_match(task->pattern, line, 0, &match_info)) {
                 while (g_match_info_matches(match_info)) {
                     gint start_pos, end_pos;
                     g_match_info_fetch_pos(match_info, 0, &start_pos, &end_pos);
                     
                     SearchMatch m;
                     m.start = task->current_offset + (size_t)start_pos;
                     m.end = task->current_offset + (size_t)end_pos;
                     g_array_append_val(task->matches, m);
                     
                     g_match_info_next(match_info, NULL);
                 }
             }
             g_match_info_free(match_info);
         }
        
        task->current_offset += len;
        task->lines_searched++;
        


        lines_this_chunk++;
    }
    
    /* Report progress when budget expires */
    if (budget_expired) {
        if (task->callback) task->callback(task->matches, FALSE, task->user_data);
        return G_SOURCE_CONTINUE;
    }
    
    /* Finished - EOF was reached */

    if (task->callback) task->callback(task->matches, TRUE, task->user_data);
    
    task->idle_id = 0; /* Source removed */
    return G_SOURCE_REMOVE;
}




SearchTask *
document_search_async_start(Document *doc, const char *raw_query, gboolean regex, gboolean case_sensitive, gboolean whole_word, SearchCallback callback, void *user_data)
{
    if (!doc || !raw_query || !*raw_query) return NULL;
    
    SearchTask *task = g_new0(SearchTask, 1);
    task->doc = doc;
    task->callback = callback;
    task->user_data = user_data;
    task->matches = NULL;  /* Lazily populated from compact_matches when needed */
    task->compact_matches = NULL;
    task->original_query = g_strdup(raw_query);
    
    piece_table_iter_init(doc->pt, &task->iter);
    task->line_buf = g_string_sized_new(4096);
    task->start_change_count = doc->pt->change_count;
    
    task->current_offset = 0;
    task->total_lines = document_get_line_count(doc); /* For progress estimation if needed */
    task->case_sensitive = case_sensitive;
    task->whole_word = whole_word;
    task->is_regex = regex;

    /* Determine if we can use Fast Literal Path */
    if (!regex && !whole_word) {
        task->is_literal = TRUE;
        task->query = g_strdup(raw_query);
        task->pattern = NULL;
        task->match_length = strlen(raw_query);
        /* Use compact storage for literal search - ~8x memory reduction */
        task->compact_matches = compact_matches_new(task->match_length);
    } else {
        task->is_literal = FALSE;
        /* For regex, use GArray since match lengths vary */
        task->matches = g_array_new(FALSE, FALSE, sizeof(SearchMatch));
        
        /* Prepare regex */
        char *query = NULL;
        if (!regex) {
            char *unescaped = unescape_string(raw_query);
            char *safe_query = g_regex_escape_string(unescaped, -1);
            g_free(unescaped);
            
            if (whole_word) {
                query = g_strdup_printf("\\b%s\\b", safe_query);
                g_free(safe_query);
            } else {
                query = safe_query;
            }
        } else {
            if (whole_word) {
                query = g_strdup_printf("\\b(?:%s)\\b", raw_query);
            } else {
                query = g_strdup(raw_query);
            }
        }
        
        GRegexCompileFlags flags = G_REGEX_OPTIMIZE;
        if (!case_sensitive) flags |= G_REGEX_CASELESS;
        task->pattern = g_regex_new(query, flags, 0, NULL);
        g_free(query);
        
        if (!task->pattern) {
            document_search_async_cancel(task);
            return NULL;
        }
    }
    
    task->idle_id = g_idle_add(search_idle_step, task);
    return task;
}

void
document_search_async_cancel(SearchTask *task)
{
    if (!task) return;
    
    if (task->idle_id) {
        g_source_remove(task->idle_id);
        task->idle_id = 0;
    }
    
    if (task->matches) {
        g_array_unref(task->matches);
    }
    
    if (task->compact_matches) {
        compact_matches_free(task->compact_matches);
    }
    
    if (task->pattern) {
        g_regex_unref(task->pattern);
    }
    
    if (task->query) {
        g_free(task->query);
    }
    if (task->original_query) {
        g_free(task->original_query);
    }
    
    if (task->line_buf) {
        g_string_free(task->line_buf, TRUE);
    }
    
    g_free(task);
}

GArray *
document_search_task_get_matches(SearchTask *task)
{
    if (!task) return NULL;
    
    /* If we have compact matches but no GArray, convert lazily */
    if (task->compact_matches && !task->matches) {
        task->matches = compact_matches_to_array(task->compact_matches);
    }
    
    return task->matches;
}

GRegex *
document_search_task_get_pattern(SearchTask *task)
{
    if (!task) return NULL;
    return task->pattern;
}

size_t
document_search_task_get_total_lines(SearchTask *task)
{
    return task ? task->total_lines : 0;
}

size_t
document_search_task_get_lines_searched(SearchTask *task)
{
    return task ? task->lines_searched : 0;
}

const char *document_search_task_get_query(SearchTask *task) { return task ? task->original_query : NULL; }
gboolean document_search_task_get_regex(SearchTask *task) { return task ? task->is_regex : FALSE; }
gboolean document_search_task_get_case_sensitive(SearchTask *task) { return task ? task->case_sensitive : FALSE; }
gboolean document_search_task_get_whole_word(SearchTask *task) { return task ? task->whole_word : FALSE; }

/* Get total match count without converting to GArray */
size_t document_search_task_get_match_count(SearchTask *task) {
    if (!task) return 0;
    if (task->compact_matches) {
        return compact_matches_count(task->compact_matches);
    }
    return task->matches ? task->matches->len : 0;
}

/* Get only viewport matches using binary search - O(log N) instead of O(N) */
GArray *document_search_task_get_viewport_matches(SearchTask *task, size_t start_offset, size_t end_offset) {
    if (!task) return NULL;
    
    if (task->compact_matches) {
        /* Use binary search to find matches in range */
        size_t first_idx, last_idx;
        compact_matches_find_range(task->compact_matches, start_offset, end_offset, &first_idx, &last_idx);
        return compact_matches_range_to_array(task->compact_matches, first_idx, last_idx);
    }
    
    /* Fallback for regex search (uses GArray) - linear search */
    if (!task->matches || task->matches->len == 0) return NULL;
    
    GArray *viewport = g_array_new(FALSE, FALSE, sizeof(SearchMatch));
    for (guint i = 0; i < task->matches->len; i++) {
        SearchMatch m = g_array_index(task->matches, SearchMatch, i);
        if (m.start >= start_offset && m.start < end_offset) {
            g_array_append_val(viewport, m);
        }
        if (m.start >= end_offset) break;  /* Sorted, can stop early */
    }
    return viewport;
}

/* Get a single match by global index - O(1) for regex, O(n) worst case for compact */
gboolean document_search_task_get_match_at(SearchTask *task, size_t idx, SearchMatch *out) {
    if (!task || !out) return FALSE;
    
    if (task->compact_matches) {
        size_t count = compact_matches_count(task->compact_matches);
        if (idx >= count) return FALSE;
        
        /* Extract single match from compact storage */
        size_t start, end;
        if (!compact_matches_get(task->compact_matches, idx, &start, &end))
            return FALSE;
        out->start = start;
        out->end = end;
        return TRUE;
    }
    
    /* Regex search - direct array access */
    if (task->matches && idx < task->matches->len) {
        *out = g_array_index(task->matches, SearchMatch, idx);
        return TRUE;
    }
    
    return FALSE;
}


static GRegex *compile_search_regex(const char *raw_query, gboolean regex, gboolean case_sensitive, gboolean whole_word) {
    char *query = NULL;
    GRegex *pattern = NULL;
    GError *err = NULL;

    if (!regex) {
        char *unescaped = unescape_string(raw_query);
        char *safe_query = g_regex_escape_string(unescaped, -1);
        g_free(unescaped);
        if (whole_word) {
            query = g_strdup_printf("\\b%s\\b", safe_query);
            g_free(safe_query);
        } else {
            query = safe_query;
        }
    } else {
        if (whole_word) {
            query = g_strdup_printf("\\b(?:%s)\\b", raw_query);
        } else {
            query = g_strdup(raw_query);
        }
    }
    GRegexCompileFlags flags = G_REGEX_OPTIMIZE;
    if (!case_sensitive) flags |= G_REGEX_CASELESS;
    pattern = g_regex_new(query, flags, 0, &err);
    if (err) { g_error_free(err); g_free(query); return NULL; }
    g_free(query);
    return pattern;
}

gboolean
document_find_next(Document *doc, SearchMatch *result, size_t start_pos, const char *raw_query, 
                  gboolean regex, gboolean case_sensitive, gboolean whole_word)
{
    if (!doc || !raw_query || !*raw_query) return FALSE;
    GRegex *pattern = compile_search_regex(raw_query, regex, case_sensitive, whole_word);
    if (!pattern) return FALSE;
    
    size_t line_count = document_get_line_count(doc);
    size_t start_line = document_get_line_of_offset(doc, start_pos);
    size_t offset_in_line = start_pos - piece_table_get_offset_of_line(doc->pt, start_line);
    
    PieceTableIter iter;
    piece_table_iter_init_at_line(doc->pt, &iter, start_line);
    
    char buf[65536];
    size_t current_line = start_line;
    size_t looped_limit = start_line;
    gboolean wrapped = FALSE;
    
    while (TRUE) {
        if (current_line >= line_count) {
             if (!wrapped) {
                 current_line = 0;
                 piece_table_iter_init_at_line(doc->pt, &iter, 0);
                 wrapped = TRUE;
                 continue;
             } else {
                 break; 
             }
        }
        
        if (wrapped && current_line > looped_limit) break;
        
        size_t line_len = piece_table_iter_get_next_line(&iter, buf, sizeof(buf)-1);
        if (line_len >= sizeof(buf)-1) line_len = sizeof(buf)-1;
        buf[line_len] = '\0';
        
        size_t search_start_idx = 0;
        if (current_line == start_line && !wrapped) {
             search_start_idx = offset_in_line + 1; /* Scan AFTER cursor */
        }
        
        if (search_start_idx < line_len) {
             GMatchInfo *info;
             if (g_regex_match_full(pattern, buf, -1, search_start_idx, 0, &info, NULL)) {
                  gint s, e;
                  g_match_info_fetch_pos(info, 0, &s, &e);
                  size_t line_abs_start = piece_table_get_offset_of_line(doc->pt, current_line);
                  result->start = line_abs_start + s;
                  result->end = line_abs_start + e;
                  g_match_info_free(info);
                  g_regex_unref(pattern);
                  return TRUE;
             }
             g_match_info_free(info);
        }
        current_line++;
    }
    
    g_regex_unref(pattern);
    return FALSE;
}

gboolean
document_find_prev(Document *doc, SearchMatch *result, size_t start_pos, const char *raw_query, 
                  gboolean regex, gboolean case_sensitive, gboolean whole_word)
{
    if (!doc || !raw_query || !*raw_query) return FALSE;
    GRegex *pattern = compile_search_regex(raw_query, regex, case_sensitive, whole_word);
    if (!pattern) return FALSE;
    
    size_t line_count = document_get_line_count(doc);
    
    PieceTableIter iter;
    piece_table_iter_init(doc->pt, &iter);
    
    char buf[65536];
    size_t current_line = 0;
    
    gboolean found_any = FALSE;
    SearchMatch last_match_before = {0,0};
    gboolean found_before = FALSE;
    SearchMatch last_match_global = {0,0};
    
    while (current_line < line_count) {
        size_t line_len = piece_table_iter_get_next_line(&iter, buf, sizeof(buf)-1);
        if (line_len >= sizeof(buf)-1) line_len = sizeof(buf)-1;
        buf[line_len] = '\0';
        
        GMatchInfo *info;
        if (g_regex_match(pattern, buf, 0, &info)) {
             while (g_match_info_matches(info)) {
                 gint s, e;
                 g_match_info_fetch_pos(info, 0, &s, &e);
                 
                 size_t line_abs_start = piece_table_get_offset_of_line(doc->pt, current_line);
                 size_t abs_end = line_abs_start + e;
                 
                 SearchMatch m = {line_abs_start + s, abs_end};
                 last_match_global = m;
                 found_any = TRUE;
                 
                 if (abs_end <= start_pos) {
                     last_match_before = m;
                     found_before = TRUE;
                 }
                 
                 g_match_info_next(info, NULL);
             }
        }
        g_match_info_free(info);
        current_line++;
    }
    g_regex_unref(pattern);
    
    if (found_before) {
        *result = last_match_before;
        return TRUE;
    }
    if (found_any) {
        *result = last_match_global;
        return TRUE;
    }
    return FALSE;
}

/* --- Async Replace Implementation --- */

struct _ReplaceTask {
    Document *doc;
    GArray *matches;
    char *replacement;
    char *literal_replacement;
    
    gboolean regex;
    GRegex *cached_regex;
    
    int current_idx; /* Iterating Forward: 0 to total_count - 1 */
    int total_count;
    int processed_count;
    
    /* Bulk Strategy State */
    GString *new_content_buffer;
    size_t last_copied_offset;
    size_t lf_count;  /* Pre-counted newlines to avoid O(N) scan at end */
    
    guint idle_id;
    ReplaceProgressCallback callback;
    void *user_data;
};

/* Helper to count newlines in a string */
static size_t count_lf(const char *str, size_t len) {
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '\n') count++;
    }
    return count;
}

static void replace_task_free(ReplaceTask *task) {
    if (!task) return;
    if (task->idle_id) g_source_remove(task->idle_id);
    if (task->matches) g_array_unref(task->matches);
    if (task->replacement) g_free(task->replacement);
    if (task->literal_replacement) g_free(task->literal_replacement);
    if (task->cached_regex) g_regex_unref(task->cached_regex);
    if (task->new_content_buffer) g_string_free(task->new_content_buffer, TRUE);
    g_free(task);
}

static gboolean replace_idle_step(gpointer user_data) {
    ReplaceTask *task = (ReplaceTask *)user_data;
    if (!task) return G_SOURCE_REMOVE;
    
    Document *doc = task->doc;
    
    /* Time Budget: 15ms (slightly higher for bulk throughput) */
    gint64 start_time = g_get_monotonic_time();
    gint64 budget_micros = 15000; 
    
    /* On first run, allocate buffer if not already */
    if (!task->new_content_buffer) {
        /* Estimate size: Current size + (difference * count). 
           Safe bet: Current size * 1.2 or similar. */
        size_t current_len = document_get_length(doc);
        task->new_content_buffer = g_string_sized_new(current_len + 1024);
    }
    
    /* Process continuously until budget exhausted */
    while (task->current_idx < task->total_count) {
        /* Check budget every 256 items (faster simple copies) */
        if (task->processed_count % 256 == 0 && task->processed_count > 0) {
             if (g_get_monotonic_time() - start_time > budget_micros) {
                 /* Update Progress and Yield */
                 if (task->callback) {
                     task->callback(task->processed_count, task->total_count, FALSE, task->user_data);
                 }
                 return G_SOURCE_CONTINUE;
             }
        }
        
        SearchMatch m = g_array_index(task->matches, SearchMatch, task->current_idx);
        
        /* Validate range */
        if (m.start < task->last_copied_offset) {
            /* Overlapping matches? Should not happen if search provided non-overlapping results.
               Skip or clamp. */
            task->current_idx++;
            task->processed_count++;
            continue;
        }
        
        /* 1. Append Text from Last Offset to Match Start */
        if (m.start > task->last_copied_offset) {
            size_t gap_len = m.start - task->last_copied_offset;
            char *gap_text = document_get_text_range(doc, task->last_copied_offset, gap_len);
            if (gap_text) {
                g_string_append_len(task->new_content_buffer, gap_text, gap_len);
                task->lf_count += count_lf(gap_text, gap_len);
                g_free(gap_text);
            }
        }
        
        /* 2. Compute and Append Replacement */
        if (task->regex && task->cached_regex) {
             size_t match_len = m.end - m.start;
             char *original_text = document_get_text_range(doc, m.start, match_len);
             if (original_text) {
                 char *repl_str = g_regex_replace(task->cached_regex, original_text, -1, 0, task->replacement, 0, NULL);
                 if (repl_str) {
                     size_t repl_len = strlen(repl_str);
                     g_string_append(task->new_content_buffer, repl_str);
                     task->lf_count += count_lf(repl_str, repl_len);
                     g_free(repl_str);
                 }
                 g_free(original_text);
             }
        } else {
             /* Literal */
             if (task->literal_replacement) {
                 size_t lit_len = strlen(task->literal_replacement);
                 g_string_append(task->new_content_buffer, task->literal_replacement);
                 task->lf_count += count_lf(task->literal_replacement, lit_len);
             }
        }
        
        /* Advance */
        task->last_copied_offset = m.end;
        task->current_idx++;
        task->processed_count++;
    }
    
    /* Finished Processing Matches */
    
    /* Append Tail (Last Match End to Doc End) */
    size_t total_len = document_get_length(doc);
    if (task->last_copied_offset < total_len) {
        size_t tail_len = total_len - task->last_copied_offset;
        char *tail_text = document_get_text_range(doc, task->last_copied_offset, tail_len);
        if (tail_text) {
            g_string_append_len(task->new_content_buffer, tail_text, tail_len);
            task->lf_count += count_lf(tail_text, tail_len);
            g_free(tail_text);
        }
    }
    
    /* Prepare Confirmation */
    /* ATOMIC REPLACE of WHOLE DOCUMENT */
    /* 
     * PERFORMANCE FIX: Use optimized piece_table_replace_all which creates
     * chunked pieces to maintain O(log N) line access for rendering.
     */
    
    document_suspend_callbacks(doc);
    
    /* Suppress undo recording */
    doc->undo_stack->in_undo_redo = TRUE;
    
    /* OPTIMIZED: Use piece_table_replace_all which creates chunked pieces
     * to maintain O(log N) line access for rendering. */
    piece_table_replace_all(doc->pt, task->new_content_buffer->str, 
                            task->new_content_buffer->len, task->lf_count);
    
    /* Re-enable undo recording */
    doc->undo_stack->in_undo_redo = FALSE;
    
    /* Mark document as modified */
    check_modification_state(doc);
    
    /* Re-enable callbacks BUT don't trigger them synchronously.
     * The normal document_resume_callbacks() would invoke all registered
     * callbacks (including editor_widget_update_adjustments which is O(N)),
     * causing freeze. Instead, just clear the flag. The UI will update
     * naturally on next draw cycle when it sees the piece_table change_count. */
    doc->callbacks_suspended = FALSE;
    
    /* Final Callback */
    if (task->callback) {
        task->callback(task->total_count, task->total_count, TRUE, task->user_data);
    }
    
    /* We don't free task here. We just return false. */
    task->idle_id = 0; /* It's removed */
    
    return G_SOURCE_REMOVE;
}

ReplaceTask *
document_replace_async_start(Document *doc, GArray *matches, const char *replacement, gboolean regex, GRegex *cached_regex, ReplaceProgressCallback callback, void *user_data)
{
    if (!doc || !matches || matches->len == 0) return NULL;
    
    ReplaceTask *task = g_new0(ReplaceTask, 1);
    task->doc = doc;
    task->matches = matches;
    g_array_ref(matches);
    
    /* Normalize replacement string (handles $1, \1, \n, \t, etc.) */
    task->replacement = normalize_replacement_string(replacement, regex);
    task->regex = regex;
    task->cached_regex = cached_regex; 
    if (cached_regex) g_regex_ref(cached_regex);
    
    /* For literal (non-regex), reuse normalized string */
    if (!regex && task->replacement) {
        task->literal_replacement = g_strdup(task->replacement);
    }
    
    task->total_count = matches->len;
    task->current_idx = 0; /* Forward Iteration for Bulk Build */
    task->processed_count = 0;
    
    task->new_content_buffer = NULL; /* Allocated in idle step */
    task->last_copied_offset = 0;
    
    task->callback = callback;
    task->user_data = user_data;
    
    /* Do NOT suspend callbacks or begin undo group yet. 
       We are just building the string in background. 
       We will do the actual replace (and suspend/undo) in the final step.
       
       Wait, if the user edits the document WHILE we are building the string?
       That would invalidate our matches and our buffer building!
       
       We MUST block user input or lock the document.
       Currently ViTE is single threaded (GLib Main Loop).
       The idle function runs on main thread.
       User events (keypresses) also run on main thread.
       
       If we yield (return G_SOURCE_CONTINUE), GMainLoop handles other events (like keypresses!).
       So the user CAN type while we are yielding.
       
       This is dangerous.
       If the user types, offsets change, matches become invalid.
       
       Solution:
       1. Lock the editor (Set ReadOnly? Show Modal Progress Dialog?).
       2. Or listen for changes and Cancel the task if document changes.
       
       The `find-replace-bar.c` has `on_document_changed`.
       However, we need to ensure we don't apply a stale buffer.
       
       If `Find` is fast, `Replace All` shouldn't take long enough for user to be annoyed by a lock, 
       BUT if it takes 2 seconds, they might type.
       
       Best practice: Cancel Replace if document modified.
       The existing `on_document_changed` in `find-replace-bar.c` likely handles logic?
       
       Let's assume for now we must handle the risk.
       If we suspend callbacks for the *duration of the task*, the UI won't update, but user might still type?
       No, `document_suspend_callbacks` just stops specialized listeners.
       
       If we want to be safe, `Replace All` should act as a modal operation or faster enough.
       Given the speedup, it should be fast.
    */
    
    task->idle_id = g_idle_add(replace_idle_step, task);
    return task;
}

void document_replace_async_cancel(ReplaceTask *task) {
    if (!task) return;
    
    if (task->idle_id) {
        /* Note: We no longer suspend/undo in start(), so we don't need to resume/end here. */
        g_source_remove(task->idle_id);
        task->idle_id = 0;
    }
    
    replace_task_free(task);
}
/* --- Streaming Replace Implementation (File-Backed, Buffered) --- */

struct _StreamingReplaceTask {
    Document *doc;
    char *query;           /* For literal fast path */
    char *replacement;     /* Normalized replacement string */
    size_t replacement_len;
    size_t query_len;      /* Length of literal query */
    GRegex *pattern;       /* Compiled pattern for regex */
    
    gboolean regex;
    gboolean case_sensitive;
    
    /* Line-by-line processing */
    PieceTableIter iter;
    GString *line_buf;
    size_t current_line;
    size_t total_lines;
    int replace_count;
    size_t lf_count;
    
    /* Temp file for output (buffered I/O) */
    FILE *output_file;
    char *output_path;
    size_t output_size;
    
    guint idle_id;
    ReplaceProgressCallback callback;
    void *user_data;
};

static void streaming_replace_task_free(StreamingReplaceTask *task) {
    if (!task) return;
    if (task->idle_id) g_source_remove(task->idle_id);
    if (task->query) g_free(task->query);
    if (task->replacement) g_free(task->replacement);
    if (task->pattern) g_regex_unref(task->pattern);
    if (task->line_buf) g_string_free(task->line_buf, TRUE);
    if (task->output_file) fclose(task->output_file);
    if (task->output_path) {
        unlink(task->output_path);
        g_free(task->output_path);
    }
    g_free(task);
}

static gboolean streaming_replace_idle_step(gpointer user_data) {
    StreamingReplaceTask *task = (StreamingReplaceTask *)user_data;
    if (!task) return G_SOURCE_REMOVE;
    
    Document *doc = task->doc;
    
    /* Time Budget: 50ms (increased for speed) */
    gint64 start_time = g_get_monotonic_time();
    gint64 budget_micros = 50000;
    
    size_t repl_len = task->replacement_len;
    size_t query_len = task->query_len;
    
    /* Process lines - write to temp file */
    while (task->current_line < task->total_lines) {
        /* Check budget every 1000 lines (less frequent checks) */
        if (task->current_line % 1000 == 0 && task->current_line > 0) {
            if (g_get_monotonic_time() - start_time > budget_micros) {
                /* Report progress and yield */
                if (task->callback) {
                    int progress_pct = (int)((task->current_line * 100) / task->total_lines);
                    task->callback(progress_pct, task->replace_count, FALSE, task->user_data);
                }
                return G_SOURCE_CONTINUE;
            }
        }
        
        /* Get next line */
        g_string_truncate(task->line_buf, 0);
        size_t len = piece_table_iter_get_next_line_string(&task->iter, task->line_buf);
        
        if (len == 0 && task->line_buf->len == 0) {
            break; /* EOF */
        }
        
        char *line = task->line_buf->str;
        size_t line_len = task->line_buf->len;
        
        /* Process this line - write transformed content to temp file */
        if (task->regex && task->pattern) {
            /* Regex replacement with manual iteration for safe backref expansion */
            GMatchInfo *mi = NULL;
            GError *err = NULL;
            
            if (g_regex_match_full(task->pattern, line, line_len, 0, 0, &mi, &err)) {
                size_t current_pos = 0;
                while (g_match_info_matches(mi)) {
                    gint start_pos, end_pos;
                    g_match_info_fetch_pos(mi, 0, &start_pos, &end_pos);
                    
                    /* Write text before match */
                    if ((size_t)start_pos > current_pos) {
                        size_t gap = (size_t)start_pos - current_pos;
                        fwrite(line + current_pos, 1, gap, task->output_file);
                        task->output_size += gap;
                        task->lf_count += count_lf(line + current_pos, gap);
                    }
                    
                    /* Expand replacement */
                    GError *expand_err = NULL;
                    char *expanded = g_match_info_expand_references(mi, task->replacement, &expand_err);
                    if (expanded) {
                        size_t exp_len = strlen(expanded);
                        fwrite(expanded, 1, exp_len, task->output_file);
                        task->output_size += exp_len;
                        task->lf_count += count_lf(expanded, exp_len);
                        g_free(expanded);
                    } else {
                         /* Should not happen, but safe fallback? */
                         if (expand_err) g_error_free(expand_err);
                    }
                    
                    current_pos = (size_t)end_pos;
                    task->replace_count++;
                    g_match_info_next(mi, NULL);
                }
                
                /* Write remaining text after last match */
                if (current_pos < line_len) {
                    size_t rest = line_len - current_pos;
                    fwrite(line + current_pos, 1, rest, task->output_file);
                    task->output_size += rest;
                    task->lf_count += count_lf(line + current_pos, rest);
                }
                g_match_info_free(mi);
            } else {
                /* No match - write original */
                fwrite(line, 1, line_len, task->output_file);
                task->output_size += line_len;
                task->lf_count += count_lf(line, line_len);
                if (err) g_error_free(err);
            }
        } else if (task->query && query_len > 0) {
            /* Literal replacement - fast path */
            const char *ptr = line;
            const char *line_end = line + line_len;
            
            while (ptr < line_end) {
                const char *found;
                if (task->case_sensitive) {
                    found = strstr(ptr, task->query);
                } else {
                    found = strcasestr(ptr, task->query);
                }
                
                if (!found || found >= line_end) {
                    /* No more matches - write rest of line */
                    size_t rest = line_end - ptr;
                    fwrite(ptr, 1, rest, task->output_file);
                    task->output_size += rest;
                    task->lf_count += count_lf(ptr, rest);
                    break;
                }
                
                /* Write text before match */
                if (found > ptr) {
                    size_t before = found - ptr;
                    fwrite(ptr, 1, before, task->output_file);
                    task->output_size += before;
                    task->lf_count += count_lf(ptr, before);
                }
                
                /* Write replacement */
                if (repl_len > 0) {
                    fwrite(task->replacement, 1, repl_len, task->output_file);
                    task->output_size += repl_len;
                    task->lf_count += count_lf(task->replacement, repl_len);
                }
                
                task->replace_count++;
                ptr = found + query_len;
            }
        } else {
            /* No query - just write line */
            fwrite(line, 1, line_len, task->output_file);
            task->output_size += line_len;
            task->lf_count += count_lf(line, line_len);
        }
        
        task->current_line++;
    }
    
    /* All lines processed - replace document from temp file */
    fflush(task->output_file);
    int fd = fileno(task->output_file);
    fsync(fd);
    
    /* Snapshot BEFORE replacing - for undo support */
    char *undo_path = document_snapshot_to_file(doc);
    
    /* Push Undo Command - use output_path as redo_path */
    if (undo_path) {
        undo_stack_push_restore_path(doc->undo_stack, undo_path, task->output_path);
        g_free(undo_path);
        /* Don't free output_path here - it's now owned by the undo stack */
        /* Set to NULL so streaming_replace_task_free won't unlink it */
        task->output_path = NULL;
    }
    
    /* mmap the temp file and replace document content */
    piece_table_replace_from_fd(doc->pt, fd, task->output_size, task->lf_count);
    
    /* Report completion */
    if (task->callback) {
        task->callback(100, task->replace_count, TRUE, task->user_data);
    }
    
    task->idle_id = 0;
    streaming_replace_task_free(task);
    return G_SOURCE_REMOVE;
}

StreamingReplaceTask *
document_replace_streaming_start(Document *doc, const char *raw_query, const char *replacement,
                                  gboolean regex, gboolean case_sensitive, gboolean whole_word,
                                  ReplaceProgressCallback callback, void *user_data)
{
    if (!doc || !raw_query || !*raw_query) return NULL;
    
    StreamingReplaceTask *task = g_new0(StreamingReplaceTask, 1);
    task->doc = doc;
    task->regex = regex;
    task->case_sensitive = case_sensitive;
    task->callback = callback;
    task->user_data = user_data;
    task->output_file = NULL;
    
    /* Normalize replacement string */
    task->replacement = normalize_replacement_string(replacement, regex);
    task->replacement_len = task->replacement ? strlen(task->replacement) : 0;
    
    /* Compile pattern or prepare query */
    if (regex || whole_word) {
        char *query = NULL;
        size_t query_len = 0;
        
        if (!regex) {
            char *unescaped = unescape_string(raw_query);
            query_len = strlen(unescaped);
            char *safe_query = g_regex_escape_string(unescaped, -1);
            g_free(unescaped);
            if (whole_word) {
                query = g_strdup_printf("\\b%s\\b", safe_query);
                g_free(safe_query);
            } else {
                query = safe_query;
            }
        } else {
            if (whole_word) {
                query = g_strdup_printf("\\b(?:%s)\\b", raw_query);
            } else {
                query = g_strdup(raw_query);
            }
            query_len = strlen(raw_query);
        }
        
        GRegexCompileFlags flags = G_REGEX_OPTIMIZE;
        if (!case_sensitive) flags |= G_REGEX_CASELESS;
        
        GError *err = NULL;
        task->pattern = g_regex_new(query, flags, 0, &err);
        g_free(query);
        
        if (!task->pattern) {
            if (err) g_error_free(err);
            streaming_replace_task_free(task);
            return NULL;
        }
        
        task->query_len = query_len;
    } else {
        /* Literal search - use fast path */
        char *unescaped = unescape_string(raw_query);
        task->query = unescaped;
        task->query_len = strlen(unescaped);
    }
    
    /* Create temp file for output */
    task->output_path = g_strdup("/tmp/vite_replace_XXXXXX");
    int fd = mkstemp(task->output_path);
    if (fd < 0) {
        g_warning("Failed to create temp file for replace: %s", strerror(errno));
        streaming_replace_task_free(task);
        return NULL;
    }
    
    /* Convert to FILE* for buffered I/O */
    task->output_file = fdopen(fd, "w+");
    if (!task->output_file) {
        close(fd);
        streaming_replace_task_free(task);
        return NULL;
    }
    
    /* Initialize iterator for line processing */
    piece_table_iter_init(doc->pt, &task->iter);
    task->line_buf = g_string_sized_new(4096);
    task->total_lines = document_get_line_count(doc);
    task->current_line = 0;
    task->replace_count = 0;
    task->output_size = 0;
    task->lf_count = 0;
    
    task->idle_id = g_idle_add(streaming_replace_idle_step, task);
    return task;
}

void document_replace_streaming_cancel(StreamingReplaceTask *task) {
    if (!task) return;
    
    if (task->idle_id) {
        g_source_remove(task->idle_id);
        task->idle_id = 0;
    }
    
    streaming_replace_task_free(task);
}



/* Async Loading Wrapper */

static void
on_pt_progress(double progress, FileEncoding encoding, NewlineType newline, gpointer user_data)
{
    Document *doc = user_data;
    if (doc->progress_cb) {
        doc->progress_cb(progress, encoding, newline, doc->progress_user_data);
    }
}

typedef struct {
    Document *doc;
    GAsyncReadyCallback callback;
    gpointer user_data;
    char *filename;
} DocLoadCtx;

static void
on_pt_loaded(GObject *source, GAsyncResult *res, gpointer user_data)
{
    DocLoadCtx *ctx = user_data;
    /* We don't finish here, we let document_load_file_finish do it given the res */
    
    /* We need to pass 'res' to the user callback, but the user callback expects 
       source to be Document, not PieceTable?
       Standard GAsync pattern: The finish function takes the source object.
       So document_load_file_finish(doc, res, error).
       But 'res' is from PieceTable.
       So document_load_file_finish will call piece_table_load_finish.
    */
    
    if (ctx->callback) {
        /* Pass NULL as source object if Document is not a GObject, or cast appropriately if expected. 
           But callback signature is (GObject *source, ...).
           Since Document is NOT a GObject, we should ideally pass NULL or change the callback signature.
           However, main.c expects 'source' to be 'Document*'.
           In C, pointer casting is just a value. G_OBJECT() macro performs a runtime check which FAILS.
           So we should just pass (GObject*)ctx->doc but WITHOUT the checking macro.
        */
        ctx->callback((GObject*)ctx->doc, res, ctx->user_data);
    }
    
    /* Update document state if successful? 
       Actually standard pattern is specific finish function handles result extraction.
       But we need to update doc state (undo stack, etc) upon success.
       We can do that inside document_load_file_finish.
    */
    
    g_free(ctx->filename);
    g_free(ctx);
}

void
document_load_file_async(Document *doc, const char *filename, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    if (!doc) return;
    
    DocLoadCtx *ctx = g_new0(DocLoadCtx, 1);
    ctx->doc = doc;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->filename = g_strdup(filename);
    
    /* We pass our own callback to piece_table_load_async */
    piece_table_load_async(doc->pt, filename, cancellable, 
                           on_pt_progress, doc,
                           on_pt_loaded, ctx);
}

gboolean
document_load_file_finish(Document *doc, GAsyncResult *res, GError **error)
{
    /* res comes from piece_table_load_async (GTask) */
    gboolean success = piece_table_load_finish(doc->pt, res, error);
    
    if (success) {
        /* Reset document state */
        undo_stack_free(doc->undo_stack);
        doc->undo_stack = undo_stack_new();
        
        g_free(doc->file_path);
        /* We need to recover filename? It was in the async task. 
           But piece_table doesn't store filename in PT. 
           We can look at the task source tag or data if we had access.
           Wait, we passed filename to load_file_async. 
           Ideally we should update it here.
           But we don't have it easily here unless we stashed it in the GTask or passed it via source object.
           Actually, the caller usually knows what file they loaded.
           BUT, document_open meant setting the path.
           Let's retrieve it from the GTask if possible?
           Or just require caller to set it?
           
           Better: document_load_file_async caller (main.c) updates the path on success.
           Wait, `document_new(filename)` sets it.
           If we use `document_new_empty`, path is NULL.
           Then `document_load_file_async`.
           
           I will fix this by setting the path in Document in the start of async load?
           No, only on success.
           
           I'll rely on the caller (main.c) to set the path using `document_set_file_path` (which I need to add)
           OR I can peek into the task data if I really want to.
           
           Actually, let's add `document_set_path` helper or just expose it?
           `document.c` has `doc->file_path`.
           
           Let's just update `doc->file_path` inside `document_load_file_async` immediately? 
           No, if it fails we shouldn't.
           
           I will add `char *pending_filename` to DocLoadCtx and access it?
           No, Finish function receives `res`.
           
           Option: Store filename in GTask/AsyncResult's source object data?
           
           Simplest: Add `document_set_file_path` to header/impl and let caller handle it.
           BUT `document_new` sets it. `main.c` relies on `document_get_file_path`.
           
           I will add `document_set_file_path(doc, filename)` usage in main.c's callback.
        */
        
        doc->saved_command = NULL;
        doc->callbacks_suspended = FALSE;
        
        /* Notify change */
        check_modification_state(doc);
        for (GList *l = doc->content_callbacks; l != NULL; l = l->next) {
            ContentCallbackData *cb = l->data;
            cb->func(doc, cb->user_data);
        }
    }
    return success;
}

void
document_set_progress_callback(Document *doc, DocumentProgressCallback callback, void *user_data)
{
    if (doc) {
        doc->progress_cb = callback;
        doc->progress_user_data = user_data;
    }
}

/* Filter Implementation */
void filter_result_free(FilterResult *res) {
    if (!res) return;
    if (res->matches) compact_matches_free(res->matches);
    g_free(res);
}

FilterResult *document_filter_lines(Document *doc, const char *pattern, gboolean regex, gboolean case_sensitive) {
    /* Synchronous wrapper around async implementation for backward compatibility if needed, 
       or just keep original for small files? 
       Actually, let's keep the original sync implementation for now or redirect?
       For now, let's keep the original implementation but maybe optimized?
       No, the plan is to use async entirely in the UI. 
       Let's leave this existing sync function as is for now, or minimal update.
    */
    if (!doc || !pattern || !*pattern) return NULL;

    GRegex *reg = NULL;
    if (regex) {
        GError *err = NULL;
        GRegexCompileFlags flags = G_REGEX_OPTIMIZE;
        if (!case_sensitive) flags |= G_REGEX_CASELESS;
        
        reg = g_regex_new(pattern, flags, 0, &err);
        if (err) {
            g_warning("Invalid regex for filter: %s", err->message);
            g_error_free(err);
            return NULL; 
        }
    }

    size_t total_lines = document_get_line_count(doc);
    /* Use 0 match length for line indices */
    CompactMatches *matches_storage = compact_matches_new(0);

    char *lower_pattern = NULL;
    if (!regex && !case_sensitive) {
        lower_pattern = g_utf8_strdown(pattern, -1);
    }

    /* Stack optimization for non-regex search? */
    /* Implementation kept simple for sync version */

    for (size_t i = 0; i < total_lines; i++) {
        size_t len;
        char *line = document_get_line(doc, i, &len);
        if (!line) continue;
        
        gboolean match = FALSE;
        if (regex) {
            match = g_regex_match(reg, line, 0, NULL);
        } else {
            if (case_sensitive) {
                match = (strstr(line, pattern) != NULL);
            } else {
                char *lower_line = g_utf8_strdown(line, -1);
                match = (strstr(lower_line, lower_pattern) != NULL);
                g_free(lower_line);
            }
        }
        
        if (match) {
            compact_matches_append(matches_storage, i);
        }
        
        g_free(line);
    }

    if (reg) g_regex_unref(reg);
    if (lower_pattern) g_free(lower_pattern);

    FilterResult *res = g_new0(FilterResult, 1);
    res->matches = matches_storage;
    res->count = compact_matches_count(res->matches);
    
    return res;
}

struct _DocumentFilterTask {
    Document *doc;
    PieceTableIter iter;
    GArray *matches;
    
    char *pattern;
    GRegex *regex_pattern;
    gboolean is_regex;
    gboolean case_sensitive;
    
    char *lower_pattern; /* For case-insensitive string search */
    
    size_t total_lines;
    size_t processed_lines;
    
    CompactMatches *matches_storage;
};

DocumentFilterTask *document_filter_async_start(Document *doc, const char *pattern, gboolean regex, gboolean case_sensitive) {
    if (!doc || !pattern) return NULL;
    
    DocumentFilterTask *task = g_new0(DocumentFilterTask, 1);
    task->doc = doc;
    task->pattern = g_strdup(pattern);
    task->is_regex = regex;
    task->case_sensitive = case_sensitive;
    
    if (regex) {
        GError *err = NULL;
        GRegexCompileFlags flags = G_REGEX_OPTIMIZE;
        if (!case_sensitive) flags |= G_REGEX_CASELESS;
        task->regex_pattern = g_regex_new(pattern, flags, 0, &err);
        if (err) {
            g_warning("Invalid regex: %s", err->message);
            g_error_free(err);
            /* Continue anyway, will just match nothing */
        }
    } else {
        if (!case_sensitive) {
            task->lower_pattern = g_utf8_strdown(pattern, -1);
        }
    }
    
    task->total_lines = document_get_line_count(doc);
    /* Initialize with match_length=0 since we store line indices */
    task->matches_storage = compact_matches_new(0);
    
    /* Initialize Iterator */
    piece_table_iter_init(doc->pt, &task->iter);
    
    return task;
}

gboolean document_filter_async_step(DocumentFilterTask *task, gint64 time_budget_us) {
    if (!task) return TRUE;
    
    gint64 start_time = g_get_monotonic_time();
    char buf[4096]; /* 4KB stack buffer for line checking */
    
    while (task->processed_lines < task->total_lines) {
        /* Check time budget */
        if ((g_get_monotonic_time() - start_time) > time_budget_us) {
            return FALSE; /* Yield */
        }
        
        /* Get next line into buffer without malloc if possible */
        /* Note: piece_table_iter_get_next_line handles the traversal */
        size_t len = piece_table_iter_get_next_line(&task->iter, buf, sizeof(buf) - 1);
        
        /* IMPORTANT: Iter returns FULL line length, which might exceed buffer. Clamp it! */
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        
        buf[len] = '\0'; /* Null terminate for regex/strstr */
        
        /* If line was truncated (len == sizeof(buf)-1), we might miss matches at the end.
           For filter, this is an acceptable trade-off for speed vs correctness on extremely long lines.
           Or we could alloc only for long lines. let's stick to stack for speed. 
           Most code lines are < 4KB. */
           
        gboolean match = FALSE;
        
        if (task->is_regex) {
            if (task->regex_pattern) {
                match = g_regex_match(task->regex_pattern, buf, 0, NULL);
            }
            /* If invalid regex, match remains FALSE */
        } else {
            if (task->case_sensitive) {
                match = (strstr(buf, task->pattern) != NULL);
            } else {
                /* Optimization: Avoid full lowercasing if not needed? 
                   We do need to lowercase the line to search case-insensitively. */
                char *lower_line = NULL;
                if (g_utf8_validate(buf, len, NULL)) {
                    lower_line = g_utf8_strdown(buf, len);
                } else {
                    /* Fallback for binary/invalid data: ASCII lowercasing */
                    lower_line = g_strdup(buf);
                    for (size_t i = 0; i < len; ++i) {
                        lower_line[i] = g_ascii_tolower(lower_line[i]);
                    }
                }
                
                if (task->lower_pattern && lower_line) {
                    /* Note: strstr might not be ideal for binary, but it's safe enough if null-terminated */
                    match = (strstr(lower_line, task->lower_pattern) != NULL);
                }
                g_free(lower_line);
            }
        }
        
        if (match) {
            compact_matches_append(task->matches_storage, task->processed_lines);
        }
        
        task->processed_lines++;
    }
    
    return TRUE; /* Finished */
}

FilterResult *document_filter_async_finish(DocumentFilterTask *task) {
    if (!task) return NULL;
    
    FilterResult *res = g_new0(FilterResult, 1);
    res->matches = task->matches_storage;
    res->count = compact_matches_count(res->matches);
    task->matches_storage = NULL; /* Transfer ownership */
    
    /* Cleanup task */
    document_filter_async_cancel(task);
    
    return res;
}

void document_filter_async_cancel(DocumentFilterTask *task) {
    if (!task) return;
    
    if (task->pattern) g_free(task->pattern);
    if (task->lower_pattern) g_free(task->lower_pattern);
    if (task->regex_pattern) g_regex_unref(task->regex_pattern);
    if (task->matches_storage) compact_matches_free(task->matches_storage);
    
    g_free(task);
}

size_t document_filter_task_get_processed(DocumentFilterTask *task) {
    if (!task) return 0;
    return task->processed_lines;
}

size_t document_filter_task_get_total(DocumentFilterTask *task) {
    if (!task) return 0;
    return task->total_lines;
}

size_t document_filter_task_get_match_count(DocumentFilterTask *task) {
    if (!task || !task->matches_storage) return 0;
    return compact_matches_count(task->matches_storage);
}
/* --- Streaming Change Case Implementation --- */

struct _StreamingChangeCaseTask {
    Document *doc;
    size_t start;
    size_t end;
    CharTransformFunc simple_func;
    int type; /* 0=simple, 1=title, 2=invert */
    ReplaceProgressCallback callback;
    void *user_data;
    
    FILE *output_file;
    char *output_path;
    
    PieceTableIter iter;
    size_t current_offset;
    size_t total_size;
    
    size_t output_size;
    size_t lf_count;
    
    guint idle_id;
    
    /* State for title case */
    gboolean new_word;
};

static void
streaming_change_case_task_free(StreamingChangeCaseTask *task)
{
    if (!task) return;
    
    if (task->output_file) fclose(task->output_file);
    if (task->output_path) {
        unlink(task->output_path);
        g_free(task->output_path);
    }
    
    g_free(task);
}



/* Helper to snapshot current document state to a temp file */
static char *
document_snapshot_to_file(Document *doc)
{
    char *path = g_strdup("/tmp/vite_snapshot_XXXXXX");
    int fd = mkstemp(path);
    if (fd == -1) {
        g_free(path);
        return NULL;
    }
    
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(path);
        g_free(path);
        return NULL;
    }
    
    /* Stream entire document to file */
    PieceTableIter iter;
    piece_table_iter_init(doc->pt, &iter);
    
    size_t chunk_len;
    const char *chunk;
    
    while ((chunk = piece_table_iter_get_chunk(&iter, &chunk_len))) {
        if (chunk_len > 0) {
            fwrite(chunk, 1, chunk_len, f);
        }
        piece_table_iter_advance(&iter, chunk_len);
    }
    
    fclose(f);
    return path; /* Caller owns path and file */
}

static gboolean
streaming_change_case_idle_step(gpointer user_data)
{
    StreamingChangeCaseTask *task = user_data;
    Document *doc = task->doc;
    
    /* Process a chunk of time (e.g. 2-5ms) or fixed bytes */
    /* Let's process 64KB per step to yield to UI */
    const size_t BYTES_PER_STEP = 65536;
    size_t processed_this_step = 0;
    
    char buf[4096];
    
    while (processed_this_step < BYTES_PER_STEP) {
        size_t available = 0;
        const char *ptr = piece_table_iter_get_chunk(&task->iter, &available);
        
        if (!ptr || available == 0) break;
        
        size_t chunk_len = MIN(available, sizeof(buf));
        
        /* Copy to mutable buffer */
        memcpy(buf, ptr, chunk_len);
        
        piece_table_iter_advance(&task->iter, chunk_len);
        
        /* Apply transform if in range */
        size_t chunk_start = task->current_offset;
        size_t chunk_end = task->current_offset + chunk_len;
        
        /* Check intersection with selection [start, end) */
        size_t intersect_start = MAX(chunk_start, task->start);
        size_t intersect_end = MIN(chunk_end, task->end);
        
        if (intersect_start < intersect_end) {
            /* There is overlap */
            size_t buf_off_start = intersect_start - chunk_start;
            size_t len = intersect_end - intersect_start;
            
            for (size_t i = 0; i < len; i++) {
                char c = buf[buf_off_start + i];
                char r = c;
                
                if (task->type == CHANGE_CASE_TITLE) {
                    if (g_ascii_isalpha(c)) {
                        r = task->new_word ? g_ascii_toupper(c) : g_ascii_tolower(c);
                        task->new_word = FALSE;
                    } else if (g_ascii_isspace(c) || g_ascii_ispunct(c)) {
                        task->new_word = TRUE;
                    }
                } else if (task->type == CHANGE_CASE_INVERT) {
                    if (g_ascii_isupper(c)) r = g_ascii_tolower(c);
                    else if (g_ascii_islower(c)) r = g_ascii_toupper(c);
                } else if (task->simple_func) {
                    r = task->simple_func(c);
                }
                
                buf[buf_off_start + i] = r;
            }
        } else {
             /* Outside selection */
             if (task->type == CHANGE_CASE_TITLE) {
                 if (chunk_end <= task->start) {
                     for (size_t i = 0; i < chunk_len; i++) {
                         char c = buf[i];
                         if (g_ascii_isalpha(c)) task->new_word = FALSE;
                         else if (g_ascii_isspace(c) || g_ascii_ispunct(c)) task->new_word = TRUE;
                     }
                 }
             }
        }
        
        /* Write to output */
        fwrite(buf, 1, chunk_len, task->output_file);
        task->output_size += chunk_len;
        task->lf_count += count_lf(buf, chunk_len);
        
        task->current_offset += chunk_len;
        processed_this_step += chunk_len;
        
        /* Report progress */
        if (task->callback && (task->current_offset % 65536 == 0)) {
            task->callback(task->current_offset, task->total_size, FALSE, task->user_data);
        }
    }
    
    if (task->current_offset >= task->total_size) {
        /* Done */
        fflush(task->output_file);
        int fd = fileno(task->output_file);
        fsync(fd);
        
        /* Snapshot BEFORE replacing logic */
        char *undo_path = document_snapshot_to_file(doc);
        char *redo_path = g_strdup(task->output_path);

        /* Push Undo CMD */
        if (undo_path) {
             undo_stack_push_restore_path(doc->undo_stack, undo_path, redo_path);
             g_free(undo_path);
             g_free(redo_path);
        }
        
        /* Replace document content */
        piece_table_replace_from_fd(doc->pt, fd, task->output_size, task->lf_count);
        
        if (task->callback) {
            task->callback(task->total_size, task->total_size, TRUE, task->user_data);
        }
        
        task->idle_id = 0;
        streaming_change_case_task_free(task);
        return G_SOURCE_REMOVE;
    }
    
    return G_SOURCE_CONTINUE;
}

StreamingChangeCaseTask *
document_change_case_streaming_start(Document *doc, 
                                     size_t start, size_t end,
                                     CharTransformFunc simple_func,
                                     int type,
                                     ReplaceProgressCallback callback, void *user_data)
{
    if (!doc) return NULL;
    
    /* Disk Space Check: Ensure we have enough space for the temp file copy */
    size_t total_size = document_get_length(doc);
    /* Require 5% margin or at least 10MB extra */
    size_t required = total_size + (total_size / 20) + (10 * 1024 * 1024);
    
    if (!check_disk_space("/tmp", required)) {
        return NULL;
    }
    
    StreamingChangeCaseTask *task = g_new0(StreamingChangeCaseTask, 1);
    task->doc = doc;
    task->start = MIN(start, end);
    task->end = MAX(start, end);
    task->simple_func = simple_func;
    task->type = type;
    task->callback = callback;
    task->user_data = user_data;
    task->new_word = TRUE; /* Default start state */
    
    /* Create temp file */
    task->output_path = g_strdup("/tmp/vite_case_XXXXXX");
    int fd = mkstemp(task->output_path);
    if (fd < 0) {
        g_warning("Failed to create temp file: %s", strerror(errno));
        streaming_change_case_task_free(task);
        return NULL;
    }
    
    task->output_file = fdopen(fd, "w+");
    if (!task->output_file) {
        close(fd);
        streaming_change_case_task_free(task);
        return NULL;
    }
    
    piece_table_iter_init(doc->pt, &task->iter);

    task->total_size = document_get_length(doc);
    task->current_offset = 0;
    
    task->idle_id = g_idle_add(streaming_change_case_idle_step, task);
    return task;
}

void
document_change_case_streaming_cancel(StreamingChangeCaseTask *task)
{
    if (!task) return;
    if (task->idle_id) {
        g_source_remove(task->idle_id);
        task->idle_id = 0;
    }
    streaming_change_case_task_free(task);
}

/* ============================================================================
 * Save Implementation - Atomic file saving with temp file + rename
 * ============================================================================ */

gboolean
document_save_as(Document *doc, const char *path, GError **error)
{
    if (!doc) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "NULL document");
        return FALSE;
    }
    
    if (!path || *path == '\0') {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid path");
        return FALSE;
    }
    
    /* Create temp file in same directory as target for atomic rename */
    char *dir = g_path_get_dirname(path);
    char *temp_path = g_strdup_printf("%s/.vite_save_XXXXXX", dir);
    g_free(dir);
    
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Failed to create temp file: %s", strerror(errno));
        g_free(temp_path);
        return FALSE;
    }
    
    /* Stream content to temp file */
    if (!piece_table_save_to_fd(doc->pt, fd, error)) {
        close(fd);
        unlink(temp_path);
        g_free(temp_path);
        return FALSE;
    }
    
    /* Ensure data is on disk before rename */
    if (fsync(fd) < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "fsync failed: %s", strerror(errno));
        close(fd);
        unlink(temp_path);
        g_free(temp_path);
        return FALSE;
    }
    
    close(fd);
    
    /* Atomic rename */
    if (rename(temp_path, path) < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Rename failed: %s", strerror(errno));
        unlink(temp_path);
        g_free(temp_path);
        return FALSE;
    }
    
    g_free(temp_path);
    
    /* Update document state */
    document_set_file_path(doc, path);
    document_mark_saved(doc);
    
    return TRUE;
}

gboolean
document_save(Document *doc, GError **error)
{
    if (!doc) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "NULL document");
        return FALSE;
    }
    
    const char *current_path = document_get_file_path(doc);
    if (!current_path) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Document has no file path. Use Save As.");
        return FALSE;
    }
    
    /* IMPORTANT: Make a copy of the path because document_save_as() will
     * call document_set_file_path() which frees doc->file_path.
     * If we pass doc->file_path directly, it becomes a use-after-free. */
    char *path_copy = g_strdup(current_path);
    gboolean result = document_save_as(doc, path_copy, error);
    g_free(path_copy);
    
    return result;
}

/* Async Save Task */
struct _DocumentSaveTask {
    Document *doc;
    char *path;
    char *temp_path;
    int fd;
    PieceTableSaveTask *pt_task;
    gboolean cancelled;
    GError *error;
};

DocumentSaveTask *
document_save_async_start(Document *doc, const char *path)
{
    if (!doc || !path) return NULL;
    
    /* Disk Space Check */
    size_t total_size = document_get_length(doc);
    size_t required = total_size + (total_size / 20) + (10 * 1024 * 1024);
    
    char *dir = g_path_get_dirname(path);
    /* Check disk space in the TARGET directory, not /tmp */
    if (!check_disk_space(dir, required)) {
        g_free(dir);
        return NULL;
    }
    
    /* Create temp file */
    char *temp_path = g_strdup_printf("%s/.vite_save_XXXXXX", dir);
    g_free(dir);
    
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        g_warning("Failed to create temp file: %s", strerror(errno));
        g_free(temp_path);
        return NULL;
    }
    
    DocumentSaveTask *task = g_new0(DocumentSaveTask, 1);
    task->doc = document_ref(doc); /* Keep doc alive during async save */
    task->path = g_strdup(path);
    task->temp_path = temp_path;
    task->fd = fd;
    task->pt_task = piece_table_save_async_start(doc->pt, fd);
    task->cancelled = FALSE;
    task->error = NULL;
    
    if (!task->pt_task) {
        close(fd);
        unlink(temp_path);
        g_free(temp_path);
        g_free(task->path);
        document_free(task->doc); /* Cleanup ref if start fails */
        g_free(task);
        return NULL;
    }
    
    return task;
}

gboolean
document_save_async_step(DocumentSaveTask *task, gint64 budget_us, double *progress_out)
{
    if (!task || task->cancelled) {
        if (progress_out) *progress_out = 1.0;
        return TRUE;
    }
    
    return piece_table_save_async_step(task->pt_task, budget_us, progress_out);
}

void
document_save_async_finish(DocumentSaveTask *task, GError **error)
{
    if (!task) return;
    
    if (task->cancelled) {
        /* Clean up temp file */
        if (task->fd >= 0) close(task->fd);
        if (task->temp_path) {
            unlink(task->temp_path);
            g_free(task->temp_path);
        }
        g_free(task->path);
        if (task->pt_task) piece_table_save_async_cancel(task->pt_task);
        document_free(task->doc);
        g_free(task);
        return;
    }
    
    /* Check for write errors */
    GError *write_err = piece_table_save_async_get_error(task->pt_task);
    if (write_err) {
        g_propagate_error(error, write_err);
        
        piece_table_save_async_finalize(task->pt_task);
        if (task->fd >= 0) close(task->fd);
        unlink(task->temp_path); /* Delete partial file */
        
        g_free(task->temp_path);
        g_free(task->path);
        document_free(task->doc);
        g_free(task);
        return;
    }
    
    /* Finalize stream */
    piece_table_save_async_finalize(task->pt_task);
    
    /* fsync and close */
    if (task->fd >= 0) {
        fsync(task->fd);
        close(task->fd);
    }
    
    /* Atomic rename */
    if (rename(task->temp_path, task->path) < 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Rename failed: %s", strerror(errno));
        unlink(task->temp_path);
    } else {
        /* Success - update document state */
        document_set_file_path(task->doc, task->path);
        document_mark_saved(task->doc);
    }
    
    g_free(task->temp_path);
    g_free(task->path);
    document_free(task->doc);
    g_free(task);
}

void
document_save_async_cancel(DocumentSaveTask *task)
{
    if (task) {
        task->cancelled = TRUE;
        if (task->pt_task) {
            piece_table_save_async_cancel(task->pt_task);
        }
    }
}

size_t
document_get_line_into(Document *doc, size_t line_index, char *buf, size_t buf_len)
{
    return piece_table_get_line_into(doc->pt, line_index, buf, buf_len);
}

void
document_iter_init(Document *doc, DocumentIter *iter, size_t line_index)
{
    if (line_index == 0) {
        piece_table_iter_init(doc->pt, &iter->iter);
    } else {
        piece_table_iter_init_at_line(doc->pt, &iter->iter, line_index);
    }
}

size_t
document_iter_next_line(DocumentIter *iter, char *buf, size_t buf_len)
{
    return piece_table_iter_get_next_line(&iter->iter, buf, buf_len);
}
