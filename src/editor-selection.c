#include "editor-internal.h"
#include <string.h>
#include <math.h>

/* Selection Helpers */
static inline gboolean
debug_hittest_enabled(void)
{
    const char *env = g_getenv("VITE_DEBUG_HITTEST");
    return env && *env && strcmp(env, "0") != 0;
}

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
    size_t count = get_visual_line_count(self);
    
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
        if (count > 0 && start_line >= count) start_line = count - 1;
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
    size_t max_lines = get_visual_line_count(self);
    PangoContext *context = gtk_widget_get_pango_context(GTK_WIDGET(self));
    int width = get_stable_width(self);
    int height = gtk_widget_get_height(GTK_WIDGET(self));
    double text_start_x = get_effective_gutter_width(self) + self->padding_left;

    double scroll_x = 0;
    if (self->hadjustment)
        scroll_x = gtk_adjustment_get_value(self->hadjustment);

    /* Pre-calculate Tab Array for hit testing */
    PangoTabArray *tab_array = NULL;
    if (self->tab_width > 0) {
        PangoFontMetrics *metrics = pango_context_get_metrics(context, self->font_desc, NULL);
        int char_width = pango_font_metrics_get_approximate_char_width(metrics);
        pango_font_metrics_unref(metrics);
        
        int tab_width_pango = char_width * self->tab_width;
        tab_array = pango_tab_array_new(1, FALSE); /* Pango Units */
        pango_tab_array_set_tab(tab_array, 0, PANGO_TAB_LEFT, tab_width_pango);
    }

    size_t line_idx = start_line;
    
    /* Handle clicks above the visible content area */
    if (click_y < current_y) {
        if (start_line == 0) {
            *out_offset = 0;
        } else {
            size_t phys_line = get_physical_line_index(self, start_line);
            if (phys_line == (size_t)-1) {
                *out_offset = 0;
            } else {
                *out_offset = document_get_offset_of_line(self->doc, phys_line);
            }
        }
        return;
    }
    
    /* Limit scan to reasonable screen height + buffer */
    while (line_idx < max_lines) {
        size_t phys_line = get_physical_line_index(self, line_idx);
        if (phys_line == (size_t)-1) {
            line_idx++;
            continue;
        }
        size_t len;
        char *text = document_get_line_truncated(self->doc, phys_line, &len, MAX_PANGO_LINE_LEN + 1024, NULL);
        
        if (!g_utf8_validate(text, len, NULL)) {
             char *safe = g_utf8_make_valid(text, len);
             g_free(text); text = safe; len = strlen(text);
        }
        
        size_t full_len = document_get_line_length(self->doc, phys_line);
        gboolean is_virtualized = FALSE;
        size_t chunk_padding = 0;
        double render_x_offset = 0;

        char *orig_text = NULL; /* To keep track if we swapped text for virtualization */

        if (full_len > 4096) {
             is_virtualized = TRUE;
             if (!self->wrap_lines) {
                 double cw = self->cached_char_width > 1.0 ? self->cached_char_width : 8.0; 
                 
                 double start_char_visual = (scroll_x / cw) - 100.0;
                 if (start_char_visual < 0) start_char_visual = 0;
                 
                 size_t start_byte_approx = (size_t)start_char_visual;
                 if (start_byte_approx > full_len) start_byte_approx = full_len;

                 size_t visible_chars = (size_t)(width / cw) + 300; 
                 size_t safe_len = visible_chars;
                 if (start_byte_approx + safe_len > full_len) safe_len = full_len - start_byte_approx;
                 
                 char *chunk_text = document_get_text_range(self->doc, document_get_offset_of_line(self->doc, phys_line) + start_byte_approx, safe_len);
                 
                 /* Swap for layout */
                 orig_text = text; /* Free original truncated fetch */
                 text = chunk_text;
                 len = safe_len;
                 chunk_padding = start_byte_approx;
                 render_x_offset = start_byte_approx * cw;
                 
                 /* UTF-8 Clean */
                 if (len > 0 && !g_utf8_validate(text, len, NULL)) {
                     char *safe_chunk = g_utf8_make_valid(text, len);
                     g_free(text);
                     text = safe_chunk;
                     len = strlen(text);
                 }
             }
        }

        while (len > 0 && (text[len-1] == '\n' || text[len-1] == '\r')) {
            len--;
        }
        
        PangoLayout *layout = pango_layout_new(context);
        pango_layout_set_font_description(layout, self->font_desc);
        pango_layout_set_text(layout, text, (len > MAX_PANGO_LINE_LEN) ? MAX_PANGO_LINE_LEN : (int)len);
        
        double virtual_height = 0;
        gboolean is_massive_wrapped = FALSE;

        if (self->wrap_lines && !is_virtualized) {
            /* Minimap Width Calculation for Wrap */
            double minimap_w = 0;
            if (self->minimap_enabled) {
                minimap_w = self->minimap_width;
                if (minimap_w > width / 2) minimap_w = width / 2;
            }

            int available_w = (int)((double)width - text_start_x - (double)self->active_right_padding - minimap_w); /* Buffer + Minimap */
            if (available_w < 50) available_w = 50; 
            pango_layout_set_width(layout, available_w * PANGO_SCALE);
            pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        } else if (self->wrap_lines && is_virtualized) {
            /* Massive line with wrap: logic in 'else' block below normally sets width -1 (NO WRAP).
               This causes the scan loop to think height is 1 row.
               We must compute REAL virtual height here to advance current_y correctly.
                we must compute REAL virtual height here to advance current_y correctly.
            */
            is_massive_wrapped = TRUE;
            double cw = self->cached_char_width > 1.0 ? self->cached_char_width : 8.0; 
            
            double minimap_w = 0;
            if (self->minimap_enabled) {
                minimap_w = self->minimap_width;
                if (minimap_w > width / 2) minimap_w = width / 2;
            }
            
            int available_w = (int)((double)width - text_start_x - (double)self->active_right_padding - minimap_w); 
            if (available_w < 50) available_w = 50;
            int chars_per_line = (int)((double)available_w / cw);
            if (chars_per_line < 1) chars_per_line = 1;

            size_t rows = (full_len + chars_per_line - 1) / chars_per_line;
            virtual_height = (double)rows * self->line_height;

            /* Set layout to something valid but small to avoid cost? 
               Actually we don't use layout for hit test in this branch (if we hit), 
               we use the grid logic in the check block below.
            */
            pango_layout_set_width(layout, -1); /* Dummy */
        } else {
            pango_layout_set_width(layout, -1);
            pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        }
        
        if (tab_array) pango_layout_set_tabs(layout, tab_array);
        
        int h;
        pango_layout_get_pixel_size(layout, NULL, &h);
        double line_h = (double)h;
        if (is_massive_wrapped) line_h = virtual_height; /* Override Pango height */
        
        if (line_h < self->line_height) line_h = self->line_height;
        
        /* Check if click falls within this line */
        if (click_y >= current_y && click_y < current_y + line_h) {
            /* Found it! */
            int idx, trailing;
            double local_x = (x - text_start_x) + scroll_x;
            if (is_virtualized && !self->wrap_lines) {
                local_x -= render_x_offset;
            }
            if (local_x < 0) local_x = 0;
            
            pango_layout_xy_to_index(layout,
                                     (int)(local_x * PANGO_SCALE),
                                     (int)((click_y - current_y) * PANGO_SCALE),
                                     &idx, &trailing);
            
            size_t line_start = document_get_offset_of_line(self->doc, phys_line);
            
            if (is_virtualized && self->wrap_lines) {
                /* Vertical Virtualization Hit Test (viewport chunk) */
                double cw = self->cached_char_width > 1.0 ? self->cached_char_width : 8.0;
                
                double minimap_w = 0;
                if (self->minimap_enabled) {
                    minimap_w = self->minimap_width;
                    if (minimap_w > width / 2) minimap_w = width / 2;
                }

                int available_w = width - text_start_x - self->active_right_padding - (int)minimap_w;
                if (available_w < 50) available_w = 50;
                int chars_per_line = (int)((double)available_w / cw);
                if (chars_per_line < 1) chars_per_line = 1;

                double relative_y = click_y - current_y;
                int row_in_line = (int)(relative_y / self->line_height);
                if (row_in_line < 0) row_in_line = 0;

                /* Compute viewport chunk start row (same logic as renderer) */
                double line_doc_y = current_y + scroll_y;
                size_t start_row = 0;
                if (scroll_y > line_doc_y) {
                    start_row = (size_t)((scroll_y - line_doc_y) / self->line_height);
                }

                size_t row_in_chunk = 0;
                if ((size_t)row_in_line >= start_row) {
                    row_in_chunk = (size_t)row_in_line - start_row;
                }

                size_t visible_rows = (size_t)(height / self->line_height) + 2;
                if (visible_rows < 1) visible_rows = 1;

                size_t start_char_idx = start_row * (size_t)chars_per_line;
                if (start_char_idx > full_len) start_char_idx = full_len;

                size_t line_start = document_get_offset_of_line(self->doc, phys_line);
                size_t start_off = line_start + start_char_idx;
                if (start_off > line_start) {
                    start_off = utf8_prev_grapheme(self, start_off);
                }
                start_char_idx = start_off - line_start;

                size_t max_len = (full_len > start_char_idx) ? (full_len - start_char_idx) : 0;
                size_t chars_to_fetch = visible_rows * (size_t)chars_per_line + 100;
                size_t safe_len = (chars_to_fetch < max_len) ? chars_to_fetch : max_len;

                char *chunk_text = document_get_text_range(self->doc, start_off, safe_len);
                size_t chunk_len = safe_len;
                if (!chunk_text) {
                    *out_offset = line_start;
                } else {
                    if (chunk_len > 0 && !g_utf8_validate(chunk_text, chunk_len, NULL)) {
                        char *safe = g_utf8_make_valid(chunk_text, chunk_len);
                        g_free(chunk_text);
                        chunk_text = safe;
                        chunk_len = strlen(chunk_text);
                    }

                    /* Accurate hit test using a layout for the visible chunk */
                    PangoLayout *chunk_layout = pango_layout_new(context);
                    pango_layout_set_font_description(chunk_layout, self->font_desc);
                    if (tab_array) pango_layout_set_tabs(chunk_layout, tab_array);
                    pango_layout_set_width(chunk_layout, available_w * PANGO_SCALE);
                    pango_layout_set_wrap(chunk_layout, PANGO_WRAP_CHAR);
                    pango_layout_set_text(chunk_layout, chunk_text, (int)chunk_len);

                    double relative_x = local_x;
                    double relative_y = click_y - current_y - ((double)start_row * self->line_height);
                    if (relative_x < 0) relative_x = 0;
                    if (relative_y < 0) relative_y = 0;

                    int idx2 = 0, trailing2 = 0;
                    gboolean ok = pango_layout_xy_to_index(chunk_layout,
                                                           (int)(relative_x * PANGO_SCALE),
                                                           (int)(relative_y * PANGO_SCALE),
                                                           &idx2, &trailing2);
                    if (debug_hittest_enabled()) {
                        g_printerr("[HITTEST] line=%zu full_len=%zu start_row=%zu row_in_line=%d row_in_chunk=%zu rel_x=%.2f rel_y=%.2f idx=%d trailing=%d ok=%d\\n",
                                   phys_line, full_len, start_row, row_in_line, row_in_chunk, relative_x, relative_y, idx2, trailing2, ok);
                    }

                    if (idx2 < 0) idx2 = 0;
                    if ((size_t)idx2 > chunk_len) idx2 = (int)chunk_len;

                    size_t byte_off = (size_t)idx2;
                    if (trailing2 > 0 && (size_t)idx2 < chunk_len) {
                        const char *ptr = chunk_text + idx2;
                        ptr = g_utf8_next_char(ptr);
                        byte_off = (size_t)(ptr - chunk_text);
                        if (byte_off > chunk_len) byte_off = chunk_len;
                    }

                    *out_offset = line_start + start_char_idx + byte_off;
                    g_object_unref(chunk_layout);
                    g_free(chunk_text);
                }

                g_object_unref(layout);
                g_free(text);
                if (tab_array) pango_tab_array_free(tab_array);
                if (orig_text) g_free(orig_text);
                return;
            } else {
                /* Standard Pango Hit Test (for normal lines or Horizontal Virtualization) */
                /* For Horizontal Virt, we adjusted local_x with render_x_offset earlier. */
                *out_offset = line_start + chunk_padding + idx;
            }
            
             /* Move forward if trailing */
            if (trailing > 0) {
                const char *ptr = text + idx;
                if ((size_t)idx < len) {
                   ptr = g_utf8_next_char(ptr);
                   *out_offset = line_start + chunk_padding + (ptr - text);
                }
            }
            if (*out_offset > document_get_length(self->doc)) *out_offset = document_get_length(self->doc);
            
            g_object_unref(layout);
            g_free(text);
            if (tab_array) pango_tab_array_free(tab_array);
            return;
        }
        
        current_y += line_h;
        g_object_unref(layout);
        g_free(text);
        if (orig_text) g_free(orig_text);
        line_idx++;
        
         /* Safety break if we scanned way past screen */
        if (current_y > click_y + 1000) break;
    }
    
    /* Fallback if not found (clicked below content?): return end of last scanned line */
    if (line_idx > 0) line_idx--;
    size_t phys_line = get_physical_line_index(self, line_idx);
    if (phys_line == (size_t)-1) phys_line = 0;
    *out_offset = document_get_offset_of_line(self->doc, phys_line);
    size_t last_len;
    char *ltext = document_get_line(self->doc, phys_line, &last_len);
    g_free(ltext);
    *out_offset += last_len;
    if (tab_array) pango_tab_array_free(tab_array);
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
