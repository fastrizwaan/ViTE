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

typedef struct {
    EditorWidget *self;
    GtkSnapshot *snapshot;
    PangoContext *pango_ctx;
    
    int width;
    int height;
    
    double scroll_x;
    double scroll_y;
    
    double gutter_w;
    double text_start_x;
    double minimap_w;
    
    gboolean is_dark;
    GdkRGBA bg_color;
    
    size_t start_line;
    double partial_y;
    size_t count_lines;
    
    size_t *cursor_lines;
    size_t num_cursors;
    
    PangoTabArray *tab_array;
    PangoLayout *line_layout;
    PangoLayout *lnum_layout;
    PangoLayout *fold_layout;
    PangoLayout *temp_layout;
} SnapshotRenderContext;

static void
render_context_init(SnapshotRenderContext *ctx, EditorWidget *self, GtkSnapshot *snapshot)
{
    ctx->self = self;
    ctx->snapshot = snapshot;
    ctx->pango_ctx = gtk_widget_get_pango_context(GTK_WIDGET(self));
    
    ctx->width = gtk_widget_get_width(GTK_WIDGET(self));
    ctx->height = gtk_widget_get_height(GTK_WIDGET(self));
    
    ctx->scroll_x = self->hadjustment ? gtk_adjustment_get_value(self->hadjustment) : 0;
    ctx->scroll_y = self->vadjustment ? gtk_adjustment_get_value(self->vadjustment) : 0;
    
    ctx->minimap_w = 0;
    if (self->minimap_enabled) {
        ctx->minimap_w = self->minimap_width;
        if (ctx->minimap_w > ctx->width / 2) ctx->minimap_w = ctx->width / 2;
    }
    
    ctx->gutter_w = get_effective_gutter_width(self);
    ctx->text_start_x = ctx->gutter_w + self->padding_left;
    
    /* Theme / BG Logic */
    gtk_widget_get_color(GTK_WIDGET(self), &self->color_text);
    self->color_cursor = self->color_text;
    
    ctx->bg_color = (GdkRGBA){1, 1, 1, 1};
    ctx->is_dark = FALSE;
    if (self->color_text.red > 0.5 && self->color_text.green > 0.5 && self->color_text.blue > 0.5) {
        /* #1d1d20 = 29, 29, 32 */
        ctx->bg_color = (GdkRGBA){0.113725, 0.113725, 0.12549, 1.0};
        ctx->is_dark = TRUE;
    }
    
    /* Update Theme Colors (Auto-Theme Logic for now) */
    self->color_background = ctx->bg_color;
    /* Gutter same as background */
    self->color_gutter_bg = self->color_background;
    
    if (ctx->is_dark) {
        /* Use 'd_variable_c' (Light Grey #d1d1d1) for plain text as requested */
        self->color_text = (GdkRGBA){0.82, 0.82, 0.82, 1.0};
        
        self->color_line_highlight = self->color_text; 
        self->color_line_highlight.alpha = 0.04; /* Reduced from 0.1 */
        
        self->color_line_number = self->color_text;
        self->color_line_number.alpha = 0.5;
    } else {
        /* Keep original text color for light mode (likely black) */
        
        self->color_line_highlight = self->color_text; 
        self->color_line_highlight.alpha = 0.03; /* Reduced from 0.05 */
        
        self->color_line_number = self->color_text;
        self->color_line_number.alpha = 0.5;
    }
    
    if (self->last_theme_dark_mode != ctx->is_dark) {
        syntax_set_theme_mode(ctx->is_dark);
        if (self->syntax_ctx) syntax_context_invalidate_cache(self->syntax_ctx);
        self->last_theme_dark_mode = ctx->is_dark;
    }
}

static void
draw_editor_background(SnapshotRenderContext *ctx)
{
    gtk_snapshot_append_color(ctx->snapshot, &ctx->bg_color, 
        &GRAPHENE_RECT_INIT(0, 0, (float)ctx->width, (float)ctx->height));
    
    if (ctx->self->show_line_numbers) {
        gtk_snapshot_append_color(ctx->snapshot, &ctx->self->color_gutter_bg, 
            &GRAPHENE_RECT_INIT(0, 0, (float)ctx->gutter_w, (float)ctx->height));
    }
}

static void
render_context_calculate_visible_range(SnapshotRenderContext *ctx)
{
    size_t start_line = 0;
    double partial_y = 0;
    size_t max_lines = get_visual_line_count(ctx->self);
    
    if (ctx->self->line_y_offsets && ctx->self->line_y_offsets->len > 0) {
        double *offsets = (double*)ctx->self->line_y_offsets->data;
        size_t low = 0;
        size_t high = ctx->self->line_y_offsets->len - 1;
        
        while (low < high) {
            size_t mid = low + (high - low + 1) / 2;
            if (offsets[mid] <= ctx->scroll_y) {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        start_line = low;
        if (start_line >= max_lines && max_lines > 0) start_line = max_lines - 1;
        partial_y = ctx->scroll_y - offsets[start_line];
    } else {
        double multiplier = (ctx->self->wrap_lines) ? ctx->self->avg_visual_lines : 1.0;
        if (multiplier < 1.0) multiplier = 1.0;
        start_line = (size_t)(ctx->scroll_y / (ctx->self->line_height * multiplier));
        partial_y = fmod(ctx->scroll_y, ctx->self->line_height * multiplier);
        if (partial_y > ctx->self->line_height * 20) partial_y = 0;
    }
    
    ctx->start_line = start_line;
    ctx->partial_y = partial_y;
    ctx->count_lines = (size_t)(ctx->height / ctx->self->line_height) + 2;
    
    /* Ensure syntax state */
    size_t phys_total_lines = document_get_line_count(ctx->self->doc);
    size_t target_scan_line = ctx->start_line + ctx->count_lines;
    if (target_scan_line >= phys_total_lines) target_scan_line = phys_total_lines > 0 ? phys_total_lines - 1 : 0;
    editor_widget_ensure_syntax_state_up_to(ctx->self, target_scan_line);
}

static void
render_context_prepare_state(SnapshotRenderContext *ctx)
{
    /* Pre-compute cursor lines */
    ctx->num_cursors = ctx->self->cursors->len;
    ctx->cursor_lines = g_new(size_t, ctx->num_cursors);
    for (guint c = 0; c < ctx->num_cursors; c++) {
        EditorCursor *cur = &g_array_index(ctx->self->cursors, EditorCursor, c);
        ctx->cursor_lines[c] = document_get_line_of_offset(ctx->self->doc, cur->cursor_offset);
    }

    /* Tab array */
    ctx->tab_array = NULL;
    if (ctx->self->tab_width > 0) {
        PangoFontMetrics *metrics = pango_context_get_metrics(ctx->pango_ctx, ctx->self->font_desc, NULL);
        int char_width = pango_font_metrics_get_approximate_char_width(metrics);
        pango_font_metrics_unref(metrics);
        int tab_width_pango = char_width * ctx->self->tab_width;
        ctx->tab_array = pango_tab_array_new(1, FALSE);
        pango_tab_array_set_tab(ctx->tab_array, 0, PANGO_TAB_LEFT, tab_width_pango);
    }

    /* Pango layouts */
    ctx->line_layout = pango_layout_new(ctx->pango_ctx);
    ctx->lnum_layout = ctx->self->show_line_numbers ? pango_layout_new(ctx->pango_ctx) : NULL;
    ctx->fold_layout = ctx->self->enable_folding ? pango_layout_new(ctx->pango_ctx) : NULL;
    ctx->temp_layout = pango_layout_new(ctx->pango_ctx);
}

static void
render_context_clear(SnapshotRenderContext *ctx)
{
    g_free(ctx->cursor_lines);
    if (ctx->tab_array) pango_tab_array_free(ctx->tab_array);
    if (ctx->line_layout) g_object_unref(ctx->line_layout);
    if (ctx->lnum_layout) g_object_unref(ctx->lnum_layout);
    if (ctx->fold_layout) g_object_unref(ctx->fold_layout);
    if (ctx->temp_layout) g_object_unref(ctx->temp_layout);
}

static void
render_single_line(SnapshotRenderContext *ctx, size_t phys_line, double current_y_pos, double *advance_h)
{
    EditorWidget *self = ctx->self;
    GtkSnapshot *snapshot = ctx->snapshot;
    int width = ctx->width;
    int height = ctx->height;
    double text_start_x = ctx->text_start_x;
    double scroll_x = ctx->scroll_x;
    double scroll_y = ctx->scroll_y;
    double minimap_w = ctx->minimap_w;
    double gutter_w = ctx->gutter_w;

    size_t len;
    char *text = NULL;
    gboolean is_virtualized = FALSE;
    size_t chunk_padding = 0;
    double render_x_offset = 0;
    double render_y_offset = 0;
    double virtual_full_height = 0;

    size_t fetched_len = 0;
    size_t full_len = 0;
    char *pre_fetched_text = document_get_line_truncated(self->doc, phys_line, &fetched_len, 4096, &full_len);
    
    if (full_len <= 4096) {
        text = pre_fetched_text;
        len = fetched_len;
    } else {
        g_free(pre_fetched_text);
        is_virtualized = TRUE;
        double cw = self->cached_char_width > 1.0 ? self->cached_char_width : 8.0; 
        
        if (self->wrap_lines) {
            double line_doc_y = current_y_pos + scroll_y;
            if (self->line_y_offsets && phys_line < self->line_y_offsets->len) {
                line_doc_y = g_array_index(self->line_y_offsets, double, phys_line);
            }
            double relative_start = scroll_y - line_doc_y;
            size_t start_row = (relative_start > 0) ? (size_t)(relative_start / self->line_height) : 0;
            int available_w = width - text_start_x - self->active_right_padding; 
            if (available_w < 50) available_w = 50;
            int chars_per_line = (int)((double)available_w / cw);
            if (chars_per_line < 1) chars_per_line = 1;
            
            size_t full_rows = (full_len + chars_per_line - 1) / chars_per_line;
            virtual_full_height = (double)(full_rows > 0 ? full_rows : 1) * self->line_height;
            size_t start_char_idx = start_row * chars_per_line;
            if (start_char_idx > full_len) start_char_idx = full_len;
            
            size_t visible_rows = (size_t)(height / self->line_height) + 2; 
            size_t safe_len = visible_rows * chars_per_line + 100;
            if (start_char_idx + safe_len > full_len) safe_len = full_len - start_char_idx;
            
            text = document_get_text_range(self->doc, document_get_offset_of_line(self->doc, phys_line) + start_char_idx, safe_len);
            len = safe_len;
            chunk_padding = start_char_idx;
            render_y_offset = (double)start_row * self->line_height;
        } else {
            double start_char_visual = (scroll_x / cw) - 100.0;
            size_t start_byte_approx = (start_char_visual > 0) ? (size_t)start_char_visual : 0;
            if (start_byte_approx > full_len) start_byte_approx = full_len;
            size_t visible_chars = (size_t)(width / cw) + 300; 
            size_t safe_len = visible_chars;
            if (start_byte_approx + safe_len > full_len) safe_len = full_len - start_byte_approx;
            text = document_get_text_range(self->doc, document_get_offset_of_line(self->doc, phys_line) + start_byte_approx, safe_len);
            len = safe_len;
            chunk_padding = start_byte_approx;
            render_x_offset = start_byte_approx * cw;
        }
    }

    if (!text) { text = g_strdup(""); len = 0; }
    if (len > 0 && !g_utf8_validate(text, len, NULL)) {
         char *safe_text = g_utf8_make_valid(text, len);
         g_free(text); text = safe_text; len = strlen(text);
    }
    while (len > 0 && (text[len-1] == '\n' || text[len-1] == '\r')) { text[len-1] = '\0'; len--; }

    PangoLayout *layout = ctx->line_layout;
    pango_layout_set_attributes(layout, NULL);
    pango_layout_set_width(layout, -1);
    pango_layout_set_indent(layout, 0);
    pango_layout_set_font_description(layout, self->font_desc);
    pango_layout_set_text(layout, text, (len > MAX_PANGO_LINE_LEN) ? MAX_PANGO_LINE_LEN : (int)len);
    if (ctx->tab_array) pango_layout_set_tabs(layout, ctx->tab_array);
    
    if (self->wrap_lines) {
        int avail = width - text_start_x - self->active_right_padding - (int)minimap_w;
        pango_layout_set_width(layout, (avail < 50 ? 50 : avail) * PANGO_SCALE);
        pango_layout_set_wrap(layout, is_virtualized ? PANGO_WRAP_CHAR : PANGO_WRAP_WORD_CHAR);
    }

    PangoAttrList *cached_attrs = is_virtualized ? NULL : syntax_highlight_line(self->syntax_ctx, phys_line, text);
    PangoAttrList *attrs = cached_attrs ? pango_attr_list_copy(cached_attrs) : pango_attr_list_new();
    pango_layout_set_attributes(layout, attrs);
    if (cached_attrs) pango_attr_list_unref(cached_attrs);

    int pixel_h_int;
    pango_layout_get_pixel_size(layout, NULL, &pixel_h_int);
    double pixel_h = (double)pixel_h_int;
    double layout_h = MAX(pixel_h, self->line_height);
    *advance_h = (is_virtualized && self->wrap_lines) ? virtual_full_height : layout_h;
    double centering_offset = floor((layout_h - pixel_h) / 2.0);

    /* Line Highlights */
    gboolean is_highlighted = FALSE;
    if (self->highlight_current_line) {
         for (guint c = 0; c < ctx->num_cursors; c++) {
             EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
             if (ctx->cursor_lines[c] == phys_line && cur->cursor_offset == cur->selection_anchor) { is_highlighted = TRUE; break; }
         }
         if (is_highlighted) {
             /* Highlight full width including gutter */
             gtk_snapshot_append_color(snapshot, &self->color_line_highlight, &GRAPHENE_RECT_INIT(0, (float)(current_y_pos + self->padding_top), (float)(width), (float)layout_h));
         }
    }

    if (self->show_line_numbers) {
        char lnum[32]; snprintf(lnum, sizeof(lnum), "%zu", phys_line + 1);
        
        PangoFontDescription *desc = pango_font_description_copy(self->font_desc);
        if (is_highlighted) {
            pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
        }
        pango_layout_set_font_description(ctx->lnum_layout, desc);
        pango_font_description_free(desc);
        
        pango_layout_set_text(ctx->lnum_layout, lnum, -1);
        pango_layout_set_alignment(ctx->lnum_layout, PANGO_ALIGN_RIGHT);
        double fold_w = editor_widget_get_fold_gutter_width(self);
        double lnum_w = gutter_w - fold_w - 8.0;
        pango_layout_set_width(ctx->lnum_layout, (int)(MAX(lnum_w, 1.0) * PANGO_SCALE));
        GdkRGBA gfg = self->color_line_number; /* Use distinct color (alpha handled in init) */
        
        /* Ensure distinct color for highlighted line number if needed, currently just bold */
        if (is_highlighted) gfg.alpha = 0.9;
        
        gtk_snapshot_save(snapshot);
        gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(4, (float)(current_y_pos + self->padding_top + centering_offset)));
        gtk_snapshot_append_layout(snapshot, ctx->lnum_layout, &gfg);
        gtk_snapshot_restore(snapshot);

        if (self->enable_folding && self->fold_ranges) {
            FoldRange fr;
            if (editor_widget_get_fold_range(self, phys_line, &fr)) {
                gboolean collapsed = editor_widget_is_fold_collapsed(self, phys_line);
                if (collapsed || self->mouse_in_gutter) {
                    pango_layout_set_font_description(ctx->fold_layout, self->font_desc);
                    pango_layout_set_text(ctx->fold_layout, collapsed ? "▶" : "▼", -1);
                    pango_layout_set_alignment(ctx->fold_layout, PANGO_ALIGN_CENTER);
                    pango_layout_set_width(ctx->fold_layout, (int)((fold_w - 4.0) * PANGO_SCALE));
                    GdkRGBA ffg = self->color_text; ffg.alpha = 0.7;
                    gtk_snapshot_save(snapshot);
                    gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT((float)(gutter_w - fold_w + 2), (float)(current_y_pos + self->padding_top + centering_offset)));
                    gtk_snapshot_append_layout(snapshot, ctx->fold_layout, &ffg);
                    gtk_snapshot_restore(snapshot);
                }
            }
        }
    }

    gtk_snapshot_save(snapshot);
    gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT((float)gutter_w, 0, (float)(width - gutter_w - minimap_w), (float)height));
    gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT((float)(text_start_x - scroll_x + render_x_offset), (float)(current_y_pos + self->padding_top + render_y_offset)));
    
    size_t line_start_off = document_get_offset_of_line(self->doc, phys_line);
    size_t line_end_off = (phys_line + 1 < document_get_line_count(self->doc)) ? 
        document_get_offset_of_line(self->doc, phys_line + 1) : document_get_length(self->doc);

    /* Search Highlights */
    if (self->search_matches && self->search_matches->len > 0) {
        int low = 0, high = (int)self->search_matches->len - 1, first = -1;
        while (low <= high) {
            int mid = (low + high) / 2;
            SearchMatch *m = &g_array_index(self->search_matches, SearchMatch, mid);
            if (m->end > line_start_off + chunk_padding) { first = mid; high = mid - 1; }
            else { low = mid + 1; }
        }
        if (first >= 0) {
            for (int m = first; m < (int)self->search_matches->len; m++) {
                SearchMatch match = g_array_index(self->search_matches, SearchMatch, m);
                if (match.start >= line_start_off + chunk_padding + len) break;
                int i_start = (int)(MAX(match.start, line_start_off + chunk_padding) - (line_start_off + chunk_padding));
                int i_end = (int)(MIN(match.end, line_start_off + chunk_padding + len) - (line_start_off + chunk_padding));
                if (i_start < i_end) {
                    PangoLayoutIter *iter = pango_layout_get_iter(layout);
                    int l_idx = 0, l_cnt = pango_layout_get_line_count(layout);
                    do {
                        PangoLayoutLine *pl = pango_layout_iter_get_line_readonly(iter);
                        int ls = pl->start_index, le = ls + pl->length;
                        PangoRectangle lr; pango_layout_iter_get_line_extents(iter, NULL, &lr);
                        double ry = pango_units_to_double(lr.y) + centering_offset;
                        double rh = pango_units_to_double(lr.height);
                        if (l_idx == 0) { rh += ry; ry = 0; }
                        if (l_idx == l_cnt - 1) rh = layout_h - ry;
                        if (i_end >= ls && i_start <= le) {
                            int *rs, nrs; pango_layout_line_get_x_ranges(pl, MAX(i_start, ls), MIN(i_end, le), &rs, &nrs);
                            for (int r = 0; r < nrs; r++) {
                                double rx = pango_units_to_double(rs[2*r]), rw = pango_units_to_double(rs[2*r+1]-rs[2*r]);
                                if (rw > 0) {
                                    if (match.start == self->current_match_offset) {
                                        GdkRGBA f = {1.0, 0.8, 0.4, 0.2}, b = {1.0, 0.6, 0.0, 1.0};
                                        gtk_snapshot_append_color(snapshot, &f, &GRAPHENE_RECT_INIT((float)rx, (float)ry, (float)rw, (float)rh));
                                        gtk_snapshot_append_border(snapshot, &GSK_ROUNDED_RECT_INIT((float)rx, (float)ry, (float)rw, (float)rh), (float[4]){1,1,1,1}, (GdkRGBA[4]){b,b,b,b});
                                    } else {
                                        GdkRGBA f = ctx->is_dark ? (GdkRGBA){0.8, 0.8, 0.8, 0.15} : (GdkRGBA){0.6, 0.6, 0.6, 0.2};
                                        gtk_snapshot_append_color(snapshot, &f, &GRAPHENE_RECT_INIT((float)rx, (float)ry, (float)rw, (float)rh));
                                        GdkRGBA u = ctx->is_dark ? (GdkRGBA){0.7, 0.7, 0.7, 0.4} : (GdkRGBA){0.5, 0.5, 0.5, 0.5};
                                        gtk_snapshot_append_color(snapshot, &u, &GRAPHENE_RECT_INIT((float)rx, (float)(ry+rh-1), (float)rw, 1.0f));
                                    }
                                }
                            }
                            g_free(rs);
                        }
                        l_idx++;
                    } while (pango_layout_iter_next_line(iter));
                    pango_layout_iter_free(iter);
                }
            }
        }
    }

    /* Filter Highlights */
    if (self->filtered_lines && self->filter_pattern && *self->filter_pattern) {
        GArray *fr = NULL;
        if (self->filter_is_regex && self->filter_regex_pattern) {
            GMatchInfo *mi;
            if (g_regex_match(self->filter_regex_pattern, text, 0, &mi)) {
                while (g_match_info_matches(mi)) {
                    int s, e; g_match_info_fetch_pos(mi, 0, &s, &e);
                    if (s >= 0 && e > s) {
                        if (!fr) fr = g_array_new(FALSE, FALSE, sizeof(int)*2);
                        int r[2] = {s, e}; g_array_append_vals(fr, r, 1);
                    }
                    g_match_info_next(mi, NULL);
                }
                g_match_info_free(mi);
            }
        } else {
            const char *needle = self->filter_pattern; size_t nlen = strlen(needle);
            if (nlen > 0) {
                char *h_low = self->filter_case_sensitive ? NULL : g_utf8_strdown(text, -1);
                char *n_low = self->filter_case_sensitive ? NULL : g_utf8_strdown(needle, -1);
                const char *haystack = h_low ? h_low : text, *sn = n_low ? n_low : needle, *c = haystack, *p;
                while ((p = strstr(c, sn)) != NULL) {
                    int off = (int)(p - haystack);
                    if (!fr) fr = g_array_new(FALSE, FALSE, sizeof(int)*2);
                    int r[2] = {off, off + (int)nlen}; g_array_append_vals(fr, r, 1);
                    c = p + nlen;
                }
                g_free(h_low); g_free(n_low);
            }
        }
        if (fr) {
            PangoAttrIterator *ai = pango_attr_list_get_iterator(attrs);
            do {
                int as, ae; pango_attr_iterator_range(ai, &as, &ae);
                GdkRGBA rc = self->color_text;
                PangoAttribute *fg = pango_attr_iterator_get(ai, PANGO_ATTR_FOREGROUND);
                if (fg) {
                    PangoColor *pc = &((PangoAttrColor*)fg)->color;
                    rc.red = pc->red / 65535.0f; rc.green = pc->green / 65535.0f; rc.blue = pc->blue / 65535.0f; rc.alpha = 1.0;
                }
                for (guint i = 0; i < fr->len; i++) {
                    int *r = &g_array_index(fr, int, i*2);
                    int is = MAX(as, r[0]), ie = MIN(ae, r[1]);
                    if (is < ie) {
                        PangoLayoutIter *li = pango_layout_get_iter(layout);
                        do {
                            PangoLayoutLine *l = pango_layout_iter_get_line_readonly(li);
                            int ls = l->start_index, le = ls + l->length;
                            int lis = MAX(is, ls), lie = MIN(ie, le);
                            if (lis < lie) {
                                double uy = floor((double)pango_layout_iter_get_baseline(li)/PANGO_SCALE + centering_offset + 3.5);
                                int *rs, nrs; pango_layout_line_get_x_ranges(l, lis, lie, &rs, &nrs);
                                if (rs) {
                                    for (int j = 0; j < nrs; j++) {
                                        double x0 = floor((double)rs[2*j]/PANGO_SCALE + 0.5), x1 = floor((double)rs[2*j+1]/PANGO_SCALE + 0.5);
                                        if (x1 - x0 >= 1.0) gtk_snapshot_append_color(snapshot, &rc, &GRAPHENE_RECT_INIT((float)x0, (float)uy, (float)(x1-x0), 1.0f));
                                    }
                                    g_free(rs);
                                }
                            }
                        } while (pango_layout_iter_next_line(li));
                        pango_layout_iter_free(li);
                    }
                }
            } while (pango_attr_iterator_next(ai));
            pango_attr_iterator_destroy(ai);
            g_array_free(fr, TRUE);
        }
    }

    /* Selection */
    for (guint c = 0; c < ctx->num_cursors; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        size_t s1 = MIN(cur->cursor_offset, cur->selection_anchor), s2 = MAX(cur->cursor_offset, cur->selection_anchor);
        if (s1 < line_end_off && s2 > line_start_off && s1 != s2) {
            size_t si1 = MAX(s1, line_start_off + chunk_padding) - (line_start_off + chunk_padding);
            size_t si2 = MIN(s2, line_start_off + chunk_padding + len) - (line_start_off + chunk_padding);
            if (len == 0 && s2 > line_start_off) gtk_snapshot_append_color(snapshot, &(GdkRGBA){0.2, 0.4, 0.8, 0.35}, &GRAPHENE_RECT_INIT(0, 0, (float)width, (float)layout_h));
            else if (len > 0 && si1 < si2) {
                PangoLayoutIter *iter = pango_layout_get_iter(layout);
                int l_idx = 0, l_cnt = pango_layout_get_line_count(layout);
                do {
                    PangoLayoutLine *pl = pango_layout_iter_get_line_readonly(iter);
                    int ls = pl->start_index, le = ls + pl->length;
                    PangoRectangle lr; pango_layout_iter_get_line_extents(iter, NULL, &lr);
                    double ry = pango_units_to_double(lr.y) + centering_offset, rh = pango_units_to_double(lr.height);
                    if (l_idx == 0) { rh += ry; ry = 0; }
                    if (l_idx == l_cnt - 1) rh = layout_h - ry;
                    
                    if (si2 >= (size_t)ls && si1 <= (size_t)le) {
                        int *rs, nrs; pango_layout_line_get_x_ranges(pl, (int)MAX(si1, (size_t)ls), (int)MIN(si2, (size_t)le), &rs, &nrs);
                        for (int r = 0; r < nrs; r++) {
                            double rx = pango_units_to_double(rs[2*r]), rw = pango_units_to_double(rs[2*r+1]-rs[2*r]);
                            if (rw > 0) gtk_snapshot_append_color(snapshot, &(GdkRGBA){0.2, 0.4, 0.8, 0.35}, &GRAPHENE_RECT_INIT((float)rx, (float)ry, (float)rw, (float)rh));
                        }
                        g_free(rs);
                        if (s2 > line_start_off + chunk_padding + (size_t)le) {
                            double dx, ew, ex;
                            if (pl->resolved_dir == PANGO_DIRECTION_RTL) {
                                dx = pango_units_to_double(lr.x);
                                ew = dx;
                                ex = 0;
                            } else {
                                dx = pango_units_to_double(lr.x + lr.width);
                                ew = (width + scroll_x) - dx;
                                ex = dx;
                            }
                            if (ew > 0) gtk_snapshot_append_color(snapshot, &(GdkRGBA){0.2, 0.4, 0.8, 0.35}, &GRAPHENE_RECT_INIT((float)ex, (float)ry, (float)ew, (float)rh));
                        }
                    }
                    l_idx++;
                } while (pango_layout_iter_next_line(iter));
                pango_layout_iter_free(iter);
            }
        }
    }

    /* Text */
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(0, (float)centering_offset));
    gtk_snapshot_append_layout(snapshot, layout, &self->color_text);
    gtk_snapshot_restore(snapshot);

    /* Cursors */
    float cursor_w = 0.4f; int scale = gtk_widget_get_scale_factor(GTK_WIDGET(self));
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        if (cur->cursor_offset >= line_start_off + chunk_padding && cur->cursor_offset <= (line_start_off + chunk_padding + len)) {
            if (gtk_widget_has_focus(GTK_WIDGET(self)) && self->cursor_alpha > 0.01 && cur->cursor_offset == cur->selection_anchor && !self->is_dragging_selection) {
                int idx = (int)MIN(cur->cursor_offset - (line_start_off + chunk_padding), len);
                PangoRectangle sp; pango_layout_get_cursor_pos(layout, idx, &sp, NULL);
                GdkRGBA cc = self->color_cursor; cc.alpha = self->cursor_alpha;
                double xp = pango_units_to_double(sp.x);
                float cx = (float)floor(xp * scale + 0.5) / scale;
                double ph = pango_units_to_double(sp.height), py = pango_units_to_double(sp.y) + centering_offset;
                double ch = MAX(ph, self->line_height), cy = py - (ch - ph) / 2.0;
                if (self->insert_mode) {
                    for (int p = 0; p < 4; p++) gtk_snapshot_append_color(snapshot, &cc, &GRAPHENE_RECT_INIT(cx, (float)((int)(cy + 0.5)), cursor_w, (float)((int)ch)));
                } else {
                    double bw = 0; int cl = 0; const char *ct = NULL;
                    if ((size_t)idx < len) {
                        const char *s = text + idx; const char *e = g_utf8_next_char(s);
                        cl = (int)(e - s); ct = s; bw = pango_units_to_double(sp.width);
                    }
                    if (bw <= 0.1) {
                        PangoFontMetrics *m = pango_context_get_metrics(ctx->pango_ctx, self->font_desc, NULL);
                        bw = pango_units_to_double(pango_font_metrics_get_approximate_char_width(m));
                        pango_font_metrics_unref(m);
                    }
                    gtk_snapshot_append_color(snapshot, &cc, &GRAPHENE_RECT_INIT(cx, (float)((int)(cy + 0.5)), (float)bw, (float)((int)ch)));
                    if (cl > 0 && ct) {
                        pango_layout_set_attributes(ctx->temp_layout, NULL); pango_layout_set_width(ctx->temp_layout, -1);
                        pango_layout_set_font_description(ctx->temp_layout, self->font_desc);
                        pango_layout_set_text(ctx->temp_layout, ct, cl);
                        GdkRGBA itc = ctx->bg_color; itc.alpha = cc.alpha;
                        gtk_snapshot_save(snapshot);
                        gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(cx, (float)py));
                        gtk_snapshot_append_layout(snapshot, ctx->temp_layout, &itc);
                        gtk_snapshot_restore(snapshot);
                    }
                }
            }
        }
    }

    /* DnD drop caret */
    if (self->is_dragging_selection && self->drag_drop_offset != (size_t)-1) {
        size_t dl = document_get_line_of_offset(self->doc, self->drag_drop_offset);
        if (phys_line == dl) {
            int di = (int)MIN(self->drag_drop_offset - (line_start_off + chunk_padding), len);
            PangoRectangle sp; pango_layout_get_cursor_pos(layout, di, &sp, NULL);
            GdkRGBA dc = self->drag_copy_mode ? (GdkRGBA){0.18, 0.76, 0.49, 1.0} : (GdkRGBA){1.0, 0.647, 0.0, 1.0};
            
            double ch = pango_units_to_double(sp.height);
            if (ch < self->line_height) ch = self->line_height;
            
            gtk_snapshot_append_color(snapshot, &dc, &GRAPHENE_RECT_INIT((float)pango_units_to_double(sp.x), (float)(pango_units_to_double(sp.y) + centering_offset), 2.0f, (float)ch));
        }
    }

    gtk_snapshot_pop(snapshot);
    gtk_snapshot_restore(snapshot);
    pango_attr_list_unref(attrs);
    g_free(text);
}

static void
draw_overlays(SnapshotRenderContext *ctx)
{
    EditorWidget *self = ctx->self;
    if (self->is_dnd_active) {
        if (self->drag_ghost_layout) {
            gtk_snapshot_save(ctx->snapshot);
            gtk_snapshot_translate(ctx->snapshot, &GRAPHENE_POINT_INIT((float)self->drag_x, (float)self->drag_y));
            GdkRGBA gc = self->color_text; gc.alpha = 0.5;
            gtk_snapshot_append_layout(ctx->snapshot, self->drag_ghost_layout, &gc);
            gtk_snapshot_restore(ctx->snapshot);
        }
        GdkRGBA bc = self->drag_copy_mode ? (GdkRGBA){0.18, 0.76, 0.49, 1.0} : (GdkRGBA){1.0, 0.647, 0.0, 1.0};
        gtk_snapshot_append_border(ctx->snapshot, &GSK_ROUNDED_RECT_INIT(0, 0, (float)ctx->width, (float)ctx->height), (float[4]){1,1,1,1}, (GdkRGBA[4]){bc,bc,bc,bc});
    }

    if (self->show_right_margin) {
        PangoFontMetrics *m = pango_context_get_metrics(ctx->pango_ctx, self->font_desc, NULL);
        int cw = pango_font_metrics_get_approximate_char_width(m);
        pango_font_metrics_unref(m);
        double mx = ctx->text_start_x + (self->right_margin_position * pango_units_to_double(cw));
        GdkRGBA mc = self->color_text; mc.alpha = 0.03;
        float mw = (float)ctx->width - (float)mx;
        if (mw > 0) gtk_snapshot_append_color(ctx->snapshot, &mc, &GRAPHENE_RECT_INIT((float)mx, 0, mw, (float)ctx->height));
        GdkRGBA lc = self->color_text; lc.alpha = 0.06;
        gtk_snapshot_append_color(ctx->snapshot, &lc, &GRAPHENE_RECT_INIT((float)mx, 0, 1.0f, (float)ctx->height));
    }

    if (self->minimap_enabled) {
        editor_minimap_draw(self, ctx->snapshot, ctx->width - ctx->minimap_w, 0, ctx->minimap_w, ctx->height);
    }
}

void
editor_widget_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    EditorWidget *self = EDITOR_WIDGET(widget);
    if (!self->doc || self->line_height <= 0) return;
    editor_widget_ensure_metrics(self);
    
    SnapshotRenderContext ctx;
    render_context_init(&ctx, self, snapshot);
    gtk_snapshot_push_clip(snapshot, &GRAPHENE_RECT_INIT(0, 0, (float)ctx.width, (float)ctx.height));
    
    draw_editor_background(&ctx);
    render_context_calculate_visible_range(&ctx);
    render_context_prepare_state(&ctx);

    double current_y_pos = -ctx.partial_y;
    size_t max_lines = get_visual_line_count(self);

    for (size_t i = 0; i < ctx.count_lines; ++i) {
        size_t visual_line_idx = ctx.start_line + i;
        if (visual_line_idx >= max_lines) break;

        size_t phys_line = get_physical_line_index(self, visual_line_idx);
        if (phys_line == (size_t)-1) continue;

        double advance_h = 0;
        render_single_line(&ctx, phys_line, current_y_pos, &advance_h);
        current_y_pos += advance_h;
        if (current_y_pos > ctx.height) break;
    }

    draw_overlays(&ctx);
    gtk_snapshot_pop(snapshot);
    render_context_clear(&ctx);
}
