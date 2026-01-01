#ifndef UNDO_H
#define UNDO_H

#include "piece-table.h"
#include <glib.h>

typedef enum {
    UNDO_OP_INSERT,
    UNDO_OP_DELETE
} UndoOpType;

typedef struct {
    UndoOpType type;
    size_t start;
    char *text; /* For insert: text inserted (ref counting?? No. Copy.). For delete: text deleted. */
    size_t length;
} UndoCommand;

typedef struct {
    GList *undo_stack; /* List of UndoCommand* */
    GList *redo_stack; /* List of UndoCommand* */
    
    gboolean in_undo_redo; /* Flag to prevent recording during undo/redo execution */
} UndoStack;

UndoStack *undo_stack_new(void);
void undo_stack_free(UndoStack *stack);

void undo_stack_push_insert(UndoStack *stack, size_t start, const char *text, size_t len);
void undo_stack_push_delete(UndoStack *stack, size_t start, const char *deleted_text, size_t len);

/* Returns command to execute (inverse of recorded) */
gboolean undo_stack_undo(UndoStack *stack, PieceTable *pt);
gboolean undo_stack_redo(UndoStack *stack, PieceTable *pt);

#endif
