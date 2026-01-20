#include "editor-internal.h"
#include <gtk/gtk.h>
#include <math.h>
#include <string.h>
#include "syntax.h"

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
    
    /* Clip to visible area to prevent drawing over splitters */
    gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(0, 0, (float)width, (float)height));
    
    /* Ensure background is cleared/drawn to prevent drag artifacts */
    GdkRGBA bg_color = {1, 1, 1, 1}; /* Default white */
    if (self->color_text.red > 0.5 && self->color_text.green > 0.5 && self->color_text.blue > 0.5) {
         /* Text is light -> Background should be dark */
         bg_color = (GdkRGBA){0.11, 0.11, 0.11, 1.0}; /* #1e1e1e approx */
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

    for (size_t i = 0; i < count_lines; ++i) {
        size_t visual_line_idx = start_line + i;
        if (visual_line_idx >= max_lines) break;

        size_t phys_line = get_physical_line_index(self, visual_line_idx);
        if (phys_line == (size_t)-1) continue;

        size_t len;
        char *text = document_get_line_truncated(self->doc, phys_line, &len, MAX_PANGO_LINE_LEN + 1024);
        // fprintf(stderr, "[DEBUG] snapshot loop: len=%zu\n", len);

        /* UTF-8 Validation */
        if (!g_utf8_validate(text, len, NULL)) {
             char *safe_text = g_utf8_make_valid(text, len);
             g_free(text);
             text = safe_text;
             len = strlen(text);
        }
        
        /* Strip trailing newlines for Pango render (\n, \r\n, \r) */
        while (len > 0 && (text[len-1] == '\n' || text[len-1] == '\r')) {
            len--;
        }

        PangoLayout *layout = pango_layout_new(context);
        pango_layout_set_font_description(layout, self->font_desc);
        int pango_len = (len > MAX_PANGO_LINE_LEN) ? MAX_PANGO_LINE_LEN : (int)len;
        pango_layout_set_text(layout, text, pango_len);
        
        /* Word Wrap - account for gutter and padding */
        if (self->wrap_lines) {
            int available_w = width - text_start_x - 20; /* 20px buffer for scrollbar */
            if (available_w < 50) available_w = 50; /* Safe min width */
            pango_layout_set_width(layout, available_w * PANGO_SCALE);
            pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        } else {
            pango_layout_set_width(layout, -1);
            pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        }
        
        /* Syntax highlight and Search Highlight */
        /* MUST COPY cached attributes to avoid corrupting the syntax cache with search highlights! */
        PangoAttrList *cached_attrs = syntax_highlight_line(self->syntax_ctx, phys_line, text);
        PangoAttrList *attrs = cached_attrs ? pango_attr_list_copy(cached_attrs) : pango_attr_list_new();
        
        /* Inject Search Highlights */
        if (self->search_matches && self->search_matches->len > 0) {
            size_t line_start_off = document_get_offset_of_line(self->doc, phys_line);
            size_t raw_line_end = line_start_off + len;
            
            /* Binary search to find first match that could overlap this line */
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
                    if (match.start >= raw_line_end) break;
                    
                    if (match.start < raw_line_end && match.end > line_start_off) {
                        size_t local_start = MAX(match.start, line_start_off) - line_start_off;
                        size_t local_end = MIN(match.end, raw_line_end) - line_start_off;
                        
                        if (local_start < local_end) {
                            /* Yellow: 65535, 65535, 0 */
                            guint16 r = 65535, g = 65535, b = 0;
                            if (m == self->current_match_idx) {
                                /* Orange for active: 65535, 40000, 0 */
                                g = 40000; b = 0;
                            }
                            
                            PangoAttribute *bg = pango_attr_background_new(r, g, b);
                            bg->start_index = (guint)local_start;
                            bg->end_index = (guint)local_end;
                            pango_attr_list_change(attrs, bg);
                            
                            /* Alpha: 0.5 -> 32768 */
                            PangoAttribute *alpha = pango_attr_background_alpha_new(32768);
                            alpha->start_index = (guint)local_start;
                            alpha->end_index = (guint)local_end;
                            pango_attr_list_change(attrs, alpha);
                        }
                    }
                }
            }
        }
        
        pango_layout_set_attributes(layout, attrs);
        pango_attr_list_unref(attrs);
        
        /* Calculate height of this layout */
        int pixel_h;
        pango_layout_get_pixel_size(layout, NULL, &pixel_h);
        double real_layout_h = (double)pixel_h;
        double layout_h = real_layout_h;
        if (layout_h < self->line_height) layout_h = self->line_height; /* Min height */
        
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

        /* Draw Line Number */
        if (self->show_line_numbers) {
            char lnum_buf[32];
            snprintf(lnum_buf, sizeof(lnum_buf), "%zu", phys_line + 1);
            
            PangoLayout *lnum_layout = pango_layout_new(context);
            pango_layout_set_font_description(lnum_layout, self->font_desc);
            
            pango_layout_set_text(lnum_layout, lnum_buf, -1);
            pango_layout_set_alignment(lnum_layout, PANGO_ALIGN_RIGHT);
            /* Width = gutter_w - right_padding(4) - left_padding(4) */
            pango_layout_set_width(lnum_layout, (int)((gutter_w - 8) * PANGO_SCALE));
            
            /* Gutter text color - dim it */
            GdkRGBA gutter_fg = self->color_text;
            gutter_fg.alpha = 0.5;
            
            gtk_snapshot_save(snapshot);
            /* Translate X=4 for 4px left padding. Use centering_offset for consistent alignment. */
            gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(4, current_y_pos + self->padding_top + centering_offset));
            gtk_snapshot_append_layout(snapshot, lnum_layout, &gutter_fg);
            gtk_snapshot_restore(snapshot);
            
            g_object_unref(lnum_layout);
        }
        
        // fprintf(stderr, "[DEBUG] snapshot loop: line_idx=%zu done\n", line_idx);

        gtk_snapshot_save(snapshot);
        /* Clip text area to ensure it doesn't draw over the gutter */
        gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(gutter_w, 0, width - gutter_w, height));
        
        /* Apply centering_offset to the text drawing translation */
        gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(text_start_x - scroll_x, current_y_pos + self->padding_top + centering_offset));
        
        /* Draw Line Background if selected */
        /* Selection rendering across lines is complex. 
           Simplified: If line is fully selected or partially.
        */
        
        /* 1. Draw Selections for all cursors */
        size_t line_start_off = document_get_offset_of_line(self->doc, phys_line);
        size_t raw_line_end = line_start_off + len + 1;

        for (guint c = 0; c < self->cursors->len; c++) {
            EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
            
            size_t start_sel = MIN(cur->cursor_offset, cur->selection_anchor);
            size_t end_sel = MAX(cur->cursor_offset, cur->selection_anchor);
            
            if (start_sel < raw_line_end && end_sel > line_start_off && start_sel != end_sel) {
                size_t sel_in_line_start = MAX(start_sel, line_start_off) - line_start_off;
                size_t sel_in_line_end = MIN(end_sel, (line_start_off + len)) - line_start_off;
                
                if (len == 0) {
                    if (end_sel > line_start_off) {
                        /* For empty lines, draw selection block using layout_h */
                        gtk_snapshot_append_color(snapshot, 
                                                  &(GdkRGBA){0.2, 0.4, 0.8, 0.35},
                                                  &GRAPHENE_RECT_INIT(0, 0, (float)width, (float)real_layout_h));
                    }
                } else {
                    PangoLayoutIter *iter = pango_layout_get_iter(layout);
                    do {
                        PangoLayoutLine *p_line = pango_layout_iter_get_line_readonly(iter);
                        int line_start_index = p_line->start_index;
                        int line_end_index = line_start_index + p_line->length;
                        
                        PangoRectangle line_rect;
                        pango_layout_iter_get_line_extents(iter, NULL, &line_rect);
                        double ry = pango_units_to_double(line_rect.y);
                        /* Use logical line height directly instead of copying iterator to peek ahead */
                        double rh = pango_units_to_double(line_rect.height);

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
                    } while (pango_layout_iter_next_line(iter));
                    pango_layout_iter_free(iter);
                }
            }
        }
        
        /* 2. Draw Text */
        gtk_snapshot_append_layout(snapshot, layout, &self->color_text);
        
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
                     double pango_y = pango_units_to_double(strong_pos.y);
                     
                     double cursor_h = MAX(pango_h, self->line_height);
                     double cursor_y = pango_y - (cursor_h - pango_h) / 2.0;
                     
                     /* Draw 4 times to accumulate opacity and force "sharpness" on sub-pixels */
                     for (int pass = 0; pass < 4; pass++) {
                        gtk_snapshot_append_color(snapshot, &cursor_color, &GRAPHENE_RECT_INIT(cursor_x, (float)((int)(cursor_y + 0.5)), cursor_w, (float)((int)cursor_h)));
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
                double pango_y = pango_units_to_double(strong_pos.y);
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
        current_y_pos += layout_h;
        
        gtk_snapshot_pop(snapshot);
        gtk_snapshot_restore(snapshot);
        g_object_unref(layout);
        g_free(text);

        if (current_y_pos > height) {
             break;
        }
    }
    // fprintf(stderr, "[DEBUG] snapshot: Loop finished\n");

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
    
    gtk_snapshot_pop(snapshot);
}
