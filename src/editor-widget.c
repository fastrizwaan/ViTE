#include "editor-widget.h"
#include "syntax.h"
#include <math.h>

struct _EditorWidget {
    GtkWidget parent_instance;

    Document *doc;
    
    /* GtkScrollable implementation */
    GtkAdjustment *hadjustment;
    GtkAdjustment *vadjustment;
    GtkScrollablePolicy hscroll_policy;
    GtkScrollablePolicy vscroll_policy;

    /* Theme colors */
    GdkRGBA color_text;
    GdkRGBA color_bg;
    GdkRGBA color_cursor;

    double line_height;
    PangoFontDescription *font_desc;
    
    /* State */
    size_t cursor_offset;
    size_t selection_anchor; /* If different from cursor_offset, we have selection */
    
    /* Cursor blink animation */
    guint cursor_blink_tick_id;
    gint64 cursor_blink_start_time;
    double cursor_alpha;  /* Current cursor opacity 0.0-1.0 */
    
    /* Drag and drop */
    gboolean is_dragging_selection;
    size_t drag_start_offset;
    gboolean multi_click_selection; /* Set after double/triple-click to prevent drag_begin clearing selection */
    int multi_click_mode;  /* 2 = word mode (double-click), 3 = line mode (triple-click) */
    size_t multi_click_start;  /* Original start of multi-click selection */
    size_t multi_click_end;    /* Original end of multi-click selection */
    
    /* Input */
    GtkIMContext *im_context;
    
    /* Syntax */
    SyntaxContext *syntax_ctx;

    /* Visual navigation */
    double target_x;  /* Target X position for Up/Down navigation (in pixels) */
};

static void editor_widget_scrollable_init (GtkScrollableInterface *iface);
static void editor_widget_ensure_metrics(EditorWidget *self);
static void editor_widget_reset_cursor_blink(EditorWidget *self);
static void update_target_x(EditorWidget *self);
static void move_cursor(EditorWidget *self, int visual_lines_delta);

G_DEFINE_TYPE_WITH_CODE (EditorWidget, editor_widget, GTK_TYPE_WIDGET,
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, editor_widget_scrollable_init))

enum {
    PROP_0,
    PROP_HADJUSTMENT,
    PROP_VADJUSTMENT,
    PROP_HSCROLL_POLICY,
    PROP_VSCROLL_POLICY,
    N_PROPS
};

static void
editor_widget_update_adjustments(EditorWidget *self)
{
    if (!self->vadjustment || !self->doc) return;

    editor_widget_ensure_metrics(self);

    size_t total_lines = document_get_line_count(self->doc);
    double page_size = gtk_widget_get_height(GTK_WIDGET(self)) / self->line_height;
    
    double upper = (double)total_lines;
    if (upper < page_size) upper = page_size;

    gtk_adjustment_configure(self->vadjustment,
                             gtk_adjustment_get_value(self->vadjustment),
                             0,
                             upper,
                             1,
                             page_size,
                             page_size);
}

/* UTF-8 grapheme cluster navigation helpers */

/* Move cursor right by one grapheme cluster */
static size_t
utf8_next_grapheme(Document *doc, size_t offset)
{
    size_t total = document_get_length(doc);
    if (offset >= total) return offset;
    
    /* Get a small chunk of text to analyze (UTF-8 max is 4 bytes, but grapheme clusters 
       like emoji sequences can be longer - use 16 bytes for safety) */
    size_t chunk_len = 16;
    if (offset + chunk_len > total) chunk_len = total - offset;
    
    char *text = document_get_text_range(doc, offset, chunk_len);
    if (!text) return offset + 1;
    
    if (!g_utf8_validate(text, chunk_len, NULL)) {
        g_free(text);
        return offset + 1; /* Fallback to byte movement for invalid UTF-8 */
    }
    
    /* Get next character boundary */
    char *next = g_utf8_next_char(text);
    size_t bytes = next - text;
    g_free(text);
    
    return offset + bytes;
}

/* Move cursor left by one grapheme cluster */
static size_t
utf8_prev_grapheme(Document *doc, size_t offset)
{
    if (offset == 0) return 0;
    
    /* Get text before cursor to find previous char start 
       (16 bytes should be enough for any grapheme cluster) */
    size_t start = (offset > 16) ? offset - 16 : 0;
    size_t len = offset - start;
    char *text = document_get_text_range(doc, start, len);
    
    if (!text) return (offset > 0) ? offset - 1 : 0;
    
    if (!g_utf8_validate(text, len, NULL)) {
        g_free(text);
        return (offset > 0) ? offset - 1 : 0; /* Fallback */
    }
    
    /* Find the last character start */
    char *prev = g_utf8_find_prev_char(text, text + len);
    if (!prev) {
        g_free(text);
        return (offset > 0) ? offset - 1 : 0;
    }
    
    size_t bytes = (text + len) - prev;
    g_free(text);
    
    return offset - bytes;
}

/* Helper: Check if character at offset is a word character */
static gboolean
is_word_char_at(Document *doc, size_t offset)
{
    size_t total = document_get_length(doc);
    if (offset >= total) return FALSE;
    
    char *text = document_get_text_range(doc, offset, 4); /* Max UTF-8 char */
    if (!text) return FALSE;
    
    gunichar ch = g_utf8_get_char_validated(text, -1);
    g_free(text);
    
    if (ch == (gunichar)-1 || ch == (gunichar)-2) return FALSE;
    return g_unichar_isalnum(ch) || ch == '_';
}

/* Find word boundaries around offset */
static void
find_word_at_offset(Document *doc, size_t offset, size_t *word_start, size_t *word_end)
{
    size_t total = document_get_length(doc);
    
    /* Find start of word - go back while we're on word chars */
    size_t start = offset;
    while (start > 0) {
        size_t prev = utf8_prev_grapheme(doc, start);
        if (!is_word_char_at(doc, prev)) break;
        start = prev;
    }
    
    /* Find end of word - go forward while we're on word chars */
    size_t end = offset;
    while (end < total && is_word_char_at(doc, end)) {
        end = utf8_next_grapheme(doc, end);
    }
    
    /* If we started on non-word char, select just that char */
    if (start == end && offset < total) {
        end = utf8_next_grapheme(doc, offset);
        start = offset;
    }
    
    *word_start = start;
    *word_end = end;
}

/* Find line boundaries (start and end of line, excluding trailing newline) */
static void
find_line_at_offset(Document *doc, size_t offset, size_t *line_start, size_t *line_end)
{
    size_t line_idx = document_get_line_of_offset(doc, offset);
    *line_start = document_get_offset_of_line(doc, line_idx);
    
    /* Get line length */
    size_t len;
    char *text = document_get_line(doc, line_idx, &len);
    
    /* Exclude trailing newline from selection */
    if (len > 0 && text[len - 1] == '\n') {
        len--;
    }
    
    g_free(text);
    
    *line_end = *line_start + len;
}

/* Move to start of next word */
static size_t
word_next(Document *doc, size_t offset)
{
    size_t total = document_get_length(doc);
    
    /* Skip current word characters */
    while (offset < total && is_word_char_at(doc, offset)) {
        offset = utf8_next_grapheme(doc, offset);
    }
    /* Skip whitespace/non-word to reach next word */
    while (offset < total && !is_word_char_at(doc, offset)) {
        offset = utf8_next_grapheme(doc, offset);
    }
    return offset;
}

/* Move to start of current/previous word */
static size_t
word_prev(Document *doc, size_t offset)
{
    if (offset == 0) return 0;
    
    /* Move back one char first */
    offset = utf8_prev_grapheme(doc, offset);
    
    /* Skip non-word characters backwards */
    while (offset > 0 && !is_word_char_at(doc, offset)) {
        offset = utf8_prev_grapheme(doc, offset);
    }
    /* Find start of current word */
    while (offset > 0) {
        size_t prev = utf8_prev_grapheme(doc, offset);
        if (!is_word_char_at(doc, prev)) break;
        offset = prev;
    }
    return offset;
}
static void
editor_widget_ensure_metrics(EditorWidget *self)
{
    if (self->line_height > 0) return;

    PangoContext *context = gtk_widget_get_pango_context(GTK_WIDGET(self));
    PangoLayout *layout = pango_layout_new(context);
    pango_layout_set_font_description(layout, self->font_desc);
    pango_layout_set_text(layout, "Wg", -1);
    
    int h;
    pango_layout_get_pixel_size(layout, NULL, &h);
    self->line_height = (double)h;
    
    g_object_unref(layout);
}

static void
editor_widget_snapshot(GtkWidget *widget, GtkSnapshot *snapshot)
{
    EditorWidget *self = EDITOR_WIDGET(widget);
    if (!self->doc) return;

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
    
    double start_y = 0;
    if (self->vadjustment)
        start_y = gtk_adjustment_get_value(self->vadjustment);

    size_t start_line = (size_t)start_y;
    size_t count_lines = (size_t)(height / self->line_height) + 2;
    size_t max_lines = document_get_line_count(self->doc);

    PangoContext *context = gtk_widget_get_pango_context(widget);
    
    size_t cursor_line = document_get_line_of_offset(self->doc, self->cursor_offset);
    size_t anchor_line = document_get_line_of_offset(self->doc, self->selection_anchor);

    double current_y_pos = 0;

    for (size_t i = 0; i < count_lines; ++i) {
        size_t line_idx = start_line + i;
        if (line_idx >= max_lines) break;

        size_t len;
        char *text = document_get_line(self->doc, line_idx, &len);

        /* UTF-8 Validation */
        if (!g_utf8_validate(text, len, NULL)) {
             char *safe_text = g_utf8_make_valid(text, len);
             g_free(text);
             text = safe_text;
             len = strlen(text);
        }
        
        /* Strip trailing newline for Pango render */
        if (len > 0 && text[len-1] == '\n') {
            len--;
            /* Note: We don't change 'text' buf just use len for pango, 
               but we should ensure 0-termination if pango expects string? 
               pango_layout_set_text takes length. 
            */
        }

        PangoLayout *layout = pango_layout_new(context);
        pango_layout_set_font_description(layout, self->font_desc);
        pango_layout_set_text(layout, text, (int)len);
        
        /* Word Wrap */
        pango_layout_set_width(layout, width * PANGO_SCALE);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        
        /* Syntax highlight */
        PangoAttrList *attrs = syntax_highlight_line(self->syntax_ctx, line_idx, text);
        pango_layout_set_attributes(layout, attrs);
        pango_attr_list_unref(attrs);
        
        gtk_snapshot_save(snapshot);
        gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(0, current_y_pos));
        
        /* Calculate height of this layout */
        int pixel_h;
        pango_layout_get_pixel_size(layout, NULL, &pixel_h);
        double layout_h = (double)pixel_h;
        if (layout_h < self->line_height) layout_h = self->line_height; /* Min height */
        
        /* Draw Line Background if selected */
        /* Selection rendering across lines is complex. 
           Simplified: If line is fully selected or partially.
        */
        
        /* Draw Selection */
        size_t start_sel = MIN(self->cursor_offset, self->selection_anchor);
        size_t end_sel = MAX(self->cursor_offset, self->selection_anchor);
        size_t line_start_off = document_get_offset_of_line(self->doc, line_idx);
        size_t raw_line_end = line_start_off + len + 1; /* Approximate end including newline if any */
        
        if (start_sel < raw_line_end && end_sel > line_start_off && start_sel != end_sel) {
            size_t sel_in_line_start = MAX(start_sel, line_start_off) - line_start_off;
            size_t sel_in_line_end = MIN(end_sel, (line_start_off + len)) - line_start_off;
            
            /* Handle empty/newline-only lines */
            if (len == 0) {
                gtk_snapshot_append_color(snapshot, 
                                          &(GdkRGBA){0.2, 0.4, 0.8, 0.35},
                                          &GRAPHENE_RECT_INIT(0, 0, (float)width, (float)layout_h));
            } else {
                /* Iterate over visual lines in the layout */
                PangoLayoutIter *iter = pango_layout_get_iter(layout);
                do {
                    PangoLayoutLine *p_line = pango_layout_iter_get_line_readonly(iter);
                    int line_start_index = p_line->start_index;
                    int line_end_index = line_start_index + p_line->length;
                    
                    PangoRectangle line_rect;
                    pango_layout_iter_get_line_extents(iter, NULL, &line_rect);
                    
                    double ry = pango_units_to_double(line_rect.y);
                    double rh;
                    
                    /* Peek next line to calculate height for vertical continuity */
                    PangoLayoutIter *next_iter = pango_layout_iter_copy(iter);
                    if (pango_layout_iter_next_line(next_iter)) {
                        PangoRectangle next_rect;
                        pango_layout_iter_get_line_extents(next_iter, NULL, &next_rect);
                        rh = pango_units_to_double(next_rect.y) - ry;
                    } else {
                        rh = layout_h - ry;
                    }
                    pango_layout_iter_free(next_iter);
                    
                    if (sel_in_line_end >= (size_t)line_start_index && sel_in_line_start <= (size_t)line_end_index) {
                        int x1, x2;
                        pango_layout_line_index_to_x(p_line, (int)MAX(sel_in_line_start, (size_t)line_start_index), FALSE, &x1);
                        pango_layout_line_index_to_x(p_line, (int)MIN(sel_in_line_end, (size_t)line_end_index), FALSE, &x2);
                        
                        double rx = pango_units_to_double(MIN(x1, x2));
                        double rw = pango_units_to_double(abs(x2 - x1));
                        
                        /* If selection extends beyond this visual line (wrap or logical end) */
                        if (end_sel > line_start_off + line_end_index) {
                            rw = width - rx;
                        }
                        
                        if (rw > 0 || (sel_in_line_start == sel_in_line_end && end_sel > line_start_off + len)) {
                            /* Even if rw is 0, if this is the last char and we select the newline, show something? 
                               Actually rw > 0 is better, but for end-of-line we force rw to extend. */
                            if (rw <= 0 && end_sel > line_start_off + line_end_index) rw = width - rx;
                            
                            if (rw > 0) {
                                gtk_snapshot_append_color(snapshot, 
                                                          &(GdkRGBA){0.2, 0.4, 0.8, 0.35},
                                                          &GRAPHENE_RECT_INIT((float)rx, (float)ry, (float)rw, (float)rh));
                            }
                        }
                    }
                } while (pango_layout_iter_next_line(iter));
                pango_layout_iter_free(iter);
            }
        }

        gtk_snapshot_append_layout(snapshot, layout, &self->color_text);
        
        /* Draw cursor - only when no selection */
        gboolean has_selection = (self->cursor_offset != self->selection_anchor);
        if (line_idx == cursor_line && self->cursor_alpha > 0.01 && !has_selection) {
             size_t line_start_off = document_get_offset_of_line(self->doc, line_idx);
             if (self->cursor_offset >= line_start_off) {
                 size_t index_in_line = self->cursor_offset - line_start_off;
                 if (index_in_line > len) index_in_line = len;
                 
                 PangoRectangle strong_pos;
                 pango_layout_get_cursor_pos(layout, (int)index_in_line, &strong_pos, NULL);
                 
                 /* Apply cursor alpha for blink animation */
                 GdkRGBA cursor_color = self->color_cursor;
                 cursor_color.alpha = self->cursor_alpha;
                 
                 /* Snap to integer pixels for crisp cursor */
                 int cursor_x = (int)(pango_units_to_double(strong_pos.x) + 0.5);
                 int cursor_y = (int)(pango_units_to_double(strong_pos.y) + 0.5);
                 int cursor_h = (int)(pango_units_to_double(strong_pos.height) + 0.5);
                 
                 gtk_snapshot_append_color(snapshot, 
                                           &cursor_color,
                                           &GRAPHENE_RECT_INIT(cursor_x, cursor_y, 1, cursor_h));
             }
        }
        
        /* Update Y position for next line */
        current_y_pos += layout_h;
        
        gtk_snapshot_restore(snapshot);
        g_object_unref(layout);
        g_free(text);

        if (current_y_pos > height) {
             break;
        }
    }
}


static void
editor_widget_measure (GtkWidget      *widget,
                       GtkOrientation  orientation,
                       int             for_size,
                       int            *minimum,
                       int            *natural,
                       int            *minimum_baseline,
                       int            *natural_baseline)
{
    if (minimum) *minimum = 100;
    if (natural) *natural = 800;
}

static void
editor_widget_size_allocate(GtkWidget *widget, int width, int height, int baseline)
{
    EditorWidget *self = EDITOR_WIDGET(widget);
    editor_widget_update_adjustments(self);
}

static void scroll_to_cursor(EditorWidget *self);

static void
editor_widget_get_offset_at_point(EditorWidget *self, double x, double y, size_t *out_offset)
{
    if (!self->doc) return;
    
    double start_y = (self->vadjustment) ? gtk_adjustment_get_value(self->vadjustment) : 0;
    size_t line_idx = (size_t)start_y;
    size_t count = document_get_line_count(self->doc);
    
    PangoContext *context = gtk_widget_get_pango_context(GTK_WIDGET(self));
    int width = gtk_widget_get_width(GTK_WIDGET(self));
    
    double current_y = 0;
    
    /* Iterate from top of viewport until we find the clicked line */
    while (line_idx < count) {
        size_t len;
        char *text = document_get_line(self->doc, line_idx, &len);
        
        /* Validation needed as in render loop ? */
        if (!g_utf8_validate(text, len, NULL)) {
             char *safe_text = g_utf8_make_valid(text, len);
             g_free(text);
             text = safe_text;
             len = strlen(text);
        }

        /* Strip trailing newline for hit test matching render */
        if (len > 0 && text[len-1] == '\n') {
            len--;
        }

        PangoLayout *layout = pango_layout_new(context);
        pango_layout_set_font_description(layout, self->font_desc);
        pango_layout_set_text(layout, text, len);
        pango_layout_set_width(layout, width * PANGO_SCALE);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        
        int pixel_h;
        pango_layout_get_pixel_size(layout, NULL, &pixel_h);
        double layout_h = (double)pixel_h;
        if (layout_h < self->line_height) layout_h = self->line_height;
        
        if (y < current_y + layout_h) {
            /* Found the line! */
            int index, trailing;
            pango_layout_xy_to_index(layout, 
                                     (int)(x * PANGO_SCALE), 
                                     (int)((y - current_y) * PANGO_SCALE), 
                                     &index, &trailing);
            
            /* Add trailing to handle clicks at end of characters/line */
            size_t line_start_off = document_get_offset_of_line(self->doc, line_idx);
            *out_offset = line_start_off + index + trailing;
            
            g_object_unref(layout);
            g_free(text);
            return;
        }
        
        current_y += layout_h;
        line_idx++;
        g_object_unref(layout);
        g_free(text);
        
        /* Optimization: if we went way past Y (should be caught by if), or screen height? */
        /* If user clicked way below last line? */
        if (current_y > y + 500) break; /* Safety break? */
    }
    
    /* Fallback: end of file or end of specific line loop */
    size_t max = document_get_line_count(self->doc);
    if (line_idx >= max) line_idx = max - 1;
    *out_offset = document_get_offset_of_line(self->doc, line_idx); 
    size_t last_len;
    char *ltext = document_get_line(self->doc, line_idx, &last_len);
    g_free(ltext);
    /* Maybe end of line? */
    *out_offset += last_len;
}

static void
on_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    gtk_widget_grab_focus(GTK_WIDGET(self));
    
    if (!self->doc) return;
    
    size_t off;
    editor_widget_get_offset_at_point(self, x, y, &off);
    
    editor_widget_reset_cursor_blink(self);
    
    if (n_press == 2) {
        /* Double click - select word, with special newline extension */
        size_t total = document_get_length(self->doc);
        gboolean is_newline = FALSE;
        if (off < total) {
            char *ctext = document_get_text_range(self->doc, off, 1);
            if (ctext && ctext[0] == '\n') is_newline = TRUE;
            g_free(ctext);
        }
        
        if (is_newline) {
            size_t sel_start = off;
            size_t sel_end = off + 1;
            if (off + 1 < total) {
                size_t next_line_start, next_line_end;
                find_line_at_offset(self->doc, off + 1, &next_line_start, &next_line_end);
                sel_end = next_line_end;
            }
            self->selection_anchor = sel_start;
            self->cursor_offset = sel_end;
        } else {
            size_t word_start, word_end;
            find_word_at_offset(self->doc, off, &word_start, &word_end);
            self->selection_anchor = word_start;
            self->cursor_offset = word_end;
        }
        
        self->multi_click_selection = TRUE;
        self->multi_click_mode = 2;
        self->multi_click_start = self->selection_anchor;
        self->multi_click_end = self->cursor_offset;
    } else if (n_press == 3) {
        /* Triple click - select entire line */
        size_t line_start, line_end;
        find_line_at_offset(self->doc, off, &line_start, &line_end);
        self->selection_anchor = line_start;
        self->cursor_offset = line_end;
        self->multi_click_selection = TRUE;
        self->multi_click_mode = 3;
        self->multi_click_start = line_start;  /* Store for drag extension */
        self->multi_click_end = line_end;
    } else {
        /* Single click */
        self->multi_click_selection = FALSE;
        GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
        
        /* Check if clicking inside existing selection */
        size_t sel_start = MIN(self->cursor_offset, self->selection_anchor);
        size_t sel_end = MAX(self->cursor_offset, self->selection_anchor);
        gboolean has_selection = (sel_start != sel_end);
        gboolean click_in_selection = has_selection && (off >= sel_start && off < sel_end);
        
        if (click_in_selection && !(state & GDK_SHIFT_MASK)) {
            /* Start potential drag of selection */
            self->is_dragging_selection = TRUE;
            self->drag_start_offset = off;
            /* Don't change selection yet - wait for drag or click release */
        } else if (state & GDK_SHIFT_MASK) {
            /* Extend selection - keep anchor, move cursor */
            self->cursor_offset = off;
        } else {
            /* Reset selection */
            self->cursor_offset = off;
            self->selection_anchor = off;
        }
    }
    
    update_target_x(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->doc) return;
    
    /* If we just did a multi-click selection (double/triple-click), 
       don't process drag_begin as it would interfere with the selection */
    if (self->multi_click_selection) {
        self->is_dragging_selection = TRUE;  /* Treat as selecting within multi-click */
        return;
    }
    
    /* Check if we're starting a drag on a selection */
    size_t off;
    editor_widget_get_offset_at_point(self, x, y, &off);
    
    size_t sel_start = MIN(self->cursor_offset, self->selection_anchor);
    size_t sel_end = MAX(self->cursor_offset, self->selection_anchor);
    
    if (sel_start != sel_end && off >= sel_start && off < sel_end) {
        /* Starting drag on existing selection - prepare for DnD */
        self->is_dragging_selection = TRUE;
        self->drag_start_offset = off;
    } else {
        /* Normal click/drag - selection handled by on_click_pressed and on_drag_update */
        self->is_dragging_selection = FALSE;
    }
}

static void
on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->doc) return;
    
    double start_x, start_y;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
    
    size_t off;
    editor_widget_get_offset_at_point(self, start_x + offset_x, start_y + offset_y, &off);
    
    if (self->multi_click_selection) {
        /* Multi-click drag - extend selection while keeping original word/line as minimum */
        size_t current_start = off;
        size_t current_end = off;
        
        if (self->multi_click_mode == 2) {
            /* Word mode */
            find_word_at_offset(self->doc, off, &current_start, &current_end);
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
            self->cursor_offset = current_end;
        } else {
            /* Still within original selection - keep original bounds */
            self->selection_anchor = self->multi_click_start;
            self->cursor_offset = self->multi_click_end;
        }
    } else if (self->is_dragging_selection) {
        /* Dragging selection for DnD - visual feedback handled by cursor */
        /* We'll execute the move on drag_end */
    } else {
        /* Normal drag selection - extend selection to current position */
        /* Only update cursor, anchor was set by on_click_pressed */
        self->cursor_offset = off;
    }
    
    scroll_to_cursor(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->doc) return;
    
    /* If this was a multi-click selection (double/triple-click), 
       just clear the flag and preserve the selection */
    if (self->multi_click_selection) {
        self->multi_click_selection = FALSE;
        self->is_dragging_selection = FALSE;
        return;
    }
    
    if (self->is_dragging_selection) {
        double start_x, start_y;
        gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
        
        size_t drop_off;
        editor_widget_get_offset_at_point(self, start_x + offset_x, start_y + offset_y, &drop_off);
        
        size_t sel_start = MIN(self->cursor_offset, self->selection_anchor);
        size_t sel_end = MAX(self->cursor_offset, self->selection_anchor);
        
        /* Check if there was actual movement (not just a click) - 8px threshold */
        gboolean has_movement = (fabs(offset_x) > 8 || fabs(offset_y) > 8);
        
        if (has_movement && (drop_off < sel_start || drop_off >= sel_end)) {
            /* Move selection to new location */
            size_t sel_len = sel_end - sel_start;
            char *text = document_get_text_range(self->doc, sel_start, sel_len);
            
            if (text) {
                document_delete(self->doc, sel_start, sel_len);
                
                if (drop_off > sel_end) {
                    drop_off -= sel_len;
                }
                
                document_insert(self->doc, drop_off, text, sel_len);
                
                self->selection_anchor = drop_off;
                self->cursor_offset = drop_off + sel_len;
                
                g_free(text);
                editor_widget_update_adjustments(self);
            }
        } else if (!has_movement) {
            /* Just a click inside selection - place cursor there */
            self->cursor_offset = drop_off;
            self->selection_anchor = drop_off;
        }
        /* If moved but still inside selection, do nothing */
        
        self->is_dragging_selection = FALSE;
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

static gboolean
editor_widget_delete_selection(EditorWidget *self)
{
    if (self->cursor_offset == self->selection_anchor) return FALSE;
    
    size_t start = MIN(self->cursor_offset, self->selection_anchor);
    size_t end = MAX(self->cursor_offset, self->selection_anchor);
    size_t len = end - start;
    
    document_delete(self->doc, start, len);
    self->cursor_offset = start;
    self->selection_anchor = start;
    
    return TRUE;
}

static void
scroll_to_cursor(EditorWidget *self)
{
    if (!self->vadjustment || !self->doc) return;
    
    size_t line = document_get_line_of_offset(self->doc, self->cursor_offset);
    double val = gtk_adjustment_get_value(self->vadjustment);
    double page = gtk_adjustment_get_page_size(self->vadjustment);
    
    if (line < val) {
        gtk_adjustment_set_value(self->vadjustment, (double)line);
    } else if (line >= val + page - 1) {
        gtk_adjustment_set_value(self->vadjustment, (double)line - page + 1);
    }
}

static PangoLayout *
create_pango_layout_for_line(EditorWidget *self, size_t line_idx, char **out_text, size_t *out_len)
{
    size_t len;
    char *text = document_get_line(self->doc, line_idx, &len);
    if (!text) return NULL;

    if (!g_utf8_validate(text, len, NULL)) {
        char *safe = g_utf8_make_valid(text, len);
        g_free(text);
        text = safe;
        len = strlen(text);
    }

    if (len > 0 && text[len-1] == '\n') {
        len--;
    }

    PangoContext *context = gtk_widget_get_pango_context(GTK_WIDGET(self));
    PangoLayout *layout = pango_layout_new(context);
    pango_layout_set_font_description(layout, self->font_desc);
    pango_layout_set_text(layout, text, (int)len);
    
    int width = gtk_widget_get_width(GTK_WIDGET(self));
    pango_layout_set_width(layout, width * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);

    if (out_text) *out_text = text;
    else g_free(text);
    if (out_len) *out_len = len;

    return layout;
}

static void
update_target_x(EditorWidget *self)
{
    size_t line_idx = document_get_line_of_offset(self->doc, self->cursor_offset);
    size_t line_start = document_get_offset_of_line(self->doc, line_idx);
    size_t index_in_line = self->cursor_offset - line_start;
    
    char *text;
    size_t len;
    PangoLayout *layout = create_pango_layout_for_line(self, line_idx, &text, &len);
    if (!layout) return;
    
    if (index_in_line > len) index_in_line = len;
    
    PangoRectangle strong_pos;
    pango_layout_get_cursor_pos(layout, (int)index_in_line, &strong_pos, NULL);
    self->target_x = pango_units_to_double(strong_pos.x);
    
    g_object_unref(layout);
    g_free(text);
}

static void
move_cursor(EditorWidget *self, int visual_lines_delta)
{
    size_t line_idx = document_get_line_of_offset(self->doc, self->cursor_offset);
    size_t line_start = document_get_offset_of_line(self->doc, line_idx);
    size_t char_idx = self->cursor_offset - line_start;

    char *text = NULL;
    size_t len;
    PangoLayout *layout = create_pango_layout_for_line(self, line_idx, &text, &len);
    if (!layout) return;

    /* If target_x isn't set, set it from current position */
    if (self->target_x < 0) {
        PangoRectangle strong_pos;
        pango_layout_get_cursor_pos(layout, (int)MIN(char_idx, len), &strong_pos, NULL);
        self->target_x = pango_units_to_double(strong_pos.x);
    }

    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    int current_v_line_idx = 0;
    do {
        PangoLayoutLine *p_line = pango_layout_iter_get_line_readonly(iter);
        if (char_idx >= p_line->start_index && char_idx <= p_line->start_index + p_line->length) {
            break;
        }
        current_v_line_idx++;
    } while (pango_layout_iter_next_line(iter));
    pango_layout_iter_free(iter);

    int target_v_line_idx = current_v_line_idx + visual_lines_delta;
    int total_v_lines = pango_layout_get_line_count(layout);

    if (target_v_line_idx >= 0 && target_v_line_idx < total_v_lines) {
        /* Move within the same logical line */
        iter = pango_layout_get_iter(layout);
        for (int i = 0; i < target_v_line_idx; i++) pango_layout_iter_next_line(iter);
        PangoLayoutLine *v_line = pango_layout_iter_get_line_readonly(iter);
        
        int index, trailing;
        pango_layout_line_x_to_index(v_line, (int)(self->target_x * PANGO_SCALE), &index, &trailing);
        self->cursor_offset = line_start + index + trailing;
        pango_layout_iter_free(iter);
    } else {
        /* Move to different logical line */
        int logic_delta = (visual_lines_delta > 0) ? 1 : -1;
        size_t next_line_idx = line_idx + logic_delta;
        size_t total_logical = document_get_line_count(self->doc);
        
        if (next_line_idx < total_logical) {
            g_object_unref(layout);
            g_free(text);
            
            layout = create_pango_layout_for_line(self, next_line_idx, &text, &len);
            line_start = document_get_offset_of_line(self->doc, next_line_idx);
            
            int v_count = pango_layout_get_line_count(layout);
            int v_target = (logic_delta > 0) ? 0 : v_count - 1;
            
            iter = pango_layout_get_iter(layout);
            for (int i = 0; i < v_target; i++) pango_layout_iter_next_line(iter);
            PangoLayoutLine *v_line = pango_layout_iter_get_line_readonly(iter);
            
            int index, trailing;
            pango_layout_line_x_to_index(v_line, (int)(self->target_x * PANGO_SCALE), &index, &trailing);
            self->cursor_offset = line_start + index + trailing;
            pango_layout_iter_free(iter);
        }
    }

    g_object_unref(layout);
    g_free(text);
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller,
               guint                  keyval,
               guint                  keycode,
               GdkModifierType        state,
               gpointer               user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->doc) return FALSE;
    
    if (gtk_im_context_filter_keypress(self->im_context, gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller))))
        return TRUE;

    gboolean handled = TRUE;
    
    switch (keyval) {
        case GDK_KEY_Up:
            move_cursor(self, -1);
            if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Down:
            move_cursor(self, 1);
            if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Left:
            if (state & GDK_CONTROL_MASK) {
                self->cursor_offset = word_prev(self->doc, self->cursor_offset);
            } else {
                self->cursor_offset = utf8_prev_grapheme(self->doc, self->cursor_offset);
            }
            if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
            update_target_x(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Right:
            if (state & GDK_CONTROL_MASK) {
                self->cursor_offset = word_next(self->doc, self->cursor_offset);
            } else {
                self->cursor_offset = utf8_next_grapheme(self->doc, self->cursor_offset);
            }
            if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
            update_target_x(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Home:
        {
            size_t line = document_get_line_of_offset(self->doc, self->cursor_offset);
            self->cursor_offset = document_get_offset_of_line(self->doc, line);
            if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
            update_target_x(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        }
        case GDK_KEY_End:
        {
            size_t line = document_get_line_of_offset(self->doc, self->cursor_offset);
            size_t len;
            char *t = document_get_line(self->doc, line, &len);
            g_free(t);
            size_t start = document_get_offset_of_line(self->doc, line);
            size_t real_len = len;
            if (len > 0) {
                 char *last = document_get_text_range(self->doc, start + len - 1, 1);
                 if (last && last[0] == '\n') real_len--;
                 g_free(last);
            }
            self->cursor_offset = start + real_len;
            if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
            update_target_x(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        }
        case GDK_KEY_Page_Up:
        {
             double page = (self->vadjustment) ? gtk_adjustment_get_page_size(self->vadjustment) : 10;
             move_cursor(self, -(int)page);
             if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
             scroll_to_cursor(self);
             gtk_widget_queue_draw(GTK_WIDGET(self));
             break;
        }
        case GDK_KEY_Page_Down:
        {
             double page = (self->vadjustment) ? gtk_adjustment_get_page_size(self->vadjustment) : 10;
             move_cursor(self, (int)page);
             if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
             scroll_to_cursor(self);
             gtk_widget_queue_draw(GTK_WIDGET(self));
             break;   
        }
        case GDK_KEY_Return:
            editor_widget_delete_selection(self);
            document_insert(self->doc, self->cursor_offset, "\n", 1);
            self->cursor_offset++;
            self->selection_anchor = self->cursor_offset;
            editor_widget_reset_cursor_blink(self);
            editor_widget_update_adjustments(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_BackSpace:
            if (editor_widget_delete_selection(self)) {
                editor_widget_reset_cursor_blink(self);
                editor_widget_update_adjustments(self);
                gtk_widget_queue_draw(GTK_WIDGET(self));
            } else if (self->cursor_offset > 0) {
                size_t prev = utf8_prev_grapheme(self->doc, self->cursor_offset);
                size_t bytes_to_delete = self->cursor_offset - prev;
                document_delete(self->doc, prev, bytes_to_delete);
                self->cursor_offset = prev;
                self->selection_anchor = self->cursor_offset;
                editor_widget_reset_cursor_blink(self);
                editor_widget_update_adjustments(self);
                gtk_widget_queue_draw(GTK_WIDGET(self));
            }
            break;
        case GDK_KEY_Delete:
            if (editor_widget_delete_selection(self)) {
                editor_widget_reset_cursor_blink(self);
                editor_widget_update_adjustments(self);
                gtk_widget_queue_draw(GTK_WIDGET(self));
            } else if (self->cursor_offset < document_get_length(self->doc)) {
                size_t next = utf8_next_grapheme(self->doc, self->cursor_offset);
                size_t bytes_to_delete = next - self->cursor_offset;
                document_delete(self->doc, self->cursor_offset, bytes_to_delete);
                self->selection_anchor = self->cursor_offset;
                editor_widget_reset_cursor_blink(self);
                editor_widget_update_adjustments(self);
                gtk_widget_queue_draw(GTK_WIDGET(self));
            }
            break;
        case GDK_KEY_z:
            if (state & GDK_CONTROL_MASK) {
                 document_undo(self->doc);
                 /* Should handle cursor update after undo/redo? */
                 gtk_widget_queue_draw(GTK_WIDGET(self));
            }
            break;
        case GDK_KEY_y:
            if (state & GDK_CONTROL_MASK) {
                 document_redo(self->doc);
                 gtk_widget_queue_draw(GTK_WIDGET(self));
            }
            break;
        case GDK_KEY_a:
            if (state & GDK_CONTROL_MASK) {
                /* Select all */
                self->selection_anchor = 0;
                self->cursor_offset = document_get_length(self->doc);
                gtk_widget_queue_draw(GTK_WIDGET(self));
            } else {
                handled = FALSE;
            }
            break;
        /* IME handles letters */
        default:
            handled = FALSE;
            break;
    }
    
    return handled;
}

static void
on_focus_enter (GtkEventControllerFocus *controller,
                gpointer                 user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    gtk_im_context_focus_in(self->im_context);
}

static void
on_focus_leave (GtkEventControllerFocus *controller,
                gpointer                 user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    gtk_im_context_focus_out(self->im_context);
}

static void
on_im_commit(GtkIMContext *context, const char *str, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->doc) return;
    
    editor_widget_delete_selection(self);
    
    size_t len = strlen(str);
    document_insert(self->doc, self->cursor_offset, str, len);
    self->cursor_offset += len;
    self->selection_anchor = self->cursor_offset;
    
    editor_widget_reset_cursor_blink(self);  /* Keep cursor visible while typing */
    editor_widget_update_adjustments(self);
    scroll_to_cursor(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

/* Cursor blink animation using frame clock tick callback */
static gboolean
cursor_blink_tick_callback(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(widget);
    
    gint64 now = gdk_frame_clock_get_frame_time(frame_clock);
    
    /* First tick - initialize start time */
    if (self->cursor_blink_start_time == 0) {
        self->cursor_blink_start_time = now;
    }
    
    /* Calculate elapsed time in seconds */
    double elapsed = (double)(now - self->cursor_blink_start_time) / 1000000.0;
    
    /* Blink cycle: 1 second period (500ms on, 500ms off) with smooth sine wave */
    /* sin(elapsed * PI * 2) gives a -1 to 1 wave over 1 second */
    /* We map this to 0-1 alpha with smoother fade */
    double phase = sin(elapsed * G_PI * 2.0);  /* -1 to 1 over 1 second */
    self->cursor_alpha = (phase + 1.0) / 2.0;  /* Map to 0-1 */
    
    gtk_widget_queue_draw(widget);
    
    return G_SOURCE_CONTINUE;
}

static void
editor_widget_reset_cursor_blink(EditorWidget *self)
{
    /* Reset blink animation to fully visible when user types or moves cursor */
    self->cursor_blink_start_time = 0;
    self->cursor_alpha = 1.0;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
editor_widget_dispose(GObject *object)
{
    EditorWidget *self = EDITOR_WIDGET(object);
    
    /* Remove cursor blink tick callback */
    if (self->cursor_blink_tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->cursor_blink_tick_id);
        self->cursor_blink_tick_id = 0;
    }
    
    if (self->hadjustment) g_clear_object(&self->hadjustment);
    if (self->vadjustment) g_clear_object(&self->vadjustment);
    if (self->font_desc) pango_font_description_free(self->font_desc);
    if (self->im_context) g_object_unref(self->im_context);
    if (self->syntax_ctx) syntax_context_free(self->syntax_ctx);
    
    G_OBJECT_CLASS(editor_widget_parent_class)->dispose(object);
}

static void
editor_widget_init(EditorWidget *self)
{
    self->font_desc = pango_font_description_from_string("Monospace 12");
    
    /* Initialize cursor blink animation */
    self->cursor_alpha = 1.0;
    self->cursor_blink_start_time = 0;
    self->cursor_blink_tick_id = gtk_widget_add_tick_callback(
        GTK_WIDGET(self), cursor_blink_tick_callback, NULL, NULL);
    
    GtkEventController *controller = gtk_event_controller_key_new();
    g_signal_connect(controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), controller);
    
    self->im_context = gtk_im_context_simple_new();
    gtk_im_context_set_client_widget(self->im_context, GTK_WIDGET(self));
    g_signal_connect(self->im_context, "commit", G_CALLBACK(on_im_commit), self);

    GtkEventController *focus_controller = gtk_event_controller_focus_new();
    g_signal_connect(focus_controller, "enter", G_CALLBACK(on_focus_enter), self);
    g_signal_connect(focus_controller, "leave", G_CALLBACK(on_focus_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), focus_controller);
    
    self->syntax_ctx = syntax_context_new();
    
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), self);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drag));
    
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
}

static void
editor_widget_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
    EditorWidget *self = EDITOR_WIDGET(object);
    switch (prop_id) {
        case PROP_HADJUSTMENT:
            if (self->hadjustment) g_object_unref(self->hadjustment);
            self->hadjustment = g_value_dup_object(value);
            if (self->hadjustment)
                g_signal_connect_swapped(self->hadjustment, "value-changed", G_CALLBACK(gtk_widget_queue_draw), self);
            break;
        case PROP_VADJUSTMENT:
            if (self->vadjustment) g_object_unref(self->vadjustment);
            self->vadjustment = g_value_dup_object(value);
            if (self->vadjustment) {
                 g_signal_connect_swapped(self->vadjustment, "value-changed", G_CALLBACK(gtk_widget_queue_draw), self);
                 editor_widget_update_adjustments(self);
            }
            break;
        case PROP_HSCROLL_POLICY:
            self->hscroll_policy = g_value_get_enum(value);
            break;
        case PROP_VSCROLL_POLICY:
            self->vscroll_policy = g_value_get_enum(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
editor_widget_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
    EditorWidget *self = EDITOR_WIDGET(object);
    switch (prop_id) {
        case PROP_HADJUSTMENT:
            g_value_set_object(value, self->hadjustment);
            break;
        case PROP_VADJUSTMENT:
            g_value_set_object(value, self->vadjustment);
            break;
        case PROP_HSCROLL_POLICY:
            g_value_set_enum(value, self->hscroll_policy);
            break;
        case PROP_VSCROLL_POLICY:
            g_value_set_enum(value, self->vscroll_policy);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}


static void
editor_widget_class_init(EditorWidgetClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

    object_class->set_property = editor_widget_set_property;
    object_class->get_property = editor_widget_get_property;
    object_class->dispose = editor_widget_dispose;

    widget_class->snapshot = editor_widget_snapshot;
    widget_class->measure = editor_widget_measure;
    widget_class->size_allocate = editor_widget_size_allocate;

    g_object_class_override_property(object_class, PROP_HADJUSTMENT, "hadjustment");
    g_object_class_override_property(object_class, PROP_VADJUSTMENT, "vadjustment");
    g_object_class_override_property(object_class, PROP_HSCROLL_POLICY, "hscroll-policy");
    g_object_class_override_property(object_class, PROP_VSCROLL_POLICY, "vscroll-policy");
}

static void
editor_widget_scrollable_init(GtkScrollableInterface *iface)
{
    /* properties handled */
}

GtkWidget *
editor_widget_new(void)
{
    return g_object_new(EDITOR_TYPE_WIDGET, NULL);
}

void
editor_widget_set_document(EditorWidget *self, Document *doc)
{
    self->doc = doc;
    self->cursor_offset = 0;
    self->selection_anchor = 0;
    editor_widget_update_adjustments(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
editor_widget_set_language(EditorWidget *self, const char *lang)
{
    if (self->syntax_ctx) {
        syntax_context_set_language(self->syntax_ctx, lang);
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}
