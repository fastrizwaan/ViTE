#include "editor-internal.h"
#include <string.h>
#include <math.h>

/* Selection Helpers */

void
editor_widget_get_offset_at_point(EditorWidget *self, double x, double y, size_t *out_offset)
{
    // g_print("[DEBUG] get_offset_at_point: x=%f, y=%f\n", x, y); /* Commented out to reduce noise unless clicking */
    if (!self->doc) {
        *out_offset = 0;
        return;
    }
    
    /* Pixel-based scrolling */
    double scroll_y = (self->vadjustment) ? gtk_adjustment_get_value(self->vadjustment) : 0;
    
    /* 1. Calculate Start Line (Same as Snapshot) */
    size_t start_line = 0;
    double partial_y = 0;
    size_t count = document_get_line_count(self->doc);
    
    if (self->line_y_offsets && self->line_y_offsets->len > 0) {
        guint low = 0;
        guint high = self->line_y_offsets->len - 1;
        double *offsets = (double*)self->line_y_offsets->data;
        while (low < high) {
            guint mid = low + (high - low + 1) / 2;
            if (offsets[mid] <= scroll_y) low = mid;
            else high = mid - 1;
        }
        start_line = low;
        if (start_line >= count) start_line = count - 1;
        partial_y = scroll_y - offsets[start_line];
    } else {
        /* Fallback: Statistical */
        double multiplier = (self->wrap_lines) ? self->avg_visual_lines : 1.0;
        if (multiplier < 1.0) multiplier = 1.0;
        start_line = (size_t)(scroll_y / (self->line_height * multiplier));
        
        /* Calculate partial_y for statistical mode - must match snapshot logic */
        partial_y = fmod(scroll_y, self->line_height * multiplier);
        /* Clamp partial_y to valid range */
        if (partial_y > self->line_height * 20) partial_y = 0;
    }

    /* 2. Scan forward visually to find the line at 'y' */
    double current_y = -partial_y; /* Start with calculated offset, matching snapshot logic */
    double click_y = y - self->padding_top; /* Adjust for padding in click space */
    size_t max_lines = document_get_line_count(self->doc);
    PangoContext *context = gtk_widget_get_pango_context(GTK_WIDGET(self));
    int width = gtk_widget_get_width(GTK_WIDGET(self));
    double gutter_w = get_effective_gutter_width(self);
    double text_start_x = gutter_w + self->padding_left;

    double scroll_x = 0;
    if (self->hadjustment)
        scroll_x = gtk_adjustment_get_value(self->hadjustment);

    size_t line_idx = start_line;
    
    /* Handle clicks above the visible content area */
    if (click_y < current_y) {
        if (start_line == 0) {
            *out_offset = 0;
        } else {
            *out_offset = document_get_offset_of_line(self->doc, start_line);
        }
        return;
    }
    
    /* Limit scan to reasonable screen height + buffer */
    while (line_idx < max_lines) {
        size_t len;
        char *text = document_get_line_truncated(self->doc, line_idx, &len, MAX_PANGO_LINE_LEN + 1024);
        
        if (!g_utf8_validate(text, len, NULL)) {
             char *safe = g_utf8_make_valid(text, len);
             g_free(text); text = safe; len = strlen(text);
        }
        while (len > 0 && (text[len-1] == '\n' || text[len-1] == '\r')) {
            len--;
        }
        
        PangoLayout *layout = pango_layout_new(context);
        pango_layout_set_font_description(layout, self->font_desc);
        pango_layout_set_text(layout, text, (len > MAX_PANGO_LINE_LEN) ? MAX_PANGO_LINE_LEN : (int)len);
        
        if (self->wrap_lines) {
            int available_w = width - text_start_x - 20; /* 20px buffer for scrollbar */
            if (available_w < 50) available_w = 50; 
            pango_layout_set_width(layout, available_w * PANGO_SCALE);
            pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        } else {
            pango_layout_set_width(layout, -1);
            pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        }
        
        int h;
        pango_layout_get_pixel_size(layout, NULL, &h);
        double line_h = (double)h;
        if (line_h < self->line_height) line_h = self->line_height;
        
        /* Check if click falls within this line */
        if (click_y >= current_y && click_y < current_y + line_h) {
            /* Found it! */
            int idx, trailing;
            double local_x = (x - text_start_x) + scroll_x;
            if (local_x < 0) local_x = 0;
            
            pango_layout_xy_to_index(layout, 
                                     (int)(local_x * PANGO_SCALE), 
                                     (int)((click_y - current_y) * PANGO_SCALE), 
                                     &idx, &trailing);
            
            size_t line_start = document_get_offset_of_line(self->doc, line_idx);
            *out_offset = line_start + idx;
            
             /* Move forward if trailing */
            if (trailing > 0) {
                const char *ptr = text + idx;
                if ((size_t)idx < len) {
                   ptr = g_utf8_next_char(ptr);
                   *out_offset = line_start + (ptr - text);
                }
            }
            if (*out_offset > document_get_length(self->doc)) *out_offset = document_get_length(self->doc);
            
            g_object_unref(layout);
            g_free(text);
            return;
        }
        
        current_y += line_h;
        g_object_unref(layout);
        g_free(text);
        line_idx++;
        
         /* Safety break if we scanned way past screen */
        if (current_y > click_y + 1000) break;
    }
    
    /* Fallback if not found (clicked below content?): return end of last scanned line */
    if (line_idx > 0) line_idx--;
    *out_offset = document_get_offset_of_line(self->doc, line_idx);
    size_t last_len;
    char *ltext = document_get_line(self->doc, line_idx, &last_len);
    g_free(ltext);
    *out_offset += last_len;
}

void
update_selection_extension(EditorWidget *self, size_t off)
{
    if (self->multi_click_selection) {
        /* Multi-click drag - extend selection while keeping original word/line as minimum */
        size_t current_start = off;
        size_t current_end = off;
        
        if (self->multi_click_mode == 2) {
            /* Word mode with smart segment snapping */
            editor_widget_find_segment_boundary(self, off, &current_start, &current_end);
        } else if (self->multi_click_mode == 3) {
            /* Line mode */
            find_line_at_offset(self->doc, off, &current_start, &current_end);
        }
        
        if (off < self->multi_click_start) {
            /* Dragging before the original selection - extend backwards */
            self->selection_anchor = self->multi_click_end;
            self->cursor_offset = current_start;
        } else if (off > self->multi_click_end) {
            /* Dragging after the original selection - extend forwards */
            self->selection_anchor = self->multi_click_start;
            /* If we are just at the start of the new segment, don't snap to its end yet. 
               This allows granular stops between segments (e.g. End of Space vs Start of Next Word). */
            if (off == current_start) {
                self->cursor_offset = current_start;
            } else {
                self->cursor_offset = current_end;
            }
        } else {
            /* Still within original selection - keep original bounds */
            self->selection_anchor = self->multi_click_start;
            self->cursor_offset = self->multi_click_end;
        }
        self->alt_word_mode = TRUE; /* Dragging a multi-click selection preserves word-like behavior */
    } else {
        /* Normal drag selection - extend selection to current position */
        /* Only update cursor, anchor was set by on_click_pressed */
        self->cursor_offset = off;
        self->alt_word_mode = FALSE; /* Manual drag selection is character-based */
    }
    
    /* Sync back to primary cursor */
    EditorCursor *primary = editor_widget_get_primary_cursor(self);
    if (primary) {
        primary->cursor_offset = self->cursor_offset;
        primary->selection_anchor = self->selection_anchor;
        primary->target_x = -1; /* Reset target x on drag */
    }
}
