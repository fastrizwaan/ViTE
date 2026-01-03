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
    s->current_group = NULL;
    return s;
}

static void free_command(gpointer data);

static void
free_command(gpointer data)
{
    UndoCommand *cmd = data;
    g_free(cmd->text);
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
    free(stack);
}

static void
undo_stack_push_command(UndoStack *stack, UndoCommand *cmd)
{
    if (stack->current_group) {
        stack->current_group->group_commands = g_list_append(stack->current_group->group_commands, cmd);
    } else {
        stack->undo_stack = g_list_prepend(stack->undo_stack, cmd);
        /* Clear redo stack */
        g_list_free_full(stack->redo_stack, free_command);
        stack->redo_stack = NULL;
    }
}

void
undo_stack_push_insert(UndoStack *stack, size_t start, const char *text, size_t len)
{
    if (stack->in_undo_redo) return;
    
    UndoCommand *cmd = g_malloc0(sizeof(UndoCommand));
    cmd->type = UNDO_OP_INSERT;
    cmd->start = start;
    cmd->length = len;
    cmd->text = g_strndup(text, len);
    
    undo_stack_push_command(stack, cmd);
}

void
undo_stack_push_delete(UndoStack *stack, size_t start, const char *deleted_text, size_t len)
{
    if (stack->in_undo_redo) return;
    
    UndoCommand *cmd = g_malloc0(sizeof(UndoCommand));
    cmd->type = UNDO_OP_DELETE;
    cmd->start = start;
    cmd->length = len;
    cmd->text = g_strndup(deleted_text, len);
    
    undo_stack_push_command(stack, cmd);
}

void
undo_stack_begin_group(UndoStack *stack)
{
    if (stack->current_group) return; /* No nested groups for now */
    
    UndoCommand *group = g_malloc0(sizeof(UndoCommand));
    group->type = UNDO_OP_GROUP;
    stack->current_group = group;
}

void
undo_stack_end_group(UndoStack *stack)
{
    if (!stack->current_group) return;
    
    UndoCommand *group = stack->current_group;
    stack->current_group = NULL;
    
    if (group->group_commands == NULL) {
        free_command(group);
    } else {
        undo_stack_push_command(stack, group);
    }
}

static void
execute_command(UndoCommand *cmd, PieceTable *pt, gboolean undo)
{
    if (cmd->type == UNDO_OP_INSERT) {
        if (undo) piece_table_delete(pt, cmd->start, cmd->length);
        else piece_table_insert(pt, cmd->start, cmd->text, cmd->length);
    } else if (cmd->type == UNDO_OP_DELETE) {
        if (undo) piece_table_insert(pt, cmd->start, cmd->text, cmd->length);
        else piece_table_delete(pt, cmd->start, cmd->length);
    } else if (cmd->type == UNDO_OP_GROUP) {
        if (undo) {
            /* Undo in reverse order */
            for (GList *l = g_list_last(cmd->group_commands); l; l = l->prev) {
                execute_command(l->data, pt, TRUE);
            }
        } else {
            /* Redo in forward order */
            for (GList *l = cmd->group_commands; l; l = l->next) {
                execute_command(l->data, pt, FALSE);
            }
        }
    }
}

/* Helper to extract info from a command (or the relevant sub-command in a group) */
static void
get_command_info(UndoCommand *cmd, gboolean undo, UndoInfo *info)
{
    info->success = TRUE;
    
    if (cmd->type == UNDO_OP_GROUP) {
        /* For Group Undo: The "primary" location is usually the INITIAL action of the group
           (e.g., Drag Source). However, we iterate in reverse. 
           We likely want the location of the *last* command executed during undo 
           (which is the *first* command in the group's list). */
        if (undo) {
             /* Last executed is the first in list */
             GList *first = cmd->group_commands;
             if (first) get_command_info((UndoCommand*)first->data, undo, info);
        } else {
             /* Redo: Last executed is last in list? 
                Actually for Drag Drop Redo: Delete A, Insert B. 
                We probably want to end up at B. 
                Last executed is Insert B. */
             GList *last = g_list_last(cmd->group_commands);
             if (last) get_command_info((UndoCommand*)last->data, undo, info);
        }
        return;
    }
    
    /* Single Command */
    info->start = cmd->start;
    info->length = cmd->length;
    
    if (cmd->type == UNDO_OP_INSERT) {
        /* Undo Insert -> Delete. is_insert = FALSE. */
        /* Redo Insert -> Insert. is_insert = TRUE. */
        info->is_insert = !undo;
    } else if (cmd->type == UNDO_OP_DELETE) {
        /* Undo Delete -> Insert. is_insert = TRUE. */
        /* Redo Delete -> Delete. is_insert = FALSE. */
        info->is_insert = undo;
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
    
    stack->in_undo_redo = TRUE;
    execute_command(cmd, pt, TRUE);
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
    
    stack->in_undo_redo = TRUE;
    execute_command(cmd, pt, FALSE);
    stack->in_undo_redo = FALSE;
    
    get_command_info(cmd, FALSE, &info);
    return info;
}
