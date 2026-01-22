#ifndef UNDO_H
#define UNDO_H

#include "piece-table.h"
#include <glib.h>

typedef enum {
    UNDO_OP_INSERT,
    UNDO_OP_DELETE,
    UNDO_OP_GROUP,
    UNDO_OP_RESTORE_FROM_PATH /* For bulk replaces (restore from file) */
} UndoOpType;

typedef struct _UndoCommand {
    UndoOpType type;
    size_t start;
    
    /* For INSERT/DELETE: Text is stored in the UndoStack's log file */
    uint64_t log_offset;
    size_t length;
    char *cached_text; /* RAM cache for smaller edits (avoids disk I/O) */
    
    /* For RESTORE_FROM_PATH: Paths to snapshot files */
    char *undo_path;
    char *redo_path;
    
    GList *group_commands; /* List of UndoCommand* if type == UNDO_OP_GROUP */
    
    /* Selection state to restore after UNDO (i.e., state BEFORE the operation) */
    gboolean has_selection;
    size_t selection_start;
    size_t selection_end;
    
    /* Selection state to restore after REDO (i.e., state AFTER the operation) */
    gboolean has_redo_selection;
    size_t redo_selection_start;
    size_t redo_selection_end;
} UndoCommand;

typedef struct {
    GList *undo_stack; /* List of UndoCommand* */
    GList *redo_stack; /* List of UndoCommand* */
    
    gboolean in_undo_redo; /* Flag to prevent recording during undo/redo execution */
    UndoCommand *current_group; /* Active group if any */
    
    FILE *log_file; /* Append-only log for text data */
    char *log_file_path;
} UndoStack;

UndoStack *undo_stack_new(void);
void undo_stack_free(UndoStack *stack);

void undo_stack_push_insert(UndoStack *stack, size_t start, const char *text, size_t len);
/* Push insertion where content is read from a file descriptor (for HUGE files) */
void undo_stack_push_insert_from_fd(UndoStack *stack, size_t start, int fd, size_t len);
void undo_stack_push_delete(UndoStack *stack, size_t start, const char *deleted_text, size_t len);
void undo_stack_push_restore_path(UndoStack *stack, const char *undo_path, const char *redo_path);
void undo_stack_push_command(UndoStack *stack, UndoCommand *cmd);

void undo_stack_begin_group(UndoStack *stack);
void undo_stack_end_group(UndoStack *stack);
void undo_stack_set_group_selection(UndoStack *stack, size_t start, size_t end);
void undo_stack_set_group_selection_after(UndoStack *stack, size_t start, size_t end);

typedef struct {
    gboolean success;
    size_t start;
    size_t length;
    gboolean is_insert; /* If true, text was inserted by the operation */
    
    gboolean has_selection;
    size_t selection_start;
    size_t selection_end;
} UndoInfo;

/* Returns info about the executed operation */
UndoInfo undo_stack_undo(UndoStack *stack, PieceTable *pt);
UndoInfo undo_stack_redo(UndoStack *stack, PieceTable *pt);

/* For modification tracking */
void *undo_stack_peek(UndoStack *stack);

#endif
