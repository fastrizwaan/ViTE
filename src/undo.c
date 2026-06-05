#include "undo.h"
#include "piece-table.h"
#include "resource-check.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>

/* Zero-RAM strategy: No RAM threshold, always disk. */

/* Zero-RAM strategy: No RAM threshold, always disk. */

static const char *
undo_temp_dir(void)
{
    const char *tmp_dir = g_get_tmp_dir();
    if (!tmp_dir || !*tmp_dir) tmp_dir = "/tmp";
    return tmp_dir;
}

UndoStack *
undo_stack_new(void)
{
    UndoStack *s = g_malloc0(sizeof(UndoStack));
    s->undo_stack = NULL;
    s->redo_stack = NULL;
    s->in_undo_redo = FALSE;
    s->current_group = NULL;
    s->group_depth = 0;
    
    /* Create log file for text storage */
    /* Create log file for text storage */
    const char *tmp_dir = undo_temp_dir();
#ifdef O_TMPFILE
    int fd = open(tmp_dir, O_TMPFILE | O_RDWR | O_EXCL, 0600);
    if (fd != -1) {
        s->log_file = fdopen(fd, "w+");
        s->log_file_path = NULL; /* No path needed for O_TMPFILE */
    } else
#endif
    {
        s->log_file_path = g_strdup_printf("%s/vite_undo_log_XXXXXX", tmp_dir);
        int fd = mkstemp(s->log_file_path);
        if (fd != -1) {
            /* Unlink immediately so file is deleted when fd closes (crash safety) */
            unlink(s->log_file_path);
            s->log_file = fdopen(fd, "w+");
        }
    }

    if (!s->log_file) {
        g_warning("Failed to open undo log file: %s", strerror(errno));
        if (s->log_file_path) {
             g_free(s->log_file_path);
             s->log_file_path = NULL;
        }
        g_free(s);
        return NULL;
    }
    
    return s;
}

static void free_command(gpointer data);

static void
free_command(gpointer data)
{
    UndoCommand *cmd = data;
    
    /* cached_text is removed from struct */
 
    if (cmd->undo_path) {
        unlink(cmd->undo_path);
        g_free(cmd->undo_path);
    }
    if (cmd->redo_path) {
        unlink(cmd->redo_path);
        g_free(cmd->redo_path);
    }
    if (cmd->type == UNDO_OP_CUSTOM && cmd->custom_free) {
        cmd->custom_free(cmd->custom_data);
    }
    
    if (cmd->group_commands) {
        g_list_free_full(cmd->group_commands, free_command);
    }
    g_free(cmd);
}

void
undo_stack_free(UndoStack *stack)
{
    g_list_free_full(stack->undo_stack, free_command);
    g_list_free_full(stack->redo_stack, free_command);
    if (stack->current_group) {
        free_command(stack->current_group);
    }
    
    if (stack->log_file) {
        fclose(stack->log_file);
    }
    if (stack->log_file_path) {
        unlink(stack->log_file_path);
        g_free(stack->log_file_path);
    }
    
    if (stack->map_base && stack->map_size > 0) {
        munmap(stack->map_base, stack->map_size);
    }
    
    g_free(stack);
}

void
undo_stack_clear(UndoStack *stack)
{
    if (!stack) return;
    
    g_list_free_full(stack->undo_stack, free_command);
    stack->undo_stack = NULL;
    
    g_list_free_full(stack->redo_stack, free_command);
    stack->redo_stack = NULL;
    
    if (stack->current_group) {
        free_command(stack->current_group);
        stack->current_group = NULL;
    }
    
    stack->group_depth = 0;
    
    /* We could also truncate the log file to reclaim space, but for now we'll just keep appending.
     * In a full implementation, we might want to truncate if log_offset goes back to 0.
     */
}

static void
maybe_prune_undo_history(UndoStack *stack)
{
    /* 1. Check command count limit */
    if (stack->undo_count > UNDO_MAX_COMMANDS) {
        /* Find the tail and prune the last 10% */
        size_t to_prune = stack->undo_count / 10;
        if (to_prune == 0) to_prune = 1;
        
        GList *tail = g_list_last(stack->undo_stack);
        while (tail && to_prune > 0) {
            GList *prev = tail->prev;
            free_command(tail->data);
            stack->undo_stack = g_list_delete_link(stack->undo_stack, tail);
            stack->undo_count--;
            to_prune--;
            tail = prev;
        }
    }
    
    /* 2. Check log size limit (e.g. 2GB) */
    if (stack->log_file && stack->log_written > UNDO_MAX_LOG_SIZE) {
        g_warning("Undo log exceeded %llu bytes. Truncating history to reclaim disk space.", (unsigned long long)UNDO_MAX_LOG_SIZE);
        
        /* Clear all history to safely truncate log */
        g_list_free_full(stack->undo_stack, free_command);
        stack->undo_stack = NULL;
        stack->undo_count = 0;
        
        g_list_free_full(stack->redo_stack, free_command);
        stack->redo_stack = NULL;
        
        /* Truncate log file */
        if (stack->map_base) {
            munmap(stack->map_base, stack->map_size);
            stack->map_base = NULL;
            stack->map_size = 0;
        }
        
        /* Close and reopen log file to truncate it */
        fclose(stack->log_file);
        stack->log_file = fopen(stack->log_file_path, "w+b");
        if (!stack->log_file) {
            g_warning("Failed to recreate undo log after truncation");
        }
        stack->log_written = 0;
    }
}

void
undo_stack_push_command(UndoStack *stack, UndoCommand *cmd)
{
    if (stack->current_group) {
        /* O(1) prepend - list is reversed in undo_stack_end_group() */
        stack->current_group->group_commands = g_list_prepend(stack->current_group->group_commands, cmd);
    } else {
        stack->undo_stack = g_list_prepend(stack->undo_stack, cmd);
        stack->undo_count++;
        /* Clear redo stack */
        g_list_free_full(stack->redo_stack, free_command);
        stack->redo_stack = NULL;
        /* Prune if over limit */
        maybe_prune_undo_history(stack);
    }
}

void
undo_stack_push_insert(UndoStack *stack, size_t start, const char *text, size_t len)
{
    if (stack->in_undo_redo) return;
    
    /* Validate size */
    if (!resource_size_valid(len)) {
        g_warning("undo_stack_push_insert: Invalid size %zu (possible overflow)", len);
        return;
    }
    
    UndoCommand *cmd = g_malloc0(sizeof(UndoCommand));
    cmd->type = UNDO_OP_INSERT;
    cmd->start = start;
    cmd->length = len;
    
    if (stack->current_group) {
        stack->current_group_size += len;
    }
    
    /* Zero-RAM Strategy: Always write to disk log */
    if (stack->log_file && len > 0) {
        fseeko(stack->log_file, 0, SEEK_END);
        cmd->log_offset = ftello(stack->log_file);
        
        /* Check disk space before writing */
        if (!resource_can_write_disk(undo_temp_dir(), len)) {
             g_warning("undo_stack_push_insert: Disk full, dropping undo data");
             cmd->length = 0;
        } else {
             size_t written = fwrite(text, 1, len, stack->log_file);
             fflush(stack->log_file);
             stack->log_written += written;
             if (written != len) {
                 g_warning("undo_stack_push_insert: Short write %zu < %zu", written, len);
                 cmd->length = written;
             }
        }
    }
    
    undo_stack_push_command(stack, cmd);
}

void
undo_stack_push_insert_from_fd(UndoStack *stack, size_t start, int fd, size_t len)
{
    if (stack->in_undo_redo) return;
    
    /* Validate size */
    if (!resource_size_valid(len)) {
        g_warning("undo_stack_push_insert_from_fd: Invalid size %zu", len);
        return;
    }

    UndoCommand *cmd = g_malloc0(sizeof(UndoCommand));
    cmd->type = UNDO_OP_INSERT;
    cmd->start = start;
    cmd->length = len;

    /* Zero-RAM Strategy: Always write to disk log */
    if (stack->log_file && len > 0 && fd >= 0) {
        fseeko(stack->log_file, 0, SEEK_END);
        cmd->log_offset = ftello(stack->log_file);
        
        /* Check disk space */
        if (!resource_can_write_disk(undo_temp_dir(), len)) {
             g_warning("undo_stack_push_insert_from_fd: Disk full");
             cmd->length = 0;
        } else {
            /* Copy data loop */
            char buf[65536]; /* 64KB buffer */
            size_t written = 0;
            off_t original_pos = lseek(fd, 0, SEEK_CUR);
            lseek(fd, 0, SEEK_SET);
    
            while (written < len) {
                size_t to_read = len - written;
                if (to_read > sizeof(buf)) to_read = sizeof(buf);
                
                ssize_t r = read(fd, buf, to_read);
                if (r <= 0) break;
                
                fwrite(buf, 1, r, stack->log_file);
                written += r;
            }
            fflush(stack->log_file);
            lseek(fd, original_pos, SEEK_SET); /* Restore FD pos */
            
            if (written != len) {
                g_warning("undo_stack_push_insert_from_fd: Short write %zu < %zu", written, len);
                cmd->length = written;
            }
        }
    }

    undo_stack_push_command(stack, cmd);
}

void
undo_stack_push_delete(UndoStack *stack, size_t start, const char *deleted_text, size_t len)
{
    if (stack->in_undo_redo) return;
    
    if (!resource_size_valid(len)) {
         g_warning("undo_stack_push_delete: Invalid size %zu (possible overflow)", len);
         return;
    }
    
    UndoCommand *cmd = g_malloc0(sizeof(UndoCommand));
    cmd->type = UNDO_OP_DELETE;
    cmd->start = start;
    cmd->length = len;
    
    if (stack->current_group) {
        stack->current_group_size += len;
    }
    
    /* Always write to disk */
    if (stack->log_file && len > 0) {
        fseeko(stack->log_file, 0, SEEK_END);
        cmd->log_offset = ftello(stack->log_file);
        
        if (!resource_can_write_disk(undo_temp_dir(), len)) {
             g_warning("undo_stack_push_delete: Disk full");
             cmd->length = 0;
        } else {
             size_t written = fwrite(deleted_text, 1, len, stack->log_file);
             fflush(stack->log_file);
             stack->log_written += written;
             if (written != len) {
                 g_warning("undo_stack_push_delete: Short write %zu < %zu", written, len);
                 cmd->length = written;
             }
        }
    }
    
    undo_stack_push_command(stack, cmd);
}

void
undo_stack_push_delete_streaming(UndoStack *stack, size_t start, PieceTable *pt, size_t offset, size_t len)
{
    if (stack->in_undo_redo) return;
    
    if (!resource_size_valid(len)) {
        g_warning("undo_stack_push_delete_streaming: Invalid size %zu", len);
        return;
    }
    
    UndoCommand *cmd = g_malloc0(sizeof(UndoCommand));
    cmd->type = UNDO_OP_DELETE;
    cmd->start = start;
    cmd->length = len;
    
    if (stack->current_group) {
        stack->current_group_size += len;
    }
    
    /* Zero-copy streaming: read directly from piece table mmap'd data into log */
    if (stack->log_file && len > 0) {
        if (!resource_can_write_disk(undo_temp_dir(), len)) {
            g_warning("undo_stack_push_delete_streaming: Disk full");
            cmd->length = 0;
        } else {
            fseeko(stack->log_file, 0, SEEK_END);
            cmd->log_offset = ftello(stack->log_file);
            
            PieceTableIter iter;
            piece_table_iter_init_at_offset(pt, &iter, offset);
            
            size_t written = 0;
            while (written < len) {
                size_t chunk_len = 0;
                const char *chunk = piece_table_iter_get_chunk(&iter, &chunk_len);
                if (!chunk || chunk_len == 0) break;
                
                /* Clamp to remaining bytes needed */
                size_t to_write = len - written;
                if (chunk_len > to_write) chunk_len = to_write;
                
                size_t w = fwrite(chunk, 1, chunk_len, stack->log_file);
                written += w;
                if (w != chunk_len) {
                    g_warning("undo_stack_push_delete_streaming: Short write %zu < %zu", w, chunk_len);
                    break;
                }
                
                piece_table_iter_advance(&iter, chunk_len);
            }
            fflush(stack->log_file);
            stack->log_written += written;
            
            if (written != len) {
                g_warning("undo_stack_push_delete_streaming: Wrote %zu of %zu", written, len);
                cmd->length = written;
            }
        }
    }
    
    undo_stack_push_command(stack, cmd);
}

void
undo_stack_push_restore_path(UndoStack *stack, const char *undo_path, const char *redo_path)
{
    if (stack->in_undo_redo) return;

    UndoCommand *cmd = g_malloc0(sizeof(UndoCommand));
    cmd->type = UNDO_OP_RESTORE_FROM_PATH;
    cmd->undo_path = g_strdup(undo_path);
    cmd->redo_path = g_strdup(redo_path);
    
    undo_stack_push_command(stack, cmd);
}

void
undo_stack_push_custom(UndoStack *stack, void *data, void (*exec_func)(void *, gboolean), void (*free_func)(void *))
{
    if (stack->in_undo_redo) return;

    UndoCommand *cmd = g_malloc0(sizeof(UndoCommand));
    cmd->type = UNDO_OP_CUSTOM;
    cmd->custom_data = data;
    cmd->custom_execute = exec_func;
    cmd->custom_free = free_func;
    
    undo_stack_push_command(stack, cmd);
}

void
undo_stack_begin_group(UndoStack *stack)
{
    stack->group_depth++;
    if (stack->group_depth > 1) return;
    
    /* If for some reason current_group is already set (should differ from depth > 1 check), abort */
    if (stack->current_group) return; 
    
    UndoCommand *group = g_malloc0(sizeof(UndoCommand));
    group->type = UNDO_OP_GROUP;
    stack->current_group = group;
    stack->current_group_size = 0; /* Reset accumulator */
}

void
undo_stack_end_group(UndoStack *stack)
{
    if (stack->group_depth > 0) stack->group_depth--;
    
    if (stack->group_depth > 0) return; /* Still nested */
    
    if (!stack->current_group) return;
    
    UndoCommand *group = stack->current_group;
    stack->current_group = NULL;
    
    if (group->group_commands == NULL) {
        free_command(group);
    } else {
        /* Reverse the list since we used g_list_prepend for O(1) building */
        group->group_commands = g_list_reverse(group->group_commands);
        undo_stack_push_command(stack, group);
    }
}

void
undo_stack_set_group_selection(UndoStack *stack, size_t start, size_t end)
{
    if (stack->current_group) {
        stack->current_group->has_selection = TRUE;
        stack->current_group->selection_start = start;
        stack->current_group->selection_end = end;
    }
}

void
undo_stack_set_group_selection_after(UndoStack *stack, size_t start, size_t end)
{
    if (stack->current_group) {
        stack->current_group->has_redo_selection = TRUE;
        stack->current_group->redo_selection_start = start;
        stack->current_group->redo_selection_end = end;
    }
}

/* Helper: Ensure memory map covers the whole log file */
static void
ensure_mmap(UndoStack *stack)
{
    if (!stack->log_file) return;
    
    int fd = fileno(stack->log_file);
    struct stat st;
    if (fstat(fd, &st) < 0) return;
    
    size_t file_size = st.st_size;
    if (file_size == 0) return;
    
    if (stack->map_size < file_size) {
        if (stack->map_base) {
            munmap(stack->map_base, stack->map_size);
        }
        stack->map_size = file_size;
        stack->map_base = mmap(NULL, stack->map_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (stack->map_base == MAP_FAILED) {
             g_warning("ensure_mmap: mmap failed: %s", strerror(errno));
             stack->map_base = NULL;
             stack->map_size = 0;
        }
    }
}

static void
execute_command(UndoStack *stack, UndoCommand *cmd, PieceTable *pt, gboolean undo)
{
    if (cmd->type == UNDO_OP_INSERT) {
        if (undo) {
             /* Delete what was inserted */
             piece_table_delete(pt, cmd->start, cmd->length);
        } else {
             /* Redo Insert: Read from mmap log */
             ensure_mmap(stack);
             if (stack->map_base && (cmd->log_offset + cmd->length <= stack->map_size)) {
                 piece_table_insert(pt, cmd->start, stack->map_base + cmd->log_offset, cmd->length);
             } else {
                 g_warning("execute_command: Undo log unavailable or truncated");
             }
        }
    } else if (cmd->type == UNDO_OP_DELETE) {
        if (undo) {
             /* Undo Delete -> Insert back from mmap log */
             ensure_mmap(stack);
             if (stack->map_base && (cmd->log_offset + cmd->length <= stack->map_size)) {
                 piece_table_insert(pt, cmd->start, stack->map_base + cmd->log_offset, cmd->length);
             } else {
                 g_warning("execute_command: Undo log unavailable or truncated");
             }
        } else {
             /* Redo Delete */
             piece_table_delete(pt, cmd->start, cmd->length);
        }
    } else if (cmd->type == UNDO_OP_RESTORE_FROM_PATH) {
        /* Swap backing file */
        const char *path = undo ? cmd->undo_path : cmd->redo_path;
        
        if (path) {
            int fd = open(path, O_RDONLY);
            if (fd >= 0) {
                struct stat st;
                fstat(fd, &st);
                size_t sz = st.st_size;
                piece_table_replace_from_fd(pt, fd, sz, 0);
                close(fd);
            }
        }
        
    } else if (cmd->type == UNDO_OP_GROUP) {
        /* Batch mmap once before executing all sub-commands */
        ensure_mmap(stack);
        if (undo) {
            /* Undo in reverse order */
            for (GList *l = g_list_last(cmd->group_commands); l; l = l->prev) {
                execute_command(stack, l->data, pt, TRUE);
            }
        } else {
            /* Redo in forward order */
            for (GList *l = cmd->group_commands; l; l = l->next) {
                execute_command(stack, l->data, pt, FALSE);
            }
        }
    } else if (cmd->type == UNDO_OP_CUSTOM) {
        if (cmd->custom_execute) {
            cmd->custom_execute(cmd->custom_data, undo);
        }
    }
}

/* Helper to extract info from a command (or the relevant sub-command in a group) */
static void
get_command_info(UndoCommand *cmd, gboolean undo, UndoInfo *info)
{
    info->success = TRUE;
    
    if (cmd->type == UNDO_OP_GROUP) {
        /* If the group has explicit selection info, prioritize it */
        gboolean group_has_sel = (undo && cmd->has_selection) || (!undo && cmd->has_redo_selection);
        
        if (group_has_sel) {
            info->success = TRUE; /* implicit */
            info->has_selection = TRUE;
            if (undo) {
                info->selection_start = cmd->selection_start;
                info->selection_end = cmd->selection_end;
            } else {
                info->selection_start = cmd->redo_selection_start;
                info->selection_end = cmd->redo_selection_end;
            }
        }
        
        /* Check sub-commands for other metadata, but prioritize group selection. */
        UndoInfo child_info = {0};
        if (undo) {
             GList *first = cmd->group_commands;
             if (first) get_command_info((UndoCommand*)first->data, undo, &child_info);
        } else {
             GList *last = g_list_last(cmd->group_commands);
             if (last) get_command_info((UndoCommand*)last->data, undo, &child_info);
        }
        
        if (!group_has_sel) {
            *info = child_info;
        } else {
            /* Merge non-selection info (start, length, type) from child */
            info->start = child_info.start;
            info->length = child_info.length;
            info->is_insert = child_info.is_insert;
        }
        return;
    }
    
    /* Single Command */
    info->start = cmd->start;
    info->length = cmd->length;
    
    if (cmd->type == UNDO_OP_INSERT) {
        info->is_insert = !undo;
    } else if (cmd->type == UNDO_OP_DELETE) {
        info->is_insert = undo;
    } else if (cmd->type == UNDO_OP_RESTORE_FROM_PATH) {
        info->is_insert = FALSE; /* Full reload */
    } else if (cmd->type == UNDO_OP_CUSTOM) {
        info->is_insert = FALSE; /* Metadata change */
    }
}

UndoInfo
undo_stack_undo(UndoStack *stack, PieceTable *pt)
{
    UndoInfo info = {0};
    if (!stack->undo_stack) return info;
    
    UndoCommand *cmd = stack->undo_stack->data;
    stack->undo_stack = g_list_remove(stack->undo_stack, cmd);
    stack->redo_stack = g_list_prepend(stack->redo_stack, cmd);
    if (stack->undo_count > 0) stack->undo_count--;
    
    stack->in_undo_redo = TRUE;
    execute_command(stack, cmd, pt, TRUE);
    stack->in_undo_redo = FALSE;
    
    get_command_info(cmd, TRUE, &info);
    return info;
}

UndoInfo
undo_stack_redo(UndoStack *stack, PieceTable *pt)
{
    UndoInfo info = {0};
    if (!stack->redo_stack) return info;
    
    UndoCommand *cmd = stack->redo_stack->data;
    stack->redo_stack = g_list_remove(stack->redo_stack, cmd);
    stack->undo_stack = g_list_prepend(stack->undo_stack, cmd);
    stack->undo_count++;
    
    stack->in_undo_redo = TRUE;
    execute_command(stack, cmd, pt, FALSE);
    stack->in_undo_redo = FALSE;
    
    get_command_info(cmd, FALSE, &info);
    return info;
}

void *
undo_stack_peek(UndoStack *stack)
{
    if (!stack) return NULL;
    if (stack->current_group && stack->current_group->group_commands) {
        return stack->current_group;
    }
    if (!stack->undo_stack) return NULL;
    return stack->undo_stack->data;
}

void *
undo_stack_peek_redo(UndoStack *stack)
{
    if (!stack || !stack->redo_stack) return NULL;
    return stack->redo_stack->data;
}

UndoInfo
undo_stack_undo_skip_execute(UndoStack *stack)
{
    UndoInfo info = {0};
    if (!stack || !stack->undo_stack) return info;
    
    UndoCommand *cmd = stack->undo_stack->data;
    stack->undo_stack = g_list_remove(stack->undo_stack, cmd);
    stack->redo_stack = g_list_prepend(stack->redo_stack, cmd);
    
    get_command_info(cmd, TRUE, &info);
    return info;
}

UndoInfo
undo_stack_redo_skip_execute(UndoStack *stack)
{
    UndoInfo info = {0};
    if (!stack || !stack->redo_stack) return info;
    
    UndoCommand *cmd = stack->redo_stack->data;
    stack->redo_stack = g_list_remove(stack->redo_stack, cmd);
    stack->undo_stack = g_list_prepend(stack->undo_stack, cmd);
    
    get_command_info(cmd, FALSE, &info);
    return info;
}
