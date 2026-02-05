#include "editor-minimap.h"
#include "editor-internal.h"
#include <gtk/gtk.h>
#include <math.h>

void
editor_minimap_get_params(EditorWidget *self, double viewport_h,
                          double *out_map_content_h,
                          double *out_map_scroll_y,
                          double *out_map_line_h)
{
    /* Minimap metrics */
    int block_h = self->minimap_block_height;
    if (block_h < 1) block_h = 2;
    int spacing = 1; /* Fixed 1px spacing */
    double map_line_h = (double)(block_h + spacing);

    if (out_map_line_h) *out_map_line_h = map_line_h;

    size_t total_lines;
    if (self->doc) {
        // Use visual line count when word wrap is enabled, otherwise physical line count
        if (self->wrap_lines) {
            total_lines = get_visual_line_count(self);
        } else {
            total_lines = document_get_line_count(self->doc);
        }
    } else {
        total_lines = 1;
    }

    double minimap_content_h = total_lines * map_line_h;
    if (out_map_content_h) *out_map_content_h = minimap_content_h;

    /* Calculate Scroll Y */
    double map_scroll_y = 0;

    double editor_h = gtk_widget_get_height(GTK_WIDGET(self));
    double editor_line_h = self->line_height;

    double total_editor_h = 0;
    #define MIN_EDITOR_HEIGHT 1.0
    if (self->vadjustment) {
        total_editor_h = gtk_adjustment_get_upper(self->vadjustment);
    }
    if (total_editor_h < MIN_EDITOR_HEIGHT) total_editor_h = total_lines * editor_line_h;

    double scroll_y = 0;
    if (self->vadjustment) scroll_y = gtk_adjustment_get_value(self->vadjustment);

    /*
     * Map Editor's scroll position to Minimap's scroll position using proportional mapping.
     * This ensures that when the editor is scrolled to the end, the minimap lens also reaches the end.
     *
     * The key insight is to use the same proportional relationship that the editor's scrollbar uses:
     * - Editor scroll position ranges from 0 to (upper - page_size)
     * - We map this proportionally to the minimap's scroll range
     */

    double editor_max_scroll = 0;
    if (self->vadjustment) {
        double page_size = gtk_adjustment_get_page_size(self->vadjustment);
        editor_max_scroll = gtk_adjustment_get_upper(self->vadjustment) - page_size;
    }

    // Handle edge case where editor can't scroll
    if (editor_max_scroll <= 0) {
        if (out_map_scroll_y) *out_map_scroll_y = 0;
        return;
    }

    // Calculate the scroll ratio based on editor's current scroll position
    double scroll_ratio = scroll_y / editor_max_scroll;

    // Clamp the ratio to valid range [0, 1]
    if (scroll_ratio < 0) scroll_ratio = 0;
    if (scroll_ratio > 1) scroll_ratio = 1;

    // Calculate minimap scroll position based on the ratio
    double max_map_scroll = minimap_content_h - viewport_h;
    if (max_map_scroll > 0) {
        map_scroll_y = scroll_ratio * max_map_scroll;
    } else {
        // If minimap content fits in viewport, center it or keep at top
        // For dragging to work properly, we still need to maintain the proportional relationship
        // But since the content is smaller than viewport, the scroll position should be 0
        map_scroll_y = 0;
    }

    // Clamp map_scroll_y to valid range
    if (map_scroll_y < 0) map_scroll_y = 0;
    if (max_map_scroll > 0 && map_scroll_y > max_map_scroll) {
        map_scroll_y = max_map_scroll;
    }

    if (out_map_scroll_y) *out_map_scroll_y = map_scroll_y;
}

void
editor_minimap_draw(EditorWidget *self, GtkSnapshot *snapshot, double x, double y, double w, double h)
{
    if (!self->doc || !self->minimap_enabled) return;

    /* Draw Background */
    GdkRGBA bg_color = {0.98, 0.98, 0.98, 1.0};
    if (self->color_text.red > 0.5) { /* Dark theme detection */
        bg_color = (GdkRGBA){0.15, 0.15, 0.15, 1.0};
    }
    gtk_snapshot_append_color(snapshot, &bg_color, &GRAPHENE_RECT_INIT(x, y, w, h));

    /* Clip to minimap area */
    gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(x, y, w, h));
    gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(x, y));

    /* Get shared params */
    double map_content_h, map_scroll_y, map_line_h;
    editor_minimap_get_params(self, h, &map_content_h, &map_scroll_y, &map_line_h);

    double editor_line_h = self->line_height;
    double scroll_y = 0;
    if (self->vadjustment) scroll_y = gtk_adjustment_get_value(self->vadjustment);
    double editor_h = gtk_widget_get_height(GTK_WIDGET(self));

    /* Calculate start line index */
    size_t start_line = (size_t)(map_scroll_y / map_line_h);
    double partial_map_y = fmod(map_scroll_y, map_line_h);

    /* Lines to render */
    size_t lines_to_draw = (size_t)(h / map_line_h) + 2;

    // Use visual line count when word wrap is enabled, otherwise physical line count
    size_t total_lines;
    if (self->wrap_lines) {
        total_lines = get_visual_line_count(self);
    } else {
        total_lines = document_get_line_count(self->doc);
    }

    /* Draw Lens */
    /* Draw Lens based on Visual/Physical Visible Range */
    size_t vis_start, vis_end;
    editor_widget_get_visible_line_range(self, &vis_start, &vis_end);

    /* Ensure minimal height of 1 line */
    if (vis_end <= vis_start) vis_end = vis_start + 1;

    double lens_y = (double)vis_start * map_line_h - map_scroll_y;
    double lens_h = (double)(vis_end - vis_start) * map_line_h;

    /* Clamp lens */
    /* If lens is outside viewport, clamp drawing */
    if (lens_y < 0) {
        lens_h += lens_y;
        lens_y = 0;
    }
    if (lens_y + lens_h > h) {
        lens_h = h - lens_y;
    }

    /* Ensure lens is visible even when document is small */
    if (lens_h < 5.0) lens_h = 5.0;  /* Minimum lens height for usability */

    /* Ensure lens height doesn't exceed viewport */
    if (lens_h > h) lens_h = h;

    if (lens_h > 0) {
        GdkRGBA lens_col = {0.5, 0.5, 0.5, 0.1};
        if (self->minimap_active) lens_col.alpha = 0.2;
        gtk_snapshot_append_color(snapshot, &lens_col, &GRAPHENE_RECT_INIT(0, (float)lens_y, w, (float)lens_h));
    }

    int block_h = (int)map_line_h - 1; /* Spacing is 1 */

    /* Iterate and draw lines */
    double current_y = -partial_map_y;

    for (size_t i = 0; i < lines_to_draw; i++) {
        size_t line_idx = start_line + i;
        if (line_idx >= total_lines) break;

        /* Optimization: Just skip if Y is totally out?
           lines_to_draw handles the bottom bound.
           start_line handles the top.
        */

        size_t len;
        char *text;

        // Convert visual line index to physical line index if needed
        size_t physical_line_idx;
        if (self->wrap_lines) {
            physical_line_idx = get_physical_line_index(self, line_idx);
            if (physical_line_idx == (size_t)-1) continue; // Invalid line
        } else {
            physical_line_idx = line_idx;
        }

        text = document_get_line_truncated(self->doc, physical_line_idx, &len, 1024);
        if (!text) continue;

        while (len > 0 && (text[len-1] == '\n' || text[len-1] == '\r')) len--;

        if (len > 0) {
            PangoAttrList *attrs = syntax_highlight_line(self->syntax_ctx, physical_line_idx, text);
            PangoAttrIterator *iter = pango_attr_list_get_iterator(attrs);

            do {
                int start, end;
                pango_attr_iterator_range(iter, &start, &end);
                if (start >= len) break;
                if (end > len) end = len;

                GdkRGBA fg = self->color_text;
                PangoAttribute *fg_attr = pango_attr_iterator_get(iter, PANGO_ATTR_FOREGROUND);
                if (fg_attr) {
                    PangoColor *pc = &((PangoAttrColor*)fg_attr)->color;
                    fg.red = pc->red / 65535.f;
                    fg.green = pc->green / 65535.f;
                    fg.blue = pc->blue / 65535.f;
                }
                fg.alpha = 0.6;

                double draw_x = 2;

                for (int c = start; c < end; c++) {
                    char ch = text[c];
                    if (ch == ' ' || ch == '\t') {
                        draw_x += (ch == '\t' ? (self->tab_width * 2) : 2);
                        continue;
                    }

                    double char_x = draw_x + (c * 2);

                    if (char_x < w) {
                        gtk_snapshot_append_color(snapshot, &fg,
                            &GRAPHENE_RECT_INIT((float)char_x, (float)current_y, 2.0f, (float)block_h));
                    }
                }
            } while (pango_attr_iterator_next(iter));

            pango_attr_iterator_destroy(iter);
            if (attrs) pango_attr_list_unref(attrs);
            g_free(text);
        }
        current_y += map_line_h;
    }

    gtk_snapshot_pop(snapshot);
}
