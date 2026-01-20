#include "editor-internal.h"
#include <string.h>

size_t
editor_widget_delete_selection(EditorWidget *self)
{
    /* This function handles bulk deletion of all selections */
    if (!self->cursors || self->cursors->len == 0) return 0;
    
    g_array_sort(self->cursors, compare_cursors_desc);
    
    document_begin_undo_group(self->doc);
    
    size_t total_deleted = 0;
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        
        if (start != end) {
            size_t len = end - start;
            document_delete(self->doc, start, len);
            cur->cursor_offset = start;
            cur->selection_anchor = start;
            total_deleted += len;
        }
    }
    
    document_end_undo_group(self->doc);
    return total_deleted;
}

void
editor_widget_copy(EditorWidget *self)
{
    if (!self->cursors || self->cursors->len == 0) return;

    GString *clip_text = g_string_new("");
    
    /* Copy logic: we iterate cursors. Usually document order is preferred for copy. */
    /* Implementation in widget used default array order. We should probably sort ascending. */
    
    /* Create a temp array to sort ascending */
    GArray *sorted = g_array_sized_new(FALSE, FALSE, sizeof(EditorCursor), self->cursors->len);
    g_array_append_vals(sorted, self->cursors->data, self->cursors->len);
    
    /* Sort ascending - using a lambda-like or duping compare_cursors_desc and negating? */
    /* Or just implementing a simple ascending compare here or in utils */
    /* Let's rely on standard iteration if self->cursors is usually sorted? 
       It's not guaranteed.
       Let's assume for now we just iterate.
       VS Code joins with newline.
    */
    
    /* Just use the loop as it was */
    for (guint c = 0; c < self->cursors->len; c++) {
         EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
         size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
         size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
         if (start == end) continue;
         
         char *text = document_get_text_range(self->doc, start, end - start);
         if (clip_text->len > 0) g_string_append_c(clip_text, '\n');
         g_string_append(clip_text, text);
         g_free(text);
    }
    
    if (clip_text->len > 0) {
        GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
        gdk_clipboard_set_text(clipboard, clip_text->str);
    }
    g_string_free(clip_text, TRUE);
    g_array_free(sorted, TRUE);
}

void
editor_widget_cut(EditorWidget *self)
{
    if (!self->doc) return;
    editor_widget_copy(self);
    editor_widget_delete_selection(self); 
}

static void
on_paste_text_received(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
    char *text = gdk_clipboard_read_text_finish(clipboard, res, NULL);
    if (!text) return;
    size_t len = strlen(text);
    if (len > 0) {
        document_begin_undo_group(self->doc);
        /* If we have a selection, delete it first */
        editor_widget_delete_selection(self);
        
        for (guint c = 0; c < self->cursors->len; c++) {
             EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
             document_insert(self->doc, cur->cursor_offset, text, len);
             cur->cursor_offset += len;
             cur->selection_anchor = cur->cursor_offset;
        }
        
        document_end_undo_group(self->doc);
        
        editor_widget_reset_cursor_blink(self);
        editor_widget_update_adjustments(self, -1, -1);
        scroll_to_cursor(self);
        gtk_widget_queue_draw(GTK_WIDGET(self));

        size_t line, col;
        editor_widget_get_cursor_position(self, &line, &col);
        g_signal_emit(self, editor_signals[CURSOR_MOVED], 0, (guint)line, (guint)col);
    }
    g_free(text);
}

void
editor_widget_paste(EditorWidget *self)
{
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
    gdk_clipboard_read_text_async(clipboard, NULL, on_paste_text_received, self);
}

static void
on_primary_paste_received(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
    char *text = gdk_clipboard_read_text_finish(clipboard, res, NULL);
    
    if (text) {
        size_t len = strlen(text);
        if (len > 0) {
            EditorCursor *primary = editor_widget_get_primary_cursor(self);
            if (primary) {
                document_begin_undo_group(self->doc);
                document_insert(self->doc, primary->cursor_offset, text, len);
                
                primary->cursor_offset += len;
                primary->selection_anchor = primary->cursor_offset;
                
                document_end_undo_group(self->doc);
                
                editor_widget_reset_cursor_blink(self);
                editor_widget_update_adjustments(self, -1, -1);
                scroll_to_cursor(self);
                gtk_widget_queue_draw(GTK_WIDGET(self));

                size_t line, col;
                editor_widget_get_cursor_position(self, &line, &col);
                g_signal_emit(self, editor_signals[CURSOR_MOVED], 0, (guint)line, (guint)col);
            }
        }
        g_free(text);
    }
}

void
editor_widget_paste_primary(EditorWidget *self)
{
    GdkClipboard *clipboard = gdk_display_get_primary_clipboard(gtk_widget_get_display(GTK_WIDGET(self)));
    gdk_clipboard_read_text_async(clipboard, NULL, on_primary_paste_received, self);
}
