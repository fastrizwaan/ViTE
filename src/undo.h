#ifndef UNDO_H
#define UNDO_H

#include "piece-table.h"
#include <glib.h>

/* Undo history limits - prevents unbounded growth */
#define UNDO_MAX_COMMANDS      10000
#define UNDO_MAX_LOG_SIZE      (2ULL * 1024 * 1024 * 1024)  /* 2GB */

typedef enum {
    UNDO_OP_INSERT,
    UNDO_OP_DELETE,
    UNDO_OP_GROUP,
    UNDO_OP_RESTORE_FROM_PATH, /* For bulk replaces (restore from file) */
    UNDO_OP_CUSTOM /* For metadata states (e.g. syntax highlights) */
} UndoOpType;

typedef struct _UndoCommand {
    UndoOpType type;
    size_t start;
    
    /* For INSERT/DELETE: Text is stored in the UndoStack's log file */
    /* For INSERT/DELETE: Text is stored in the UndoStack's log file */
    uint64_t log_offset;
    size_t length;
    /* cached_text removed - using mmap exclusively */
    
    /* For RESTORE_FROM_PATH: File descriptors to snapshot files */
    int undo_fd;
    int redo_fd;
    
    GList *group_commands; /* List of UndoCommand* if type == UNDO_OP_GROUP */
    
    /* For CUSTOM: Arbitrary data and callbacks */
    void *custom_data;
    void (*custom_execute)(void *data, gboolean is_undo);
    void (*custom_free)(void *data);
    
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
    size_t current_group_size; /* Accumulated size of commands in current group */
    
    FILE *log_file; /* Append-only log for text data */
    char *log_file_path;
    
    int group_depth; /* Nesting level for undo groups */
    
    /* Memory mapping removed - using direct FD for zero-RAM access */
    
    /* History management */
    size_t undo_count;  /* Cached count of commands in undo_stack */
    size_t log_written;  /* Total bytes written to log (approximate) */
} UndoStack;

UndoStack *undo_stack_new(void);
void undo_stack_free(UndoStack *stack);
void undo_stack_clear(UndoStack *stack);

void undo_stack_push_insert(UndoStack *stack, size_t start, const char *text, size_t len);
/* Push insertion where content is read from a file descriptor (for HUGE files) */
void undo_stack_push_insert_from_fd(UndoStack *stack, size_t start, int fd, size_t len);

/* Async variant for huge files to prevent blocking the UI */
typedef struct _UndoInsertFdTask UndoInsertFdTask;
UndoInsertFdTask *undo_stack_push_insert_from_fd_async(UndoStack *stack, size_t start, int fd, size_t len);
gboolean undo_stack_push_insert_from_fd_step(UndoInsertFdTask *task, gint64 budget_micros, double *progress);
void undo_stack_push_insert_from_fd_finish(UndoInsertFdTask *task);
void undo_stack_push_insert_from_fd_cancel(UndoInsertFdTask *task);
void undo_stack_push_delete(UndoStack *stack, size_t start, const char *deleted_text, size_t len);
/* Zero-copy streaming delete: reads directly from piece table iterator into log */
void undo_stack_push_delete_streaming(UndoStack *stack, size_t start, PieceTable *pt, size_t offset, size_t len);
void undo_stack_push_restore_fd(UndoStack *stack, int undo_fd, int redo_fd);
void undo_stack_push_custom(UndoStack *stack, void *data, void (*exec_func)(void *, gboolean), void (*free_func)(void *));
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

/* Variants that only manage the stack/info and skip execution (for async management) */
UndoInfo undo_stack_undo_skip_execute(UndoStack *stack);
UndoInfo undo_stack_redo_skip_execute(UndoStack *stack);

/* Incremental Execution for Groups */
typedef struct _UndoExecutor UndoExecutor;

typedef void (*UndoExecutorCallback)(UndoOpType type, size_t offset, int64_t byte_delta, gpointer user_data);

UndoExecutor *undo_executor_start_undo(UndoStack *stack, PieceTable *pt, UndoExecutorCallback cb, gpointer user_data);
UndoExecutor *undo_executor_start_redo(UndoStack *stack, PieceTable *pt, UndoExecutorCallback cb, gpointer user_data);
gboolean undo_executor_step(UndoExecutor *exec, gint64 budget_micros, double *progress);
UndoInfo undo_executor_finish(UndoExecutor *exec);

/* For modification tracking */
void *undo_stack_peek(UndoStack *stack);
void *undo_stack_peek_redo(UndoStack *stack);

#endif
