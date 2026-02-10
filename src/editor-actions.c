#include "editor-internal.h"
#include <string.h>
#include <math.h>
#include <adwaita.h>

static int compare_size_t(gconstpointer a, gconstpointer b) {
    size_t sa = *(const size_t*)a;
    size_t sb = *(const size_t*)b;
    if (sa < sb) return -1;
    if (sa > sb) return 1;
    return 0;
}

void
editor_widget_finish_typing_undo_group(EditorWidget *self)
{
    if (self->typing_undo_group_active) {
        document_end_undo_group(self->doc);
        self->typing_undo_group_active = FALSE;
        self->last_char_was_separator = FALSE;
    }
}

void
move_cursor(EditorWidget *self, int visual_lines_delta)
{
    if (visual_lines_delta == 0) return;
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        
        size_t line_idx = document_get_line_of_offset(self->doc, cur->cursor_offset);
        size_t line_start = document_get_offset_of_line(self->doc, line_idx);
        size_t char_idx = cur->cursor_offset - line_start;
        
        char *text = NULL; size_t len;
        PangoLayout *layout = create_pango_layout_for_line(self, line_idx, &text, &len);
        if (!layout) continue;

        size_t effective_len = strlen(pango_layout_get_text(layout));

        if (cur->target_x < 0) {
             PangoRectangle strong_pos;
             size_t safe_idx = MIN(char_idx, effective_len);
             pango_layout_get_cursor_pos(layout, (int)safe_idx, &strong_pos, NULL);
             cur->target_x = pango_units_to_double(strong_pos.x);
        }
        
        PangoRectangle cursor_pos;
        size_t safe_idx = MIN(char_idx, effective_len);
        pango_layout_get_cursor_pos(layout, (int)safe_idx, &cursor_pos, NULL);
        int cursor_y_center = cursor_pos.y + (cursor_pos.height / 2);
        
        PangoLayoutIter *iter = pango_layout_get_iter(layout);
        int current_v_line_idx = 0;
        gboolean found_v_line = FALSE;
        do {
            PangoRectangle line_extents;
            pango_layout_iter_get_line_extents(iter, NULL, &line_extents);
            if (cursor_y_center >= line_extents.y && cursor_y_center < line_extents.y + line_extents.height) {
                found_v_line = TRUE;
                break;
            }
            current_v_line_idx++;
        } while (pango_layout_iter_next_line(iter));
        pango_layout_iter_free(iter);
        
        if (!found_v_line) current_v_line_idx = MAX(0, pango_layout_get_line_count(layout) - 1);
        
        int local_delta = visual_lines_delta;
        size_t current_line_idx = line_idx;
        
        while (TRUE) {
            int total_v_lines = pango_layout_get_line_count(layout);
            int target_v_line = current_v_line_idx + local_delta;

            if (target_v_line >= 0 && target_v_line < total_v_lines) {
                iter = pango_layout_get_iter(layout);
                for (int i = 0; i < target_v_line; i++) pango_layout_iter_next_line(iter);
                PangoLayoutLine *v_line = pango_layout_iter_get_line_readonly(iter);
                int index, trailing;
                pango_layout_line_x_to_index(v_line, (int)(cur->target_x * PANGO_SCALE), &index, &trailing);
                size_t ls = document_get_offset_of_line(self->doc, current_line_idx);
                cur->cursor_offset = ls + index + trailing;
                pango_layout_iter_free(iter);
                break;
            } else {
                if (local_delta > 0) {
                     int lines_remaining = total_v_lines - current_v_line_idx - 1;
                     local_delta -= (lines_remaining + 1);
                     size_t next_line = current_line_idx;
                     if (!editor_widget_get_next_visible_line(self, current_line_idx, &next_line)) {
                         // ... (End of file logic)
                         break;
                     }
                     current_line_idx = next_line;
                     g_object_unref(layout); g_free(text);
                     layout = create_pango_layout_for_line(self, current_line_idx, &text, &len);
                     current_v_line_idx = 0;
                } else {
                     int lines_above = current_v_line_idx;
                     local_delta += (lines_above + 1);
                     size_t prev_line = current_line_idx;
                     if (!editor_widget_get_prev_visible_line(self, current_line_idx, &prev_line)) {
                         // ... (Top of file logic)
                         break;
                     }
                     current_line_idx = prev_line;
                     g_object_unref(layout); g_free(text);
                     layout = create_pango_layout_for_line(self, current_line_idx, &text, &len);
                     current_v_line_idx = pango_layout_get_line_count(layout) - 1; 
                }
            }
        }
        g_object_unref(layout);
        g_free(text);
    }
    
    editor_widget_update_im_cursor_location(self);
    {
        size_t line, col;
        editor_widget_get_cursor_position(self, &line, &col);
        g_signal_emit_by_name(self, "cursor-moved", (guint)line, (guint)col);
    }
}


static gboolean
selection_is_whole_word(EditorWidget *self, size_t start, size_t end, size_t total)
{
    if (start >= end || end > total) return FALSE;
    if (start > 0 && is_alt_word_char_at(self->doc, utf8_prev_grapheme(self, start))) return FALSE;
    if (end < total && is_alt_word_char_at(self->doc, end)) return FALSE;

    size_t off = start;
    while (off < end) {
        if (!is_alt_word_char_at(self->doc, off)) return FALSE;
        size_t next = utf8_next_grapheme(self, off);
        if (next <= off) return FALSE;
        off = next;
    }
    return TRUE;
}

void
editor_widget_move_selection_horizontally(EditorWidget *self, int delta)
{
    /* Collapse to single cursor for complex move operations */
    if (self->cursors && self->cursors->len > 1) editor_widget_clear_cursors(self);
    
    size_t total = document_get_length(self->doc);
    size_t s = MIN(self->cursor_offset, self->selection_anchor);
    size_t e = MAX(self->cursor_offset, self->selection_anchor);
    
    /* If no selection, auto-select word at caret precisely as in swap_words */
    if (s == e) {
        self->alt_word_mode = TRUE;
        size_t off = self->cursor_offset;
        if (off > 0 && !is_alt_word_char_at(self->doc, off) && is_alt_word_char_at(self->doc, utf8_prev_grapheme(self, off))) {
            off = utf8_prev_grapheme(self, off);
        }

        if (off >= total || !is_alt_word_char_at(self->doc, off)) return;
        
        s = off;
        while (s > 0 && is_alt_word_char_at(self->doc, utf8_prev_grapheme(self, s)))
            s = utf8_prev_grapheme(self, s);
        e = off;
        while (e < total && is_alt_word_char_at(self->doc, e))
            e = utf8_next_grapheme(self, e);
    } else {
        if (selection_is_whole_word(self, s, e, total)) {
            self->alt_word_mode = TRUE;
        } else {
            self->alt_word_mode = FALSE;
        }
    }

    if (!self->alt_word_mode) {
        /* Manual selection: move by exactly one character (shift) */
        char *sel_text = document_get_text_range(self->doc, s, e - s);
        if (delta > 0) {
            if (e < total) {
                size_t next_gap = utf8_next_grapheme(self, e);
                size_t gap_len = next_gap - e;
                char *gap_text = document_get_text_range(self->doc, e, gap_len);
                
                document_begin_undo_group(self->doc);
                /* Save actual cursor state (e.g. collapsed) */
                document_set_undo_group_selection(self->doc, self->selection_anchor, self->cursor_offset);
                document_delete(self->doc, s, (e - s) + gap_len);
                document_insert(self->doc, s, gap_text, gap_len);
                document_insert(self->doc, s + gap_len, sel_text, e - s);
                
                size_t new_anchor = s + gap_len;
                size_t new_cursor = new_anchor + (e - s);
                document_set_redo_group_selection(self->doc, new_anchor, new_cursor);
                document_end_undo_group(self->doc);
                
                self->selection_anchor = new_anchor;
                self->cursor_offset = new_cursor;
                g_free(gap_text);
            }
        } else {
            if (s > 0) {
                size_t prev_gap = utf8_prev_grapheme(self, s);
                size_t gap_len = s - prev_gap;
                char *gap_text = document_get_text_range(self->doc, prev_gap, gap_len);
                
                document_begin_undo_group(self->doc);
                /* Save actual cursor state */
                document_set_undo_group_selection(self->doc, self->selection_anchor, self->cursor_offset);
                document_delete(self->doc, prev_gap, gap_len + (e - s));
                document_insert(self->doc, prev_gap, sel_text, e - s);
                document_insert(self->doc, prev_gap + (e - s), gap_text, gap_len);
                
                size_t new_anchor = prev_gap;
                size_t new_cursor = new_anchor + (e - s);
                document_set_redo_group_selection(self->doc, new_anchor, new_cursor);
                document_end_undo_group(self->doc);
                
                self->selection_anchor = new_anchor;
                self->cursor_offset = new_cursor;
                g_free(gap_text);
            }
        }
        g_free(sel_text);
        update_target_x(self);
        
        /* Sync primary cursor */
        EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);
        primary->cursor_offset = self->cursor_offset;
        primary->selection_anchor = self->selection_anchor;
        return;
    }
    
    size_t sel_len = e - s;
    if (delta > 0) {
        /* Move Right: [SEL][SEP][W2] -> [W2][SEP][SEL] */
        size_t sep_start = e;
        size_t sep_end = sep_start;
        while (sep_end < total && !is_alt_word_char_at(self->doc, sep_end))
            sep_end = utf8_next_grapheme(self, sep_end);
            
        if (sep_end >= total) return;
        
        size_t w2_start = sep_end;
        size_t w2_end = w2_start;
        while (w2_end < total && is_alt_word_char_at(self->doc, w2_end))
            w2_end = utf8_next_grapheme(self, w2_end);
            
        size_t w2_len = w2_end - w2_start;
        size_t sep_len = w2_start - e;
        
        char *sel_text = document_get_text_range(self->doc, s, sel_len);
        char *sep_text = document_get_text_range(self->doc, e, sep_len);
        char *w2_text = document_get_text_range(self->doc, w2_start, w2_len);
        
        document_begin_undo_group(self->doc);
        /* Save actual cursor state */
        document_set_undo_group_selection(self->doc, self->selection_anchor, self->cursor_offset);
        document_delete(self->doc, s, sel_len + sep_len + w2_len);
        document_insert(self->doc, s, w2_text, w2_len);
        document_insert(self->doc, s + w2_len, sep_text, sep_len);
        document_insert(self->doc, s + w2_len + sep_len, sel_text, sel_len);
        
        size_t new_anchor = s + w2_len + sep_len;
        size_t new_cursor = new_anchor + sel_len;
        document_set_redo_group_selection(self->doc, new_anchor, new_cursor);
        document_end_undo_group(self->doc);
        
        self->selection_anchor = new_anchor;
        self->cursor_offset = new_cursor;
        
        g_free(sel_text); g_free(sep_text); g_free(w2_text);
    } else {
        /* Move Left: [W2][SEP][SEL] -> [SEL][SEP][W2] */
        size_t sep_end = s;
        size_t sep_start = sep_end;
        while (sep_start > 0 && !is_alt_word_char_at(self->doc, utf8_prev_grapheme(self, sep_start)))
            sep_start = utf8_prev_grapheme(self, sep_start);
            
        if (sep_start == 0 && !is_alt_word_char_at(self->doc, 0)) return;
        
        size_t w2_end = sep_start;
        size_t w2_start = w2_end;
        while (w2_start > 0 && is_alt_word_char_at(self->doc, utf8_prev_grapheme(self, w2_start)))
            w2_start = utf8_prev_grapheme(self, w2_start);
            
        if (w2_start == w2_end) return;
        
        size_t w2_len = w2_end - w2_start;
        size_t sep_len = s - w2_end;
        
        char *w2_text = document_get_text_range(self->doc, w2_start, w2_len);
        char *sep_text = document_get_text_range(self->doc, w2_end, sep_len);
        char *sel_text = document_get_text_range(self->doc, s, sel_len);
        
        document_begin_undo_group(self->doc);
        /* Save actual cursor state */
        document_set_undo_group_selection(self->doc, self->selection_anchor, self->cursor_offset);
        document_delete(self->doc, w2_start, w2_len + sep_len + sel_len);
        document_insert(self->doc, w2_start, sel_text, sel_len);
        document_insert(self->doc, w2_start + sel_len, sep_text, sep_len);
        document_insert(self->doc, w2_start + sel_len + sep_len, w2_text, w2_len);
        
        size_t new_anchor = w2_start;
        size_t new_cursor = w2_start + sel_len;
        document_set_redo_group_selection(self->doc, new_anchor, new_cursor);
        document_end_undo_group(self->doc);
        
        self->selection_anchor = new_anchor;
        self->cursor_offset = new_cursor;
        
        g_free(sel_text); g_free(sep_text); g_free(w2_text);
    }
    update_target_x(self);
    /* Sync primary cursor */
    EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);
    primary->cursor_offset = self->cursor_offset;
    primary->selection_anchor = self->selection_anchor;
}

void
editor_widget_move_lines_vertically(EditorWidget *self, int delta)
{
    /* Collapse to single cursor for complex move operations */
    if (self->cursors && self->cursors->len > 1) editor_widget_clear_cursors(self);

    size_t start_off = MIN(self->cursor_offset, self->selection_anchor);
    size_t end_off = MAX(self->cursor_offset, self->selection_anchor);
    
    size_t start_line = document_get_line_of_offset(self->doc, start_off);
    size_t end_line = document_get_line_of_offset(self->doc, end_off);
    
    /* If selection ends exactly at start of next line, don't include that line */
    if (end_off > start_off && end_off == document_get_offset_of_line(self->doc, end_line) && end_line > start_line) {
        end_line--;
    }
    
    size_t total_lines = document_get_line_count(self->doc);
    document_begin_undo_group(self->doc);
    /* Save selection state (moved lines) */
    document_set_undo_group_selection(self->doc, self->selection_anchor, self->cursor_offset);
    
    if (delta < 0 && start_line > 0) {
        /* Move Up: Swap [prev_line] with [range] */
        size_t prev_line = start_line - 1;
        size_t prev_start = document_get_offset_of_line(self->doc, prev_line);
        size_t range_start = document_get_offset_of_line(self->doc, start_line);
        size_t range_end = (end_line + 1 < total_lines) ? document_get_offset_of_line(self->doc, end_line + 1) : document_get_length(self->doc);
        
        size_t prev_len = range_start - prev_start;
        size_t range_len = range_end - range_start;
        
        char *prev_text = document_get_text_range(self->doc, prev_start, prev_len);
        char *range_text = document_get_text_range(self->doc, range_start, range_len);
        
        /* If range is at EOF and has no newline, but will be moved up, it MUST have a newline */
        gboolean range_needs_newline = (range_len > 0 && range_text[range_len-1] != '\n');
        
        document_delete(self->doc, prev_start, prev_len + range_len);
        
        size_t current_pos = prev_start;
        document_insert(self->doc, current_pos, range_text, range_len);
        current_pos += range_len;
        if (range_needs_newline) {
            document_insert(self->doc, current_pos, "\n", 1);
            current_pos += 1;
        }
        document_insert(self->doc, current_pos, prev_text, prev_len);
        
        self->selection_anchor = prev_start;
        self->cursor_offset = prev_start + range_len + (range_needs_newline ? 1 : 0);
        
        g_free(prev_text);
        g_free(range_text);
    } else if (delta > 0 && end_line + 1 < total_lines) {
        /* Move Down: Swap [range] with [next_line] */
        size_t next_line = end_line + 1;
        size_t range_start = document_get_offset_of_line(self->doc, start_line);
        size_t range_end = document_get_offset_of_line(self->doc, next_line);
        size_t next_end = (next_line + 1 < total_lines) ? document_get_offset_of_line(self->doc, next_line + 1) : document_get_length(self->doc);
        
        size_t range_len = range_end - range_start;
        size_t next_len = next_end - range_end;
        
        char *range_text = document_get_text_range(self->doc, range_start, range_len);
        char *next_text = document_get_text_range(self->doc, range_end, next_len);
        
        /* If next_line is at EOF and has no newline, but will be moved up, it MUST have a newline */
        gboolean next_needs_newline = (next_len > 0 && next_text[next_len-1] != '\n');

        document_delete(self->doc, range_start, range_len + next_len);
        
        size_t current_pos = range_start;
        document_insert(self->doc, current_pos, next_text, next_len);
        current_pos += next_len;
        if (next_needs_newline) {
            document_insert(self->doc, current_pos, "\n", 1);
            current_pos += 1;
        }
        document_insert(self->doc, current_pos, range_text, range_len);
        
        self->selection_anchor = range_start + next_len + (next_needs_newline ? 1 : 0);
        self->cursor_offset = self->selection_anchor + range_len;
        
        g_free(range_text);
        g_free(next_text);
    }
    
    document_end_undo_group(self->doc);
    
    update_target_x(self);
    /* Sync primary cursor */
    EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);
    primary->cursor_offset = self->cursor_offset;
    primary->selection_anchor = self->selection_anchor;
}

void
editor_widget_indent_selection(EditorWidget *self)
{
    if (!self->doc || !self->cursors) return;

    /* Collect all unique lines to indent */
    GArray *lines = g_array_new(FALSE, FALSE, sizeof(size_t));
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        
        size_t start_line = document_get_line_of_offset(self->doc, start);
        size_t end_line = document_get_line_of_offset(self->doc, end);
        
        if (end > start && end == document_get_offset_of_line(self->doc, end_line)) {
            if (end_line > start_line) end_line--;
        }
        
        for (size_t l = start_line; l <= end_line; l++) {
            g_array_append_val(lines, l);
        }
    }
    
    if (lines->len == 0) {
        g_array_free(lines, TRUE);
        return;
    }
    
    g_array_sort(lines, compare_size_t);
    
    /* Remove duplicates */
    for (guint i = 0; i < lines->len - 1; ) {
        size_t l1 = g_array_index(lines, size_t, i);
        size_t l2 = g_array_index(lines, size_t, i+1);
        if (l1 == l2) {
            g_array_remove_index(lines, i+1);
        } else {
            i++;
        }
    }
    
    /* Prepare indentation string */
    char *indent_str;
    size_t indent_len;
    if (self->indent_style == 0) {
        indent_len = self->indent_width;
        indent_str = g_malloc(indent_len + 1);
        memset(indent_str, ' ', indent_len);
        indent_str[indent_len] = '\0';
    } else {
        indent_len = 1;
        indent_str = g_strdup("\t");
    }
    
    document_begin_undo_group(self->doc);
    
    for (int i = lines->len - 1; i >= 0; i--) {
        size_t line_idx = g_array_index(lines, size_t, i);
        size_t line_start = document_get_offset_of_line(self->doc, line_idx);
        
        document_insert(self->doc, line_start, indent_str, indent_len);
        
        /* Update all cursors after this point */
        for (guint c = 0; c < self->cursors->len; c++) {
            EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
            /* If cursor is at or after insertion, shift it */
            if (cur->cursor_offset >= line_start) cur->cursor_offset += indent_len;
            if (cur->selection_anchor >= line_start) cur->selection_anchor += indent_len;
        }
    }
    
    document_end_undo_group(self->doc);
    g_array_free(lines, TRUE);
    g_free(indent_str);
}

void
editor_widget_unindent_selection(EditorWidget *self)
{
    if (!self->doc || !self->cursors) return;

    /* Collect all unique lines to unindent */
    GArray *lines = g_array_new(FALSE, FALSE, sizeof(size_t));
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        
        size_t start_line = document_get_line_of_offset(self->doc, start);
        size_t end_line = document_get_line_of_offset(self->doc, end);
        
        if (end > start && end == document_get_offset_of_line(self->doc, end_line)) {
            if (end_line > start_line) end_line--;
        }
        
        for (size_t l = start_line; l <= end_line; l++) {
            g_array_append_val(lines, l);
        }
    }
    
    if (lines->len == 0) {
        g_array_free(lines, TRUE);
        return;
    }
    
    g_array_sort(lines, compare_size_t);
    /* Remove duplicates */
    for (guint i = 0; i < lines->len - 1; ) {
        size_t l1 = g_array_index(lines, size_t, i);
        size_t l2 = g_array_index(lines, size_t, i+1);
        if (l1 == l2) {
            g_array_remove_index(lines, i+1);
        } else {
            i++;
        }
    }
    
    document_begin_undo_group(self->doc);
    
    for (int i = lines->len - 1; i >= 0; i--) {
        size_t line_idx = g_array_index(lines, size_t, i);
        size_t line_off = document_get_offset_of_line(self->doc, line_idx);
        
        size_t len;
        char *line_text = document_get_line(self->doc, line_idx, &len);
        
        gboolean can_unindent = FALSE;
        size_t delete_len = 0;
        
        if (len > 0) {
            if (line_text[0] == '\t') {
                can_unindent = TRUE;
                delete_len = 1;
            } else if (line_text[0] == ' ') {
                size_t spaces = 0;
                while (spaces < self->indent_width && spaces < len && line_text[spaces] == ' ') {
                    spaces++;
                }
                if (spaces > 0) {
                    can_unindent = TRUE;
                    delete_len = spaces;
                }
            }
        }
        g_free(line_text);
        
        if (can_unindent) {
            document_delete(self->doc, line_off, delete_len);
            
            /* Update cursors */
            for (guint c = 0; c < self->cursors->len; c++) {
                EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                if (cur->cursor_offset > line_off) {
                    cur->cursor_offset -= MIN(cur->cursor_offset - line_off, delete_len);
                }
                if (cur->selection_anchor > line_off) {
                    cur->selection_anchor -= MIN(cur->selection_anchor - line_off, delete_len);
                }
            }
        }
    }
    
    document_end_undo_group(self->doc);
    g_array_free(lines, TRUE);
}

void
editor_widget_backspace(EditorWidget *self)
{
    if (!self->cursors || self->cursors->len == 0) return;
    
    /* Sort cursors descending to avoid index shifting issues */
    g_array_sort(self->cursors, compare_cursors_desc);
    
    document_begin_undo_group(self->doc);
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        
        long delta = 0;
        
        if (start != end) {
             /* Delete selection */
             document_delete(self->doc, start, end - start);
             cur->cursor_offset = start;
             cur->selection_anchor = start;
             delta -= (long)(end - start);
        } else {
             /* Delete char before - UTF-8 Aware */
             if (cur->cursor_offset > 0) {
                 size_t lookback = (cur->cursor_offset >= 6) ? 6 : cur->cursor_offset;
                 char *prev_text = document_get_text_range(self->doc, cur->cursor_offset - lookback, lookback);
                 
                 if (prev_text) {
                     const char *end = prev_text + lookback;
                     const char *p = end - 1;
                     
                     while (p > prev_text && (*p & 0xC0) == 0x80) {
                         p--;
                     }
                     
                     size_t char_len = end - p;
                     document_delete(self->doc, cur->cursor_offset - char_len, char_len);
                     
                     cur->cursor_offset -= char_len;
                     cur->selection_anchor = cur->cursor_offset;
                     delta = -(long)char_len;
                     g_free(prev_text);
                 } else {
                     document_delete(self->doc, cur->cursor_offset - 1, 1);
                     cur->cursor_offset--;
                     cur->selection_anchor = cur->cursor_offset;
                     delta = -1;
                 }
             }
        }
        
        if (delta != 0) {
            for (guint j = 0; j < c; j++) {
                EditorCursor *prev = &g_array_index(self->cursors, EditorCursor, j);
                if ((long)prev->cursor_offset + delta < 0) prev->cursor_offset = 0;
                else prev->cursor_offset += delta;
                
                if ((long)prev->selection_anchor + delta < 0) prev->selection_anchor = 0;
                else prev->selection_anchor += delta;
            }
        }
    }
    
    document_end_undo_group(self->doc);
    
    editor_widget_reset_cursor_blink(self);
    editor_widget_update_adjustments(self, -1, -1);
    scroll_to_cursor(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
editor_widget_delete(EditorWidget *self)
{
    if (!self->cursors || self->cursors->len == 0) return;
    
    g_array_sort(self->cursors, compare_cursors_desc);
    
    document_begin_undo_group(self->doc);
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        
        long delta = 0;
        
        if (start != end) {
             document_delete(self->doc, start, end - start);
             cur->cursor_offset = start;
             cur->selection_anchor = start;
             delta -= (long)(end - start);
        } else {
             size_t total = document_get_length(self->doc);
             if (cur->cursor_offset < total) {
                 char *next_bytes = document_get_text_range(self->doc, cur->cursor_offset, 1);
                 
                 if (next_bytes) {
                     unsigned char c = (unsigned char)next_bytes[0];
                     size_t char_len = 1;
                     
                     if ((c & 0xF8) == 0xF0) char_len = 4;
                     else if ((c & 0xF0) == 0xE0) char_len = 3;
                     else if ((c & 0xE0) == 0xC0) char_len = 2;
                     
                     g_free(next_bytes);
                     
                     if (cur->cursor_offset + char_len > total) {
                         char_len = total - cur->cursor_offset;
                     }
                     
                     document_delete(self->doc, cur->cursor_offset, char_len);
                     delta = -(long)char_len;
                 }
             }
        }
        
        if (delta != 0) {
            for (guint j = 0; j < c; j++) {
                EditorCursor *prev = &g_array_index(self->cursors, EditorCursor, j);
                if ((long)prev->cursor_offset + delta < 0) prev->cursor_offset = 0;
                else prev->cursor_offset += delta;
                
                if ((long)prev->selection_anchor + delta < 0) prev->selection_anchor = 0;
                else prev->selection_anchor += delta;
            }
        }
    }
    
    document_end_undo_group(self->doc);
    
    editor_widget_reset_cursor_blink(self);
    editor_widget_update_adjustments(self, -1, -1);
    scroll_to_cursor(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
editor_widget_undo(EditorWidget *self)
{
    if (!self->doc || self->undo_redo_task) return;
    
    /* Ensure any active typing group is committed before undoing */
    editor_widget_finish_typing_undo_group(self);
    
    self->undo_redo_task = document_undo_async(self->doc, 
        (UndoRedoProgressCallback)editor_widget_on_undo_redo_progress, self);
    
    if (self->undo_redo_task) {
        g_signal_emit_by_name(self, "undo-redo-progress", 0.0, TRUE);
    }
}

void
editor_widget_redo(EditorWidget *self)
{
    if (!self->doc || self->undo_redo_task) return;
    
    self->undo_redo_task = document_redo_async(self->doc, 
        (UndoRedoProgressCallback)editor_widget_on_undo_redo_progress, self);
    
    if (self->undo_redo_task) {
        g_signal_emit_by_name(self, "undo-redo-progress", 0.0, TRUE);
    }
}

void
editor_widget_select_all(EditorWidget *self)
{
    if (!self->doc || !self->cursors) return;
    
    editor_widget_clear_cursors(self);
    EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);
    
    size_t total = document_get_length(self->doc);
    primary->selection_anchor = 0;
    primary->cursor_offset = total;
    
    self->cursor_offset = primary->cursor_offset;
    self->selection_anchor = primary->selection_anchor;
    
    self->alt_word_mode = TRUE;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void editor_widget_change_case(EditorWidget *self, int type)
{
    if (!self->doc || self->undo_redo_task) return;
    
    EditorCursor *primary = editor_widget_get_primary_cursor(self);
    if (!primary) return;
    
    size_t start = MIN(primary->cursor_offset, primary->selection_anchor);
    size_t end = MAX(primary->cursor_offset, primary->selection_anchor);
    
    if (start == end) return;
    
    size_t len = end - start;
    
    /* Optimization: For small selections (< 1MB), perform in-memory replacement 
     * to avoid rewriting the entire file (which causes freezing on huge files). */
    if (len < 1024 * 1024) {
        char *text = document_get_text_range(self->doc, start, len);
        if (!text) return;
        
        gboolean new_word = TRUE; /* For Title Case */
        
        for (size_t i = 0; i < len; i++) {
            char c = text[i];
            char r = c;
            
            if (type == CHANGE_CASE_TITLE) {
                if (g_ascii_isalpha(c)) {
                    r = new_word ? g_ascii_toupper(c) : g_ascii_tolower(c);
                    new_word = FALSE;
                } else if (g_ascii_isspace(c) || g_ascii_ispunct(c)) {
                    new_word = TRUE;
                }
            } else if (type == CHANGE_CASE_INVERT) {
                if (g_ascii_isupper(c)) r = g_ascii_tolower(c);
                else if (g_ascii_islower(c)) r = g_ascii_toupper(c);
            } else if (type == CHANGE_CASE_LOWER) {
                r = g_ascii_tolower(c);
            } else if (type == CHANGE_CASE_UPPER) {
                r = g_ascii_toupper(c);
            }
            
            text[i] = r;
        }
        
        document_begin_undo_group(self->doc);
        document_delete(self->doc, start, len);
        document_insert(self->doc, start, text, len);
        
        /* Restore selection */
        EditorCursor *cur = editor_widget_get_primary_cursor(self);
        if (cur) {
            /* If we were selecting forward (anchor < offset), restore that direction */
            if (primary->selection_anchor <= primary->cursor_offset) {
                cur->selection_anchor = start;
                cur->cursor_offset = start + len;
            } else {
                cur->selection_anchor = start + len;
                cur->cursor_offset = start;
            }
        }
        
        document_end_undo_group(self->doc);
        g_free(text);
        
        editor_widget_update_adjustments(self, -1, -1);
        scroll_to_cursor(self);
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return;
    }
    
    /* Fallback for huge selections: Streaming rewrite */
    CharTransformFunc func = NULL;
    if (type == CHANGE_CASE_LOWER) func = g_ascii_tolower;
    else if (type == CHANGE_CASE_UPPER) func = g_ascii_toupper;
    
    StreamingChangeCaseTask *task = document_change_case_streaming_start(self->doc, start, end, func, type, 
                                          (ReplaceProgressCallback)editor_widget_on_change_case_progress, self);
                                          
    if (!task) {
        /* If task is NULL, likely due to disk space check failure */
        AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Operation Failed", "Insufficient disk space to perform this operation safely."));
        adw_alert_dialog_add_response(dialog, "ok", "OK");
        adw_alert_dialog_choose(dialog, GTK_WIDGET(self), NULL, NULL, NULL);
    }
}
