#ifndef UNDO_H
#define UNDO_H

#include "piece-table.h"
#include <glib.h>

typedef enum {
    UNDO_OP_INSERT,
    UNDO_OP_DELETE,
    UNDO_OP_GROUP
} UndoOpType;

typedef struct _UndoCommand {
    UndoOpType type;
    size_t start;
    char *text;
    size_t length;
    GList *group_commands; /* List of UndoCommand* if type == UNDO_OP_GROUP */
} UndoCommand;

typedef struct {
    GList *undo_stack; /* List of UndoCommand* */
    GList *redo_stack; /* List of UndoCommand* */
    
    gboolean in_undo_redo; /* Flag to prevent recording during undo/redo execution */
    UndoCommand *current_group; /* Active group if any */
} UndoStack;

UndoStack *undo_stack_new(void);
void undo_stack_free(UndoStack *stack);

void undo_stack_push_insert(UndoStack *stack, size_t start, const char *text, size_t len);
void undo_stack_push_delete(UndoStack *stack, size_t start, const char *deleted_text, size_t len);

void undo_stack_begin_group(UndoStack *stack);
void undo_stack_end_group(UndoStack *stack);

typedef struct {
    gboolean success;
    size_t start;
    size_t length;
    gboolean is_insert; /* If true, text was inserted by the operation */
} UndoInfo;

/* Returns info about the executed operation */
UndoInfo undo_stack_undo(UndoStack *stack, PieceTable *pt);
UndoInfo undo_stack_redo(UndoStack *stack, PieceTable *pt);

#endif
