#include "editor-internal.h"
#include <gtk/gtk.h>
#include <math.h>
#include <string.h>
#include "syntax.h"
#include "editor-minimap.h"

void
editor_widget_refresh_syntax(EditorWidget *self)
{
    g_return_if_fail(EDITOR_IS_WIDGET(self));
    if (self->syntax_ctx) {
        syntax_context_invalidate_all(self->syntax_ctx);
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

void
editor_widget_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    EditorWidget *self = EDITOR_WIDGET(widget);
    if (!self->doc || self->line_height <= 0) return;

    /* Update theme colors */
    gtk_widget_get_color(widget, &self->color_text);
    /* For bg, we might rely on CSS, but let's default to transparent/handled by window, 
       or fetch 'background-color' if possible? 
       GTK4: use css name. 
    */
    /* For cursor, use text color */
    self->color_cursor = self->color_text;

    editor_widget_ensure_metrics(self);
    
    int width = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);
    
    /* Calculate Minimap Layout */
    double minimap_w = 0;
    if (self->minimap_enabled) {
        minimap_w = self->minimap_width;
        if (minimap_w > width / 2) minimap_w = width / 2; /* Limit to half width */
    }
    double minimap_x = width - minimap_w;

    /* Clip to visible area to prevent drawing over splitters */
    gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(0, 0, (float)width, (float)height));
    
    /* Ensure background is cleared/drawn to prevent drag artifacts */
    GdkRGBA bg_color = {1, 1, 1, 1}; /* Default white */
    gboolean dark_mode_needed = FALSE;
    
    if (self->color_text.red > 0.5 && self->color_text.green > 0.5 && self->color_text.blue > 0.5) {
         /* Text is light -> Background should be dark */
         bg_color = (GdkRGBA){0.11, 0.11, 0.11, 1.0}; /* #1e1e1e approx */
         dark_mode_needed = TRUE;
    }
    
    /* Update global syntax theme if changed or if local tracking differs */
    /* Note: syntax_set_theme_mode updates global static. */
    if (self->last_theme_dark_mode != dark_mode_needed) {
        syntax_set_theme_mode(dark_mode_needed);
        if (self->syntax_ctx) {
            syntax_context_invalidate_cache(self->syntax_ctx);
        }
        self->last_theme_dark_mode = dark_mode_needed;
    }
    
    gtk_snapshot_append_color(snapshot, &bg_color, &GRAPHENE_RECT_INIT(0, 0, (float)width, (float)height));
    
    /* Draw Gutter Background */
    double gutter_w = get_effective_gutter_width(self);
    
    if (self->show_line_numbers) {
        /* dim background */
        GdkRGBA gutter_bg = {0.95, 0.95, 0.95, 1.0}; /* Default light gray */
        /* Check for dark theme approximation - if text is light */
        if (self->color_text.red > 0.5 && self->color_text.green > 0.5 && self->color_text.blue > 0.5) {
             gutter_bg = (GdkRGBA){0.15, 0.15, 0.15, 1.0};
        }
        
        gtk_snapshot_append_color(snapshot, &gutter_bg, &GRAPHENE_RECT_INIT(0, 0, (float)gutter_w, (float)height));
    }

    /* Pixel-based scrolling: start_y is in pixels */
    
    /* Pixel-based scrolling: start_y is in pixels */
    double scroll_y = 0;
    if (self->vadjustment)
        scroll_y = gtk_adjustment_get_value(self->vadjustment);

    double scroll_x = 0;
    if (self->hadjustment)
        scroll_x = gtk_adjustment_get_value(self->hadjustment);



    /* Find start_line using binary search on line_y_offsets */
    size_t start_line = 0;
    double partial_y = 0;
    size_t max_lines = get_visual_line_count(self);
    
    if (self->line_y_offsets && self->line_y_offsets->len > 0) {
        double *offsets = (double*)self->line_y_offsets->data;
        size_t low = 0;
        size_t high = self->line_y_offsets->len - 1;
        
        /* We want index i such that offsets[i] <= scroll_y < offsets[i+1] */
        /* Upper bound search */
        
        while (low < high) {
            size_t mid = low + (high - low + 1) / 2;
            if (offsets[mid] <= scroll_y) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        start_line = low;
        if (start_line >= max_lines && max_lines > 0) start_line = max_lines - 1; /* clamp */
        
        partial_y = scroll_y - offsets[start_line];
    } else {
        /* Fallback: Arithmetic estimation using avg visual lines */
        double multiplier = (self->wrap_lines) ? self->avg_visual_lines : 1.0;
        if (multiplier < 1.0) multiplier = 1.0;
        start_line = (size_t)(scroll_y / (self->line_height * multiplier));
        
        /* Approximate partial Y? */
        /* If we are deep in a statistical zone, partial pixel alignment isn't perfect locally
           because the local lines are measured exactly. 
           We just want to find the rough start line. 
           Pango will layout start_line and we draw it at -partial_y.
        */
        partial_y = fmod(scroll_y, self->line_height * multiplier);
        /* Clamp partial_y to valid range if multiplier > 1? */
        if (partial_y > self->line_height * 20) partial_y = 0; // Avoid huge offset
    }
    
    size_t count_lines = (size_t)(height / self->line_height) + 2;
    size_t phys_total_lines = document_get_line_count(self->doc);
    
    /* SYNTAX CATCH-UP: Ensure state is computed for visible lines */
    size_t target_scan_line = start_line + count_lines;
    if (target_scan_line >= phys_total_lines) target_scan_line = phys_total_lines > 0 ? phys_total_lines - 1 : 0;
    editor_widget_ensure_syntax_state_up_to(self, target_scan_line);

    PangoContext *context = gtk_widget_get_pango_context(widget);
    
    /* Pre-compute cursor line indices to avoid O(N*M) complexity in the render loop */
    guint num_cursors = self->cursors->len;
    size_t *cursor_lines = NULL;
    size_t cursor_lines_stack[16];  /* Stack allocation for common case of few cursors */
    if (num_cursors <= 16) {
        cursor_lines = cursor_lines_stack;
    } else {
        cursor_lines = g_new(size_t, num_cursors);
    }
    for (guint c = 0; c < num_cursors; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        if (self->doc) {
            cursor_lines[c] = document_get_line_of_offset(self->doc, cur->cursor_offset);
        } else {
            cursor_lines[c] = (size_t)-1;
        }
    }


    double current_y_pos = -partial_y; /* Start with calculated offset */

    double text_start_x = gutter_w + self->padding_left;

    /* Pre-calculate Tab Array (Performance optimization) */
    PangoTabArray *tab_array = NULL;
    if (self->tab_width > 0) {
        PangoFontMetrics *metrics = pango_context_get_metrics(context, self->font_desc, NULL);
        int char_width = pango_font_metrics_get_approximate_char_width(metrics);
        pango_font_metrics_unref(metrics);
        
        int tab_width_pango = char_width * self->tab_width;
        tab_array = pango_tab_array_new(1, FALSE); /* Pango Units */
        pango_tab_array_set_tab(tab_array, 0, PANGO_TAB_LEFT, tab_width_pango);
    }

    for (size_t i = 0; i < count_lines; ++i) {
        size_t visual_line_idx = start_line + i;
        if (visual_line_idx >= max_lines) break;

        size_t phys_line = get_physical_line_index(self, visual_line_idx);
        if (phys_line == (size_t)-1) continue;

        size_t len;
        char *text = NULL;
        
        /* Virtualization State */
        gboolean is_virtualized = FALSE;
        size_t chunk_padding = 0; /* Byte offset of the chunk start */
        double render_x_offset = 0; /* X offset to draw the chunk at */
        double render_y_offset = 0; /* Y offset (for vertical virtualization) */
        double virtual_full_height = 0; /* Full height of a massive wrapped line */

        size_t full_len = document_get_line_length(self->doc, phys_line);

        /* Optimization for Long Lines (> 4KB) */
        if (full_len > 4096) {
             is_virtualized = TRUE;
             double cw = self->cached_char_width > 1.0 ? self->cached_char_width : 8.0; 
             
             if (self->wrap_lines) {
                 /* Vertical Virtualization (Wrapping Enabled) */
                 int available_w = width - text_start_x - 20; 
                 if (available_w < 50) available_w = 50;
                 int chars_per_line = (int)((double)available_w / cw);
                 if (chars_per_line < 1) chars_per_line = 1;
                 
                 /* Determine which rows are visible */
                 /* We need the line's top Y position relative to document */
                 /* current_y_pos passed to this loop is relative to the start of the VIEWPORT (if translating?) 
                    No, current_y_pos accumulates from 0 in the snapshot loop?
                    Wait, snapshot loop iterates visible lines from 'start'.
                    'current_y_pos' starts at 0? 
                    Actually, let's check lines before this block.
                    The loop iterates from 'start_line' to 'end_line'.
                    The 'current_y_pos' is calculated?
                    Wait, the loop uses 'document_iter_next_line' but in renderer it iterates by index?
                    Ah, lines 177: for (size_t i = start_line; i < end_line; i++) { ... }
                    Wait, earlier in the file, 'current_y_pos' was initialized.
                 */
                 
                 /* We need to know where THIS specific line starts relative to screen top */
                 /* We can assume 'current_y_pos' tracks the Render Y.
                    But if we just jumped to 'start_line', current_y_pos might be 0 relative to the draw call.
                    Actually, standard loop:
                    for (...) {
                       // layout
                       translate(x, y);
                       y += h;
                    }
                    So 'current_y_pos' is correct for the start of this line.
                 */
                 
                 /* But wait, if this massive line STARTS way above the viewport,
                    start_line (calculated by get_visible_line_range) would be THIS massive line.
                    But 'current_y_pos' in the loop starts at... ?
                    If get_visible_line_range returned start_line = 5, and we start loop at 5.
                    But line 5 start Y might be -5000 relative to scroll?
                    
                    Re-check 'editor_widget_snapshot'.
                    Line 160: double current_y_pos = 0;
                    Line 163: if (self->line_y_offsets...) { current_y_pos = offsets[start_line]; }
                    Else estimation.
                    
                    The 'current_y_pos' is essentially 'Y_start_of_line_in_document'.
                    We subtract 'scroll_y' during translation later.
                    
                    So:
                    line_doc_y = current_y_pos.
                    visible_top = scroll_y.
                    relative_start = visible_top - line_doc_y.
                    
                    If relative_start < 0, line starts below screen top. row_start = 0.
                    If relative_start > 0, line starts above. row_start = relative_start / line_h.
                 */
                  
                 double line_doc_y = 0;
                 if (self->line_y_offsets && visual_line_idx < self->line_y_offsets->len) {
                     line_doc_y = g_array_index(self->line_y_offsets, double, visual_line_idx);
                 } else {
                     line_doc_y = current_y_pos + scroll_y;
                 }
                 double relative_start = scroll_y - line_doc_y;
                 
                 size_t start_row = 0;
                 if (relative_start > 0) {
                     start_row = (size_t)(relative_start / self->line_height);
                 }
                 
                 size_t full_rows = (full_len + chars_per_line - 1) / chars_per_line;
                 if (full_rows == 0) full_rows = 1;
                 virtual_full_height = (double)full_rows * self->line_height;

                 size_t start_char_idx = start_row * chars_per_line;
                 if (start_char_idx > full_len) start_char_idx = full_len;
                 
                 /* How many rows visible? */
                 /* We need to fill 'height' (viewport height) */
                 size_t visible_rows = (size_t)(height / self->line_height) + 2; 
                 size_t chars_to_fetch = visible_rows * chars_per_line + 100; // buffer
                 
                 size_t safe_len = chars_to_fetch;
                 if (start_char_idx + safe_len > full_len) safe_len = full_len - start_char_idx;
                 
                 text = document_get_text_range(self->doc, document_get_offset_of_line(self->doc, phys_line) + start_char_idx, safe_len);
                 len = safe_len;
                 chunk_padding = start_char_idx;
                 
                 /* We must render this chunk offset vertically */
                 render_y_offset = (double)start_row * self->line_height;
                 
             } else {
                 /* Horizontal Virtualization (No Wrap) */
                 double cw = self->cached_char_width > 1.0 ? self->cached_char_width : 8.0; 
                 
                 double start_char_visual = (scroll_x / cw) - 100.0;
                 if (start_char_visual < 0) start_char_visual = 0;
                 
                 size_t start_byte_approx = (size_t)start_char_visual;
                 if (start_byte_approx > full_len) start_byte_approx = full_len;

                 size_t visible_chars = (size_t)(width / cw) + 300; 
                 size_t safe_len = visible_chars;
                 if (start_byte_approx + safe_len > full_len) safe_len = full_len - start_byte_approx;
                 
                 text = document_get_text_range(self->doc, document_get_offset_of_line(self->doc, phys_line) + start_byte_approx, safe_len);
                 len = safe_len;
                 chunk_padding = start_byte_approx;
                 render_x_offset = start_byte_approx * cw;
             }
             
             /* UTF-8 Safety */
        } else {
             text = document_get_line_truncated(self->doc, phys_line, &len, MAX_PANGO_LINE_LEN + 1024);
        }
        // fprintf(stderr, "[DEBUG] snapshot loop: len=%zu virtualized=%d\n", len, is_virtualized);

        /* Null check - document_get_line_truncated can return NULL */
        if (!text) {
            text = g_strdup("");
            len = 0;
        }
        
        /* UTF-8 Validation and Cleanup */
        if (len > 0 && !g_utf8_validate(text, len, NULL)) {
             char *safe_text = g_utf8_make_valid(text, len);
             g_free(text);
             text = safe_text;
             len = strlen(text);
        }
        
        /* Strip trailing newlines for Pango render (\n, \r\n, \r) */
        while (len > 0 && (text[len-1] == '\n' || text[len-1] == '\r')) {
            text[len-1] = '\0'; /* Null terminate at the new end */
            len--;
        }

        PangoLayout *layout = pango_layout_new(context);
        pango_layout_set_font_description(layout, self->font_desc);
        int pango_len = (len > MAX_PANGO_LINE_LEN) ? MAX_PANGO_LINE_LEN : (int)len;
        pango_layout_set_text(layout, text, pango_len);
        if (tab_array) pango_layout_set_tabs(layout, tab_array);
        
        /* Word Wrap - account for gutter and padding */
        /* Virtualized lines need handling: */
        /* If Horizontal (No Wrap for Huge): force no-wrap. */
        /* If Vertical (Wrap Huge): force wrap CHAR. */
        
        if (self->wrap_lines) {
            int available_w = width - text_start_x - 20 - (int)minimap_w; /* Buffer + Minimap */
            if (available_w < 50) available_w = 50; /* Safe min width */
            pango_layout_set_width(layout, available_w * PANGO_SCALE);
            
            if (is_virtualized) {
               pango_layout_set_wrap(layout, PANGO_WRAP_CHAR);
            } else {
               pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
            }
        } else {
            pango_layout_set_width(layout, -1); /* Unwrapped */
        }
        
        /* Syntax highlight and Search Highlight */
        /* MUST COPY cached attributes to avoid corrupting the syntax cache with search highlights! */
        PangoAttrList *cached_attrs = NULL;
        /* Disable highlighting for virtualized chunks to prevent applying Start-Of-Line state to Middle-Of-Line text */
        if (!is_virtualized) {
            cached_attrs = syntax_highlight_line(self->syntax_ctx, phys_line, text);
        }
        PangoAttrList *attrs = cached_attrs ? pango_attr_list_copy(cached_attrs) : pango_attr_list_new();
        
        /* Attribute injection removed for search matches - moving to manual drawing below */
        
        /* Filter Matches Collection */
        GArray *filter_highlight_ranges = NULL;
        if (self->filtered_lines && self->filter_pattern && *self->filter_pattern) {
             if (self->filter_is_regex && self->filter_regex_pattern) {
                 GMatchInfo *match_info;
                 if (g_regex_match(self->filter_regex_pattern, text, 0, &match_info)) {
                     while (g_match_info_matches(match_info)) {
                         int start_pos, end_pos;
                         g_match_info_fetch_pos(match_info, 0, &start_pos, &end_pos);
                         
                         if (start_pos >= 0 && end_pos > start_pos) {
                             if (!filter_highlight_ranges) 
                                 filter_highlight_ranges = g_array_new(FALSE, FALSE, sizeof(int) * 2);
                             int range[2] = {start_pos, end_pos};
                             g_array_append_vals(filter_highlight_ranges, range, 1);
                         }
                         g_match_info_next(match_info, NULL);
                     }
                 }
                 g_match_info_free(match_info);
             } else {
                 /* Plain text search */
                 const char *needle = self->filter_pattern;
                 char *haystack_lower = NULL;
                 char *needle_lower = NULL;
                 
                 const char *haystack = text;
                 const char *search_needle = needle;
                 gboolean free_needed = FALSE;
                 
                 if (!self->filter_case_sensitive) {
                     haystack_lower = g_utf8_strdown(text, -1);
                     needle_lower = g_utf8_strdown(needle, -1);
                     haystack = haystack_lower;
                     search_needle = needle_lower;
                     free_needed = TRUE;
                 }
                 
                 size_t needle_len = strlen(search_needle);
                 const char *current = haystack;
                 const char *p = NULL;
                 
                 if (needle_len > 0) {
                     while ((p = strstr(current, search_needle)) != NULL) {
                         int byte_offset = (int)(p - haystack);
                         
                         if (!filter_highlight_ranges) 
                             filter_highlight_ranges = g_array_new(FALSE, FALSE, sizeof(int) * 2);
                         int range[2] = {byte_offset, byte_offset + (int)needle_len};
                         g_array_append_vals(filter_highlight_ranges, range, 1);
                         
                         current = p + needle_len; 
                     }
                 }
                 
                if (free_needed) {
                    g_free(haystack_lower);
                    g_free(needle_lower);
                }
            }
        }
        
        /* Apply Filter Highlights to attrs */
        if (filter_highlight_ranges) {
            for (guint i = 0; i < filter_highlight_ranges->len; i++) {
                int *range = &g_array_index(filter_highlight_ranges, int, i);
                int start = range[0];
                int end = range[1];
                
                /* Ensure valid range */
                if (start >= 0 && end > start && end <= len) {
                     PangoAttribute *attr = pango_attr_background_new(65535, 65535, 0); /* Yellow */
                     attr->start_index = start;
                     attr->end_index = end;
                     pango_attr_list_insert(attrs, attr);
                     
                     PangoAttribute *fg_attr = pango_attr_foreground_new(0, 0, 0); /* Black Text */
                     fg_attr->start_index = start;
                     fg_attr->end_index = end;
                     pango_attr_list_insert(attrs, fg_attr);
                }
            }
            g_array_free(filter_highlight_ranges, TRUE);
        }

        pango_layout_set_attributes(layout, attrs);
        
        /* Cleanup */
        if (cached_attrs) pango_attr_list_unref(cached_attrs); /* Release reference from syntax_highlight_line */
        pango_attr_list_unref(attrs);
        
        /* Calculate height of this layout */
        int pixel_h;
        pango_layout_get_pixel_size(layout, NULL, &pixel_h);
        double real_layout_h = (double)pixel_h;
        double layout_h = real_layout_h;
        if (layout_h < self->line_height) layout_h = self->line_height; /* Min height */
        double advance_h = layout_h;
        if (is_virtualized && self->wrap_lines && virtual_full_height > 0) {
            advance_h = virtual_full_height;
        }
        
        /* Vertical Centering: Calculate offset to center the text within the row */
        double centering_offset = floor((layout_h - real_layout_h) / 2.0);

        /* Apply start offset if first line */
        /* Draw Current Line Highlight - check if ANY cursor is on this line (and no selection) */
        if (self->highlight_current_line) {
             gboolean highlight_this = FALSE;
             for (guint c = 0; c < num_cursors; c++) {
                 EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                 if (cursor_lines[c] == phys_line && cur->cursor_offset == cur->selection_anchor) {
                     highlight_this = TRUE;
                     break;
                 }
             }
             
             if (highlight_this) {
                 GdkRGBA hl_color = self->color_text;
                 hl_color.alpha = 0.05; 
                 if (self->color_text.red > 0.5) hl_color.alpha = 0.1;
                 
                 gtk_snapshot_append_color(snapshot, &hl_color, 
                    &GRAPHENE_RECT_INIT(text_start_x, current_y_pos + self->padding_top, width - text_start_x, layout_h));
             }
        }

        /* Draw Folded Line Highlight */
        if (self->enable_folding && editor_widget_is_fold_collapsed(self, phys_line)) {
             GdkRGBA fold_hl = {0.2, 0.4, 0.8, 0.1}; /* 10% opacity blue */
             
             gtk_snapshot_append_color(snapshot, &fold_hl, 
                &GRAPHENE_RECT_INIT(text_start_x, current_y_pos + self->padding_top, width - text_start_x, layout_h));
        }

        /* Draw Fold Marker */
        /* Draw Line Number */
        /* Draw Line Number */
        if (self->show_line_numbers) {
            char lnum_buf[32];
            snprintf(lnum_buf, sizeof(lnum_buf), "%zu", phys_line + 1);
            
            PangoLayout *lnum_layout = pango_layout_new(context);
            pango_layout_set_font_description(lnum_layout, self->font_desc);
            
            pango_layout_set_text(lnum_layout, lnum_buf, -1);
            pango_layout_set_alignment(lnum_layout, PANGO_ALIGN_RIGHT);
            
            double fold_w = editor_widget_get_fold_gutter_width(self);

            /* Width = gutter_w - fold_w - 8 (padding) */
            double lnum_w = gutter_w - fold_w - 8.0; 
            if (lnum_w < 1.0) lnum_w = 1.0;
            pango_layout_set_width(lnum_layout, (int)(lnum_w * PANGO_SCALE));
            
            /* Gutter text color - dim it */
            GdkRGBA gutter_fg = self->color_text;
            gutter_fg.alpha = 0.5;
            
            gtk_snapshot_save(snapshot);
            /* Translate X=4 for 4px left padding. Use centering_offset for consistent alignment. */
            gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(4, current_y_pos + self->padding_top + centering_offset));
            gtk_snapshot_append_layout(snapshot, lnum_layout, &gutter_fg);
            gtk_snapshot_restore(snapshot);
            
            g_object_unref(lnum_layout);
            
            /* Draw Fold Marker - In its own column on the right */
            if (self->enable_folding && self->fold_ranges) {
                FoldRange fr;
                if (editor_widget_get_fold_range(self, phys_line, &fr)) {
                    gboolean is_collapsed = editor_widget_is_fold_collapsed(self, phys_line);
                    
                    /* Show if collapsed (always) OR if mouse is hovering in gutter */
                    if (is_collapsed || self->mouse_in_gutter) {
                        const char *marker = is_collapsed ? "▶" : "▼";
                        PangoLayout *fold_layout = pango_layout_new(context);
                        pango_layout_set_font_description(fold_layout, self->font_desc);
                        pango_layout_set_text(fold_layout, marker, -1);
                        pango_layout_set_alignment(fold_layout, PANGO_ALIGN_CENTER);
                        
                        /* Center in fold gutter */
                        pango_layout_set_width(fold_layout, (int)((fold_w - 4.0) * PANGO_SCALE));
    
                        GdkRGBA fold_fg = self->color_text;
                        fold_fg.alpha = 0.7;
    
                        gtk_snapshot_save(snapshot);
                        /* Position in fold column: [gutter_w - fold_w, gutter_w] */
                        gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(gutter_w - fold_w + 2, current_y_pos + self->padding_top + centering_offset));
                        gtk_snapshot_append_layout(snapshot, fold_layout, &fold_fg);
                        gtk_snapshot_restore(snapshot);
                        g_object_unref(fold_layout);
                    }
                }
            }
        }

        
        // fprintf(stderr, "[DEBUG] snapshot loop: line_idx=%zu done\n", line_idx);

        gtk_snapshot_save(snapshot);
        /* Clip text area to ensure it doesn't draw over the gutter or minimap */
        gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(gutter_w, 0, width - gutter_w - minimap_w, height));
        
        /* Translate to the TOP of the line slot (contiguous baseline) */
        /* Incorporate render_x_offset for virtualized chunks */
        gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(text_start_x - scroll_x + render_x_offset, current_y_pos + self->padding_top + render_y_offset));
        
        /* Draw Line Background if selected */
        /* Selection rendering across lines is complex. 
           Simplified: If line is fully selected or partially.
        */

        /* Draws Search Matches (Manual Drawing) */
        /* Adjusted start offset for virtualized chunks */
        size_t line_start_off_real = document_get_offset_of_line(self->doc, phys_line);
        size_t line_start_off = line_start_off_real + chunk_padding;
        size_t line_end_off = 0;
        size_t total_lines = document_get_line_count(self->doc);
        if (phys_line + 1 < total_lines) {
            line_end_off = document_get_offset_of_line(self->doc, phys_line + 1);
        } else {
            line_end_off = document_get_length(self->doc);
        }

        if (self->search_matches && self->search_matches->len > 0) {
            
            int low = 0;
            int high = (int)self->search_matches->len - 1;
            int first_candidate = -1;
            
            while (low <= high) {
                int mid = (low + high) / 2;
                SearchMatch *m = &g_array_index(self->search_matches, SearchMatch, mid);
                if (m->end > line_start_off) {
                    first_candidate = mid;
                    high = mid - 1;
                } else {
                    low = mid + 1;
                }
            }
            
            if (first_candidate >= 0) {
                for (int m = first_candidate; m < (int)self->search_matches->len; m++) {
                    SearchMatch match = g_array_index(self->search_matches, SearchMatch, m);
                    if (match.start >= line_end_off) break;
                    
                    /* Calculate intersection */
                    size_t match_start = match.start;
                    size_t match_end = match.end;
                    
                    if (match_start < line_end_off && match_end > line_start_off) {
                        int i_start = (int)(MAX(match_start, line_start_off) - line_start_off);
                        int i_end = (int)(MIN(match_end, (line_start_off + len)) - line_start_off);
                        
                        if (i_start < i_end) {
                            /* Draw this range on the layout */
                            PangoLayoutIter *iter = pango_layout_get_iter(layout);
                            int m_count = pango_layout_get_line_count(layout);
                            int m_line_idx = 0;
                            do {
                                PangoLayoutLine *p_line = pango_layout_iter_get_line_readonly(iter);
                                int line_start_index = p_line->start_index;
                                int line_end_index = line_start_index + p_line->length;
                                
                                PangoRectangle line_rect;
                                pango_layout_iter_get_line_extents(iter, NULL, &line_rect);
                                double ry = pango_units_to_double(line_rect.y) + centering_offset;
                                double rh = pango_units_to_double(line_rect.height);
                                
                                /* CONTIGUOUS FIX: Expand highlights to cover centering gaps */
                                if (m_line_idx == 0) {
                                    rh += ry;
                                    ry = 0;
                                }
                                if (m_line_idx == m_count - 1) {
                                    rh = layout_h - ry;
                                }

                                if (i_end >= line_start_index && i_start <= line_end_index) {
                                    int *ranges; int n_ranges;
                                    int range_start = MAX(i_start, line_start_index);
                                    int range_end = MIN(i_end, line_end_index);
                                    
                                    pango_layout_line_get_x_ranges(p_line, range_start, range_end, &ranges, &n_ranges);
                                    for (int r = 0; r < n_ranges; r++) {
                                        double rx = pango_units_to_double(ranges[2 * r]);
                                        double rw = pango_units_to_double(ranges[2 * r + 1] - ranges[2 * r]);
                                        
                                        if (rw > 0) {
                                            if (match.start == self->current_match_offset) {
                                                /* Current Match: Border only? Or light Fill + Border? 
                                                   User said "current match has border". 
                                                   Let's do light orange fill + solid border. */
                                                GdkRGBA fill = {1.0, 0.8, 0.4, 0.2}; /* Light Orange, Low Opacity */
                                                GdkRGBA border = {1.0, 0.6, 0.0, 1.0}; /* Solid Orange Border */
                                                
                                                gtk_snapshot_append_color(snapshot, &fill, &GRAPHENE_RECT_INIT((float)rx, (float)ry, (float)rw, (float)rh));
                                                
                                                gtk_snapshot_append_border(snapshot, 
                                                    &GSK_ROUNDED_RECT_INIT((float)rx, (float)ry, (float)rw, (float)rh),
                                                    (float[4]){1, 1, 1, 1},
                                                    (GdkRGBA[4]){border, border, border, border});
                                            } else {
                                                /* Other matches: Greyish, low opacity, no border */
                                                GdkRGBA fill = {0.6, 0.6, 0.6, 0.2}; 
                                                if (dark_mode_needed) fill = (GdkRGBA){0.8, 0.8, 0.8, 0.15};
                                                
                                                gtk_snapshot_append_color(snapshot, &fill, &GRAPHENE_RECT_INIT((float)rx, (float)ry, (float)rw, (float)rh));
                                                
                                                /* Underline (Bottom Border) */
                                                GdkRGBA underline_col = {0.5, 0.5, 0.5, 0.5};
                                                if (dark_mode_needed) underline_col = (GdkRGBA){0.7, 0.7, 0.7, 0.4};
                                                
                                                gtk_snapshot_append_color(snapshot, &underline_col, 
                                                    &GRAPHENE_RECT_INIT((float)rx, (float)(ry + rh - 1.0), (float)rw, 1.0f));
                                            }
                                        }
                                    }
                                    g_free(ranges);
                                }
                                m_line_idx++;
                            } while (pango_layout_iter_next_line(iter));
                            pango_layout_iter_free(iter);
                        }
                    }
                }
            }
        }

        for (guint c = 0; c < self->cursors->len; c++) {
            EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
            
            size_t start_sel = MIN(cur->cursor_offset, cur->selection_anchor);
            size_t end_sel = MAX(cur->cursor_offset, cur->selection_anchor);
            
            if (start_sel < line_end_off && end_sel > line_start_off && start_sel != end_sel) {
                size_t sel_in_line_start = MAX(start_sel, line_start_off) - line_start_off;
                size_t sel_in_line_end = MIN(end_sel, (line_start_off + len)) - line_start_off;
                
                if (len == 0) {
                    if (end_sel > line_start_off) {
                        /* For empty lines, draw selection block filling full layout_h */
                        gtk_snapshot_append_color(snapshot, 
                                                  &(GdkRGBA){0.2, 0.4, 0.8, 0.35},
                                                  &GRAPHENE_RECT_INIT(0, 0, (float)width, (float)layout_h));
                    }
                } else {
                    PangoLayoutIter *iter = pango_layout_get_iter(layout);
                    int s_count = pango_layout_get_line_count(layout);
                    int s_line_idx = 0;
                    do {
                        PangoLayoutLine *p_line = pango_layout_iter_get_line_readonly(iter);
                        int line_start_index = p_line->start_index;
                        int line_end_index = line_start_index + p_line->length;
                        
                        PangoRectangle line_rect;
                        pango_layout_iter_get_line_extents(iter, NULL, &line_rect);
                        double ry = pango_units_to_double(line_rect.y) + centering_offset;
                        double rh = pango_units_to_double(line_rect.height);

                        /* CONTIGUOUS FIX: Expand highlights to cover centering gaps */
                        if (s_line_idx == 0) {
                            rh += ry;
                            ry = 0;
                        }
                        if (s_line_idx == s_count - 1) {
                            rh = layout_h - ry;
                        }

                        if (sel_in_line_end >= (size_t)line_start_index && sel_in_line_start <= (size_t)line_end_index) {
                            int *ranges; int n_ranges;
                            int range_start = (int)MAX(sel_in_line_start, (size_t)line_start_index);
                            int range_end = (int)MIN(sel_in_line_end, (size_t)line_end_index);
                            
                            pango_layout_line_get_x_ranges(p_line, range_start, range_end, &ranges, &n_ranges);
                            for (int r = 0; r < n_ranges; r++) {
                                double rx = pango_units_to_double(ranges[2 * r]);
                                double rw = pango_units_to_double(ranges[2 * r + 1] - ranges[2 * r]);
                                if (rw > 0) gtk_snapshot_append_color(snapshot, &(GdkRGBA){0.2, 0.4, 0.8, 0.35}, &GRAPHENE_RECT_INIT((float)rx, (float)ry, (float)rw, (float)rh));
                            }
                            g_free(ranges);
                            
                            if (end_sel > line_start_off + (size_t)line_end_index) {
                                int x_pos;
                                pango_layout_line_index_to_x(p_line, line_end_index, FALSE, &x_pos);
                                double dx = pango_units_to_double(x_pos);
                                double ew = (p_line->resolved_dir == PANGO_DIRECTION_RTL) ? dx : (width + scroll_x) - dx;
                                double ex = (p_line->resolved_dir == PANGO_DIRECTION_RTL) ? 0 : dx;
                                if (ew > 0) gtk_snapshot_append_color(snapshot, &(GdkRGBA){0.2, 0.4, 0.8, 0.35}, &GRAPHENE_RECT_INIT((float)ex, (float)ry, (float)ew, (float)rh));
                            }
                        }
                        s_line_idx++;
                    } while (pango_layout_iter_next_line(iter));
                    pango_layout_iter_free(iter);
                }
            }
        }
        
        /* 2. Draw Text (Apply centering offset here) */
        gtk_snapshot_save(snapshot);
        gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(0, (float)centering_offset));
        gtk_snapshot_append_layout(snapshot, layout, &self->color_text);
        gtk_snapshot_restore(snapshot);

        /* Draw Custom Filter Underlines */
        /* Draw Custom Filter Underlines */
        if (filter_highlight_ranges) {
            /* We need to draw underlines matching the syntax color of the text.
               We iterate the Pango attributes (which contain syntax colors) and 
               intersect them with our filter matching ranges.
            */
            PangoAttrIterator *attr_iter = pango_attr_list_get_iterator(attrs);
            
            do {
                int a_start, a_end;
                pango_attr_iterator_range(attr_iter, &a_start, &a_end);
                
                /* Get Color for this range */
                GdkRGBA range_color = self->color_text;
                PangoAttribute *fg_attr = pango_attr_iterator_get(attr_iter, PANGO_ATTR_FOREGROUND);
                if (fg_attr) {
                    PangoColor *pc = &((PangoAttrColor*)fg_attr)->color;
                    range_color.red = (float)pc->red / 65535.0f;
                    range_color.green = (float)pc->green / 65535.0f;
                    range_color.blue = (float)pc->blue / 65535.0f;
                    /* Use opaque alpha as PangoColors don't have alpha, 
                       and we want to revert the force-1.0-alpha logic but match text (which is opaque) */
                    range_color.alpha = 1.0; 
                }

                /* Check intersection with all filter matches */
                for (guint i = 0; i < filter_highlight_ranges->len; i++) {
                    int *range = &g_array_index(filter_highlight_ranges, int, i * 2);
                    int m_start = range[0];
                    int m_end = range[1];
                    
                    /* Intersection */
                    int i_start = MAX(a_start, m_start);
                    int i_end = MIN(a_end, m_end);
                    
                    if (i_start < i_end) {
                        /* We have an intersection [i_start, i_end] with color range_color.
                           Now find the visual X coordinates for this logical range on the line(s). */
                        
                        /* Layout iter is needed to map index to line Y/X */
                        PangoLayoutIter *liter = pango_layout_get_iter(layout);
                        do {
                            PangoLayoutLine *line = pango_layout_iter_get_line_readonly(liter);
                            int l_start = line->start_index;
                            int l_end = l_start + line->length;
                            
                            /* Line intersection with our colored match range */
                            int li_start = MAX(i_start, l_start);
                            int li_end = MIN(i_end, l_end);
                            
                            if (li_start < li_end) {
                                int baseline = pango_layout_iter_get_baseline(liter);
                                double y_base = (double)baseline / PANGO_SCALE + centering_offset;
                                /* Pixel-snap the underline vertical position */
                                double underline_y = floor(y_base + 3.0 + 0.5);
                                
                                int *ranges = NULL;
                                int n_ranges = 0;
                                pango_layout_line_get_x_ranges(line, li_start, li_end, &ranges, &n_ranges);
                                
                                if (ranges) {
                                    for (int r = 0; r < n_ranges; r++) {
                                        double x0 = (double)ranges[2*r] / PANGO_SCALE;
                                        double x1 = (double)ranges[2*r+1] / PANGO_SCALE;
                                        
                                        /* Pixel-snap the horizontal range to avoid sub-pixel rendering blur */
                                        x0 = floor(x0 + 0.5);
                                        x1 = floor(x1 + 0.5);
                                        double w = x1 - x0;
                                        
                                        if (w >= 1.0) {
                                            gtk_snapshot_append_color(snapshot, &range_color,
                                                &GRAPHENE_RECT_INIT((float)x0, (float)underline_y, (float)w, 1.0f));
                                        }
                                    }
                                    g_free(ranges);
                                }
                            }
                        } while (pango_layout_iter_next_line(liter));
                        pango_layout_iter_free(liter);
                    }
                }
            } while (pango_attr_iterator_next(attr_iter));
            
            pango_attr_iterator_destroy(attr_iter);
            g_array_free(filter_highlight_ranges, TRUE);
        }
        
        /* 3. Draw Cursors for all cursors */
        /* User requested 0.4px specifically (verified "sharp" on their display). 
           We retain the multi-pass drawing to ensure it remains opaque/black 
           rather than a faint sub-pixel blur. */
        float cursor_w = 0.4f;
        int scale = gtk_widget_get_scale_factor(widget);

        for (guint c = 0; c < self->cursors->len; c++) {
            EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
            gboolean has_selection = (cur->cursor_offset != cur->selection_anchor);
            
            /* Optimized check: Is cursor within this line's range? */
            if (cur->cursor_offset >= line_start_off && cur->cursor_offset <= (line_start_off + len)) {
                
                if (gtk_widget_has_focus(widget) && self->cursor_alpha > 0.01 && !has_selection && !self->is_dragging_selection) {
                     size_t index_in_line = cur->cursor_offset - line_start_off;
                     /* Safety clamp */
                     size_t effective_len = (len > MAX_PANGO_LINE_LEN) ? MAX_PANGO_LINE_LEN : len;
                     if (index_in_line > effective_len) index_in_line = effective_len;
                     
                     PangoRectangle strong_pos;
                     pango_layout_get_cursor_pos(layout, (int)index_in_line, &strong_pos, NULL);
                     
                     GdkRGBA cursor_color = self->color_cursor;
                     cursor_color.alpha = self->cursor_alpha;
                     
                     /* Snap to physical pixel grid */
                     double x_pos = pango_units_to_double(strong_pos.x);
                     float cursor_x = (float)floor(x_pos * scale + 0.5) / scale;

                     /* Calculate Cursor Height and Y: match line height and center */
                     double pango_h = pango_units_to_double(strong_pos.height);
                     double pango_y = pango_units_to_double(strong_pos.y) + centering_offset;
                     
                     double cursor_h = MAX(pango_h, self->line_height);
                     double cursor_y = pango_y - (cursor_h - pango_h) / 2.0;
                     
                     if (self->insert_mode) {
                         /* Insert Mode: I-Beam / Bar Cursor */
                         /* Draw 4 times to accumulate opacity and force "sharpness" on sub-pixels */
                         for (int pass = 0; pass < 4; pass++) {
                            gtk_snapshot_append_color(snapshot, &cursor_color, &GRAPHENE_RECT_INIT(cursor_x, (float)((int)(cursor_y + 0.5)), cursor_w, (float)((int)cursor_h)));
                         }
                     } else {
                         /* Overwrite Mode: Block Cursor with Inverted Text */
                         
                         /* 1. Determine block width */
                         double block_w = 0;
                         /* Need to fetch the character length in bytes for this grapheme */
                         int char_len = 0;
                         const char *char_text = NULL;
                         
                         if (index_in_line < effective_len && len > 0) {
                             /* Find grapheme boundary */
                             /* "text" is the line text available in this scope */
                             const char *start_ptr = text + index_in_line;
                             const char *end_ptr = g_utf8_next_char(start_ptr);
                             char_len = (int)(end_ptr - start_ptr);
                             char_text = start_ptr;
                             
                             /* Regular char width from existing layout */
                             block_w = pango_units_to_double(strong_pos.width);
                         } 
                         
                         /* Fallback width for EOL or zero-width chars */
                         if (block_w <= 0.1) {
                             PangoContext *pctx = gtk_widget_get_pango_context(widget);
                             PangoFontMetrics *metrics = pango_context_get_metrics(pctx, self->font_desc, NULL);
                             block_w = pango_units_to_double(pango_font_metrics_get_approximate_char_width(metrics));
                             pango_font_metrics_unref(metrics);
                         }
                         
                         /* 2. Draw Block (Cursor Color = Text Color) */
                         /* Use opacity to handle blink, but relative to "On" state it absorbs the background */
                         cursor_color.alpha = self->cursor_alpha;
                         
                         gtk_snapshot_append_color(snapshot, &cursor_color, 
                             &GRAPHENE_RECT_INIT(cursor_x, (float)((int)(cursor_y + 0.5)), (float)block_w, (float)((int)cursor_h)));
                             
                         /* 3. Draw Inverted Text (Background Color) on top */
                         if (char_len > 0 && char_text) {
                             /* Create temp layout for the single character */
                             PangoLayout *char_layout = pango_layout_new(context);
                             pango_layout_set_font_description(char_layout, self->font_desc);
                             if (tab_array) pango_layout_set_tabs(char_layout, tab_array);
                             pango_layout_set_text(char_layout, char_text, char_len);
                             
                             GdkRGBA inv_text_color = bg_color;
                             inv_text_color.alpha = self->cursor_alpha;
                             
                             /* Align positioning exactly */
                             gtk_snapshot_save(snapshot);
                             gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(cursor_x, (float)((int)(cursor_y + 0.5))));
                             
                             /* Adjust for baseline/centering diff inside the block? 
                                strong_pos coordinates are layout relative. helper calculation:
                                cursor_y = pango_y - (cursor_h - pango_h)/2.0
                                We want to draw the text at the same relative position.
                                pango_layout_draw starts at top-left of layout.
                                We need to verify if pango_layout_new creates same vertical metrics.
                                Usually yes.
                                But we shifted cursor_y to center the cursor block if cursor_h > pango_h.
                                We should draw the text at `pango_y` (offset from centering logic).
                                
                                x_pos = pango_units_to_double(strong_pos.x) -> already in cursor_x
                                pango_y = pango_units_to_double(strong_pos.y) + centering_offset
                                
                                The cursor_x we use is floor(x_pos).
                                We simply want to draw the char layout at (cursor_x, pango_y).
                             */
                             
                             /* Translate back to root of this line's drawing context or just calc delta?
                                We are currently translated to: (text_start_x - scroll_x, current_y_pos + padding_top)
                                cursor_x is relative to this.
                             */
                             
                             gtk_snapshot_restore(snapshot); /* Undo the block translation if I did any? No I didn't. */
                             
                             /* Actually, let's just append the layout at the correct coordinates */
                             gtk_snapshot_save(snapshot);
                             
                             /* Calculate Y for text.  
                                we have pango_y calculated above.
                                pango_y includes centering_offset.
                                We need to draw at (cursor_x, pango_y).
                             */
                             gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(cursor_x, (float)pango_y));
                             
                             gtk_snapshot_append_layout(snapshot, char_layout, &inv_text_color);
                             
                             gtk_snapshot_restore(snapshot);
                             g_object_unref(char_layout);
                         }
                     }
                }
            }
        }

        /* Draw DnD Drop Caret */
        if (self->is_dragging_selection && self->drag_drop_offset != (size_t)-1) {
            size_t drop_line = document_get_line_of_offset(self->doc, self->drag_drop_offset);
            if (phys_line == drop_line) {
                size_t line_start_off = document_get_offset_of_line(self->doc, phys_line);
                size_t index_in_line = self->drag_drop_offset - line_start_off;
                if (index_in_line > len) index_in_line = len;

                size_t effective_len = strlen(pango_layout_get_text(layout));
                size_t safe_idx = MIN(index_in_line, effective_len);

                PangoRectangle strong_pos;
                pango_layout_get_cursor_pos(layout, (int)safe_idx, &strong_pos, NULL);

                GdkRGBA caret_color = self->drag_copy_mode ? (GdkRGBA){0.18, 0.76, 0.49, 1.0} : (GdkRGBA){1.0, 0.647, 0.0, 1.0};
                /* Snap for drop caret */
                float caret_x = (float)(self->drag_x - text_start_x + scroll_x);
                if (caret_x < 0) caret_x = 0;
                caret_x = (float)floor(caret_x * scale + 0.5) / scale;
                
                double pango_h = pango_units_to_double(strong_pos.height);
                double pango_y = pango_units_to_double(strong_pos.y) + centering_offset;
                double caret_h = MAX(pango_h, self->line_height);
                double caret_y = pango_y - (caret_h - pango_h) / 2.0;

                for (int pass = 0; pass < 4; pass++) {
                    gtk_snapshot_append_color(snapshot, 
                                              &caret_color,
                                              &GRAPHENE_RECT_INIT(caret_x, (float)((int)(caret_y + 0.5)), cursor_w, (float)((int)caret_h)));
                }
            }
        }
        
        /* Update Y position for next line */
        current_y_pos += advance_h;
        
        gtk_snapshot_pop(snapshot);
        gtk_snapshot_restore(snapshot);
        g_object_unref(layout);
        g_free(text);

        if (current_y_pos > height) {
             break;
        }
    }
    // fprintf(stderr, "[DEBUG] snapshot: Loop finished\n");
    if (tab_array) pango_tab_array_free(tab_array);

    /* Draw DnD Overlays */
    // fprintf(stderr, "[DEBUG] snapshot: Checking DnD\n");
    if (self->is_dnd_active) {
        /* 1. Ghost Text (Cursor Follower) */
        if (self->drag_ghost_layout) {
            gtk_snapshot_save(snapshot);
            gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT((float)self->drag_x, (float)self->drag_y));
            
            GdkRGBA ghost_color = self->color_text;
            ghost_color.alpha = 0.5;
            
            gtk_snapshot_append_layout(snapshot, self->drag_ghost_layout, &ghost_color);
            gtk_snapshot_restore(snapshot);
        }

        /* 2. Viewport Border */
        GdkRGBA border_color = self->drag_copy_mode ? (GdkRGBA){0.18, 0.76, 0.49, 1.0} : (GdkRGBA){1.0, 0.647, 0.0, 1.0};
        gtk_snapshot_append_border(snapshot, 
                                   &GSK_ROUNDED_RECT_INIT(0, 0, (float)width, (float)height),
                                   (float[4]){1, 1, 1, 1},
                                   (GdkRGBA[4]){border_color, border_color, border_color, border_color});
    }

    /* Draw Right Margin */
    if (self->show_right_margin) {
        /* Calculate position based on character width approx or exact? 
           Let's use avg char width from font metrics */
        PangoContext *ctx = gtk_widget_get_pango_context(widget);
        PangoFontMetrics *metrics = pango_context_get_metrics(ctx, self->font_desc, NULL);
        int char_width = pango_font_metrics_get_approximate_char_width(metrics);
        pango_font_metrics_unref(metrics);
        
        double margin_x = text_start_x + (self->right_margin_position * pango_units_to_double(char_width));
        
        GdkRGBA margin_col = self->color_text;
        margin_col.alpha = 0.03;
        
        float margin_width = (float)width - (float)margin_x;
        if (margin_width < 0) margin_width = 0;
        
        gtk_snapshot_append_color(snapshot, &margin_col, 
            &GRAPHENE_RECT_INIT((float)margin_x, 0, margin_width, (float)height));
        
        /* Optional: Draw a slightly stronger line at the edge? */
        GdkRGBA line_col = self->color_text;
        line_col.alpha = 0.06;
        gtk_snapshot_append_color(snapshot, &line_col, 
            &GRAPHENE_RECT_INIT((float)margin_x, 0, 1.0f, (float)height));
    }

    /* Cleanup cursor_lines if heap allocated */
    if (cursor_lines != cursor_lines_stack) {
        g_free(cursor_lines);
    }
    
    gtk_snapshot_pop(snapshot); /* Pop clip from beginning */
    
    /* Draw Minimap Overlay */
    if (self->minimap_enabled) {
        editor_minimap_draw(self, snapshot, minimap_x, 0, minimap_w, height);
    }
}
