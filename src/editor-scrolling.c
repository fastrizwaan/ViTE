#include "editor-internal.h"
#include <math.h>

#ifndef MAX_PANGO_LINE_LEN
#define MAX_PANGO_LINE_LEN 10485760
#endif

/* Scroll Calculation Helper */
typedef struct {
    double current_y;
    GArray *offsets;
    int chars_per_line;
    double line_height;
} ScrollCalcState;

/* Max line width calculation helper */
typedef struct {
    size_t max_len_bytes;
} MaxWidthCalcState;

static void
calculate_max_line_len_cb(size_t len, void *user_data)
{
    MaxWidthCalcState *state = (MaxWidthCalcState*)user_data;
    if (len > state->max_len_bytes) {
        state->max_len_bytes = len;
    }
}

static void
calculate_line_height_cb(size_t len, void *user_data)
{
    ScrollCalcState *state = (ScrollCalcState*)user_data;
    
    size_t effective_len = len;
    if (effective_len > 0) effective_len--; /* approximate removing newline */
    
    size_t visual_lines = 1;
    if (state->chars_per_line > 0 && effective_len > 0) {
        visual_lines = (effective_len + state->chars_per_line - 1) / state->chars_per_line;
        if (visual_lines == 0) visual_lines = 1;
    }
    
    g_array_append_val(state->offsets, state->current_y);
    state->current_y += (double)visual_lines * state->line_height;
}

static double
calculate_total_content_height(EditorWidget *self, int widget_width, int widget_height)
{
    size_t total_lines = get_visual_line_count(self);
    double content_height = 0;

    if (!self->wrap_lines || total_lines == 0) {
        /* Simple calculation for no-wrap mode or empty doc */
        content_height = (double)total_lines * self->line_height + self->padding_top * 2;
        
        if (self->line_y_offsets) {
             g_array_set_size(self->line_y_offsets, 0);
        }
    } else {
        /* Accurate calculation for wrapped lines */
        double text_start_x = get_effective_gutter_width(self) + self->padding_left;
        
        double minimap_w = 0;
        if (self->minimap_enabled) {
            minimap_w = self->minimap_width;
            if (minimap_w > (double)widget_width / 2.0) minimap_w = (double)widget_width / 2.0;
        }
        
        /* Use active_right_padding instead of hardcoded 20.0 */
        double wrap_width = (double)widget_width - text_start_x - (double)self->active_right_padding - minimap_w;
        
        if (wrap_width < 1.0) wrap_width = 1.0;
        
        /* Estimate chars per line using cached char width */
        int chars_per_line = (int)(wrap_width / self->cached_char_width);
        if (chars_per_line < 1) chars_per_line = 1;

        g_array_set_size(self->line_y_offsets, 0);
        
        /* Strategy Switch: Exact vs Statistical */
        if (total_lines > 1500) {
            double total_sample_height = 0;
            int samples = 50;
            if ((size_t)samples > total_lines) samples = (int)total_lines;
            
            int step = total_lines / samples;
            if (step < 1) step = 1;
            
            int actual_samples = 0;
            for (int i = 0; i < samples; i++) {
                size_t idx = i * step;
                if (idx >= total_lines) break;
                size_t phys_idx = get_physical_line_index(self, idx);
                if (phys_idx == (size_t)-1) continue;
                size_t line_len_bytes = document_get_line_length(self->doc, phys_idx);
                
                /* Optimization: For massive lines, estimate height to avoid Pango stall */
                if (line_len_bytes > 4096) {
                    /* Estimate visual lines (Simple Char Wrap) */
                    size_t visual_lines = 1;
                    if (chars_per_line > 0) {
                        visual_lines = (line_len_bytes + chars_per_line - 1) / chars_per_line;
                    }
                     total_sample_height += (double)visual_lines * self->line_height;
                     actual_samples++;
                } else {
                    char *text = NULL;
                    PangoLayout *layout = create_pango_layout_for_line(self, phys_idx, &text, NULL);
                    if (layout) {
                        /* Must set width to simulate wrapping for accurate height */
                         pango_layout_set_width(layout, (int)(wrap_width * PANGO_SCALE));
                         pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

                        PangoRectangle logical;
                        pango_layout_get_extents(layout, NULL, &logical);
                        double h = (double)logical.height / PANGO_SCALE;
                        if (h < self->line_height) h = self->line_height;
                        total_sample_height += h;
                        actual_samples++;
                        g_object_unref(layout);
                    }
                    if (text) g_free(text);
                }
            }
            
            double avg_height = (actual_samples > 0) ? (total_sample_height / actual_samples) : self->line_height;
            self->avg_visual_lines = avg_height / self->line_height;
            content_height = (double)total_lines * avg_height + self->padding_top * 2;
            
        } else {
            /* Exact Scan Mode (O(N)) */
            self->avg_visual_lines = 1.0; /* Reset */
            
            ScrollCalcState state;
            state.current_y = 0;
            state.offsets = self->line_y_offsets;
            state.chars_per_line = chars_per_line;
            state.line_height = self->line_height;
            
            if (self->visible_lines && self->visible_lines->data) {
                size_t count = compact_matches_count(self->visible_lines);
                for (guint i = 0; i < count; i++) {
                    size_t phys_idx = self->visible_lines->data ? self->visible_lines->data[i] : (size_t)-1;
                    if (phys_idx == (size_t)-1) continue;
                    size_t line_len = document_get_line_length(self->doc, phys_idx);
                    calculate_line_height_cb(line_len, &state);
                }
            } else if (self->filtered_lines && self->filtered_lines->data) {
                size_t count = compact_matches_count(self->filtered_lines);
                for (guint i = 0; i < count; i++) {
                    size_t phys_idx = self->filtered_lines->data ? self->filtered_lines->data[i] : (size_t)-1;
                    if (phys_idx == (size_t)-1) continue;
                    size_t line_len = document_get_line_length(self->doc, phys_idx);
                    calculate_line_height_cb(line_len, &state);
                }
            } else {
                document_foreach_line(self->doc, calculate_line_height_cb, &state);
            }
            
            while (self->line_y_offsets->len < total_lines) {
                g_array_append_val(self->line_y_offsets, state.current_y);
                state.current_y += self->line_height;
            }
            
            g_array_append_val(self->line_y_offsets, state.current_y);
            content_height = state.current_y + self->padding_top * 2;
        }
    }
    
    return content_height;
}

void
editor_widget_update_adjustments(EditorWidget *self, int widget_width, int widget_height)
{
    if (!self->vadjustment || !self->doc) return;
    
    /* If called with -1, use current */
    if (widget_width < 0) widget_width = gtk_widget_get_width(GTK_WIDGET(self));
    if (widget_height < 0) widget_height = gtk_widget_get_height(GTK_WIDGET(self));

    editor_widget_ensure_metrics(self);

    /* Dynamic Margin Pass 1: Assume 0 padding */
    self->active_right_padding = 0;
    double content_height = calculate_total_content_height(self, widget_width, widget_height);
    
    /* Check if scrollbar is needed */
    if (content_height > (double)widget_height) {
        /* Scrollbar needed! Switch to 20 padding */
        self->active_right_padding = 20;
        /* Re-calculate height with new width constraint */
        content_height = calculate_total_content_height(self, widget_width, widget_height);
    }
    
    /* Remove overscroll to prevent scrollbar from appearing when content fits */
    double upper = MAX(content_height, (double)widget_height);

    gtk_adjustment_configure(self->vadjustment,
                             gtk_adjustment_get_value(self->vadjustment),
                             0,
                             upper,
                             self->line_height,      /* step */
                             (double)widget_height,          /* page */
                             (double)widget_height);         /* page_size */

    /* Horizontal Adjustment */
    double content_width = 0;
    
    if (self->wrap_lines) {
        content_width = (double)widget_width;
    } else {
        size_t total_lines = get_visual_line_count(self);
        if (total_lines < 50000) {
            MaxWidthCalcState mw_state = {0};
            document_foreach_line(self->doc, calculate_max_line_len_cb, &mw_state);
            
            double gutter = get_effective_gutter_width(self);
            content_width = (double)mw_state.max_len_bytes * self->cached_char_width + gutter + self->padding_left * 2 + 50;
        } else {
            content_width = (double)widget_width * 2.0; 
        }
    }
    
    if (content_width < (double)widget_width) content_width = (double)widget_width;

    if (self->hadjustment) {
        gtk_adjustment_configure(self->hadjustment,
                                 gtk_adjustment_get_value(self->hadjustment),
                                 0,
                                 content_width,
                                 self->cached_char_width, /* step */
                                 (double)widget_width,           /* page */
                                 (double)widget_width);          /* page_size */
    }
}

void
scroll_to_cursor(EditorWidget *self)
{
    /* Keep it simple for now, relies on update_adjustments to set ranges */
    /* And we might not have a good way to know exact pixel Y of cursor without layout */
    /* Assuming "ensure visible" logic is needed here */
    
    EditorCursor *cur = editor_widget_get_primary_cursor(self);
    if (!cur || !self->vadjustment) return;
    
    size_t line_idx = document_get_line_of_offset(self->doc, cur->cursor_offset);
    size_t line_start_offset = document_get_offset_of_line(self->doc, line_idx);
    size_t offset_in_line = (cur->cursor_offset >= line_start_offset)
        ? (cur->cursor_offset - line_start_offset)
        : 0;
    
    /* Find visual Y */
    double y = 0;
    if (self->line_y_offsets && line_idx < self->line_y_offsets->len) {
        y = g_array_index(self->line_y_offsets, double, line_idx);
    } else {
        /* Fallback for large files where offsets aren't cached: use average */
        if (self->wrap_lines && self->avg_visual_lines > 1.0) {
            y = (double)line_idx * self->avg_visual_lines * self->line_height;
        } else {
            y = (double)line_idx * self->line_height;
        }
    }

    /* If wrapping, adjust y to the cursor's visual row within the line. */
    if (self->wrap_lines) {
        int widget_width = gtk_widget_get_width(GTK_WIDGET(self));
        double text_start_x = get_effective_gutter_width(self) + self->padding_left;
        double minimap_w = 0;
        if (self->minimap_enabled) {
            minimap_w = self->minimap_width;
            if (minimap_w > (double)widget_width / 2.0) minimap_w = (double)widget_width / 2.0;
        }
        double wrap_width = (double)widget_width - text_start_x - (double)self->active_right_padding - minimap_w;
        if (wrap_width < 1.0) wrap_width = 1.0;
        double cw = (self->cached_char_width > 1.0) ? self->cached_char_width : 8.0;
        int chars_per_line = (int)(wrap_width / cw);
        if (chars_per_line < 1) chars_per_line = 1;
        size_t row = offset_in_line / (size_t)chars_per_line;
        y += (double)row * self->line_height;
    }
    
    double page_size = gtk_adjustment_get_page_size(self->vadjustment);
    double value = gtk_adjustment_get_value(self->vadjustment);
    
    /* Add extra padding at bottom for overlaid find bar (~80px = ~3-4 lines) */
    double bottom_padding = self->line_height * 4;
    
    if (y < value) {
        gtk_adjustment_set_value(self->vadjustment, y);
    } else if (y + self->line_height + bottom_padding > value + page_size) {
        gtk_adjustment_set_value(self->vadjustment, y + self->line_height + bottom_padding - page_size);
    }
    /* Else visible */
}

void
editor_widget_scroll_to_cursor(EditorWidget *self)
{
    scroll_to_cursor(self);
}

gboolean
editor_on_scroll(GtkEventControllerScroll *controller G_GNUC_UNUSED, double dx G_GNUC_UNUSED, double dy, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    
    if (!self->vadjustment) return GDK_EVENT_PROPAGATE;
    
    double current = gtk_adjustment_get_value(self->vadjustment);
    double upper = gtk_adjustment_get_upper(self->vadjustment);
    double page = gtk_adjustment_get_page_size(self->vadjustment);
    
    double step = self->line_height * 4; /* Scroll 4 lines per wheel tick */
    
    double new_val = current + (dy * step);
    new_val = CLAMP(new_val, 0, upper - page);
    
    gtk_adjustment_set_value(self->vadjustment, new_val);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    
    return GDK_EVENT_STOP; 
}

gboolean
editor_resize_idle_cb(gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    self->idle_resize_id = 0;
    
    /* Pass -1, -1 to use current size */
    editor_widget_update_adjustments(self, -1, -1);
    
    return G_SOURCE_REMOVE;
}

gboolean
autoscroll_tick(gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    
    if (!self->vadjustment || self->autoscroll_direction == 0) {
        self->autoscroll_timer_id = 0;
        return G_SOURCE_REMOVE;
    }
    
    self->autoscroll_tick_count++;
    
    double current_val = gtk_adjustment_get_value(self->vadjustment);
    double upper = gtk_adjustment_get_upper(self->vadjustment);
    double page_size = gtk_adjustment_get_page_size(self->vadjustment);
    
    double scale = 1.0;
    size_t top_line = (size_t)(current_val / self->line_height);
    size_t phys_top = get_physical_line_index(self, top_line);
    if (phys_top != (size_t)-1 && phys_top < document_get_line_count(self->doc)) {
        char *text = NULL;
        size_t len;
        PangoLayout *layout = create_pango_layout_for_line(self, phys_top, &text, &len);
        if (layout) {
            int h;
            pango_layout_get_pixel_size(layout, NULL, &h);
            double real_h = (double)h;
            if (real_h > self->line_height) {
                scale = self->line_height / real_h;
            }
            g_object_unref(layout);
            g_free(text);
        }
    }
    
    double new_val = current_val + (self->autoscroll_direction * self->autoscroll_speed * scale);
    new_val = CLAMP(new_val, 0, upper - page_size);
    
    if (new_val != current_val) {
        gtk_adjustment_set_value(self->vadjustment, new_val);
    } else {
        stop_autoscroll(self);
        return G_SOURCE_REMOVE;
    }
    
    if (self->autoscroll_tick_count % 15 == 0) {
        if (self->is_dnd_active) {
            /* Update drop offset based on new scroll position */
            size_t drop_off;
            editor_widget_get_offset_at_point(self, self->drag_x, self->drag_y, &drop_off);
            
            EditorCursor *primary = editor_widget_get_primary_cursor(self);
            size_t sel_start = MIN(primary->cursor_offset, primary->selection_anchor);
            size_t sel_end = MAX(primary->cursor_offset, primary->selection_anchor);
            
            if (drop_off >= sel_start && drop_off < sel_end) {
                self->drag_drop_offset = (size_t)-1;
            } else {
                self->drag_drop_offset = drop_off;
            }
        } else {
             size_t off;
             editor_widget_get_offset_at_point(self, self->drag_x, self->drag_y, &off);
             update_selection_extension(self, off);
        }
    }
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
    
    return G_SOURCE_CONTINUE;
}

void
stop_autoscroll(EditorWidget *self)
{
    if (self->autoscroll_timer_id) {
        g_source_remove(self->autoscroll_timer_id);
        self->autoscroll_timer_id = 0;
    }
    self->autoscroll_direction = 0;
    self->autoscroll_speed = 0;
}

void
start_autoscroll(EditorWidget *self, int direction, double speed)
{
    self->autoscroll_direction = direction;
    self->autoscroll_speed = speed;
    self->autoscroll_tick_count = 0;
    
    if (!self->autoscroll_timer_id) {
        self->autoscroll_timer_id = g_timeout_add(16, autoscroll_tick, self);
    }
}

void
editor_widget_scrollable_init(GtkScrollableInterface *iface G_GNUC_UNUSED)
{
}

GtkAdjustment *
editor_widget_get_vadjustment(EditorWidget *self)
{
    g_return_val_if_fail(EDITOR_IS_WIDGET(self), NULL);
    return self->vadjustment;
}

void
editor_widget_scroll_to_line(EditorWidget *self, size_t line)
{
    if (!self->doc) return;

    size_t total_lines = document_get_line_count(self->doc);
    if (line >= total_lines) {
        if (total_lines > 0) line = total_lines - 1;
        else line = 0;
    }

    double target_y = 0;
    editor_widget_ensure_metrics(self);

    if (self->wrap_lines && total_lines > 50000) {
        target_y = (double)line * self->avg_visual_lines * self->line_height;
    } else {
        editor_widget_update_adjustments(self, -1, -1);
        
        if (self->line_y_offsets && line < self->line_y_offsets->len) {
            target_y = g_array_index(self->line_y_offsets, double, line);
        } else {
            target_y = (double)line * self->line_height;
        }
    }
    
    if (self->vadjustment) {
        gtk_adjustment_set_value(self->vadjustment, target_y);
    }
    
    /* Move cursor to start of line */
    size_t line_start_offset = document_get_offset_of_line(self->doc, line);
    
    if (self->cursors) {
        g_array_set_size(self->cursors, 0);
        EditorCursor c;
        c.cursor_offset = line_start_offset;
        c.selection_anchor = line_start_offset;
        c.target_x = -1;
        g_array_append_val(self->cursors, c);
    }
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
editor_widget_get_visible_line_range(EditorWidget *self, size_t *start, size_t *end)
{
    g_return_if_fail(EDITOR_IS_WIDGET(self));
    
    if (!self->vadjustment || self->line_height <= 0) {
        if (start) *start = 0;
        if (end) *end = 0;
        return;
    }
    
    double val = gtk_adjustment_get_value(self->vadjustment);
    double page_size = gtk_adjustment_get_page_size(self->vadjustment);
    
    size_t s = 0;
    
    if (self->line_y_offsets && self->line_y_offsets->len > 0) {
        double *offsets = (double*)self->line_y_offsets->data;
        guint low = 0;
        guint high = self->line_y_offsets->len - 1;
        
        while (low < high) {
            guint mid = low + (high - low + 1) / 2;
            if (offsets[mid] <= val) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        s = low;
    } else {
        double effective_line_h = self->line_height;
        if (self->wrap_lines) {
            double avg = self->avg_visual_lines;
            if (avg < 1.0) avg = 1.0;
            effective_line_h = self->line_height * avg;
        }
        if (effective_line_h < 1.0) effective_line_h = self->line_height;
        s = (size_t)(val / effective_line_h);
    }

    double effective_line_h = self->line_height;
    if (self->wrap_lines) {
        double avg = self->avg_visual_lines;
        if (avg < 1.0) avg = 1.0;
        effective_line_h = self->line_height * avg;
    }
    if (effective_line_h < 1.0) effective_line_h = self->line_height;
    size_t lines_visible = (size_t)(page_size / effective_line_h) + 5; 
    size_t e = s + lines_visible;
    
    size_t total = get_visual_line_count(self);
    
    if (s > total) s = total;
    if (e > total) e = total;
    
    if (start) *start = s;
    if (end) *end = e;
}

void
editor_widget_get_visible_offset_range(EditorWidget *self, size_t *start_offset, size_t *end_offset)
{
    g_return_if_fail(EDITOR_IS_WIDGET(self));
    
    size_t start_line, end_line;
    editor_widget_get_visible_line_range(self, &start_line, &end_line);
    
    if (!self->doc) {
        if (start_offset) *start_offset = 0;
        if (end_offset) *end_offset = 0;
        return;
    }
    
    if (start_offset) {
        size_t phys_start = get_physical_line_index(self, start_line);
        if (phys_start == (size_t)-1) phys_start = 0;
        *start_offset = document_get_offset_of_line(self->doc, phys_start);
    }
    if (end_offset) {
        size_t phys_end = get_physical_line_index(self, end_line);
        if (phys_end == (size_t)-1) phys_end = document_get_line_count(self->doc);
        *end_offset = document_get_offset_of_line(self->doc, phys_end);
    }
}
