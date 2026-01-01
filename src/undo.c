#include "undo.h"
#include <string.h>
#include <stdlib.h>

UndoStack *
undo_stack_new(void)
{
    UndoStack *s = malloc(sizeof(UndoStack));
    s->undo_stack = NULL;
    s->redo_stack = NULL;
    s->in_undo_redo = FALSE;
    return s;
}

static void
free_command(gpointer data)
{
    UndoCommand *cmd = data;
    g_free(cmd->text);
    g_free(cmd);
}

void
undo_stack_free(UndoStack *stack)
{
    g_list_free_full(stack->undo_stack, free_command);
    g_list_free_full(stack->redo_stack, free_command);
    free(stack);
}

void
undo_stack_push_insert(UndoStack *stack, size_t start, const char *text, size_t len)
{
    if (stack->in_undo_redo) return;
    
    UndoCommand *cmd = g_malloc(sizeof(UndoCommand));
    cmd->type = UNDO_OP_INSERT;
    cmd->start = start;
    cmd->length = len;
    cmd->text = g_strndup(text, len);
    
    stack->undo_stack = g_list_prepend(stack->undo_stack, cmd);
    
    /* Clear redo stack */
    g_list_free_full(stack->redo_stack, free_command);
    stack->redo_stack = NULL;
}

void
undo_stack_push_delete(UndoStack *stack, size_t start, const char *deleted_text, size_t len)
{
    if (stack->in_undo_redo) return;
    
    UndoCommand *cmd = g_malloc(sizeof(UndoCommand));
    cmd->type = UNDO_OP_DELETE;
    cmd->start = start;
    cmd->length = len;
    cmd->text = g_strndup(deleted_text, len);
    
    stack->undo_stack = g_list_prepend(stack->undo_stack, cmd);
    
    /* Clear redo stack */
    g_list_free_full(stack->redo_stack, free_command);
    stack->redo_stack = NULL;
}

gboolean
undo_stack_undo(UndoStack *stack, PieceTable *pt)
{
    if (!stack->undo_stack) return FALSE;
    
    UndoCommand *cmd = stack->undo_stack->data;
    stack->undo_stack = g_list_remove(stack->undo_stack, cmd);
    stack->redo_stack = g_list_prepend(stack->redo_stack, cmd);
    
    stack->in_undo_redo = TRUE;
    
    if (cmd->type == UNDO_OP_INSERT) {
        /* Inverse of Insert is Delete */
        piece_table_delete(pt, cmd->start, cmd->length);
    } else {
        /* Inverse of Delete is Insert */
        piece_table_insert(pt, cmd->start, cmd->text, cmd->length);
    }
    
    stack->in_undo_redo = FALSE;
    return TRUE;
}

gboolean
undo_stack_redo(UndoStack *stack, PieceTable *pt)
{
    if (!stack->redo_stack) return FALSE;
    
    UndoCommand *cmd = stack->redo_stack->data;
    stack->redo_stack = g_list_remove(stack->redo_stack, cmd);
    stack->undo_stack = g_list_prepend(stack->undo_stack, cmd);
    
    stack->in_undo_redo = TRUE;
    
    if (cmd->type == UNDO_OP_INSERT) {
        /* Redo Insert */
        piece_table_insert(pt, cmd->start, cmd->text, cmd->length);
    } else {
        /* Redo Delete */
        piece_table_delete(pt, cmd->start, cmd->length);
    }
    
    stack->in_undo_redo = FALSE;
    return TRUE;
}
