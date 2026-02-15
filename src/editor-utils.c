#include "editor-internal.h"
#include <math.h>

double
editor_widget_get_fold_gutter_width(EditorWidget *self)
{
    if (!self->show_line_numbers) return 0.0;
    if (!self->enable_folding) return 0.0;
    
    /* Disable for non-code files */
    if (self->syntax_ctx && syntax_context_get_language(self->syntax_ctx) == LANG_NONE) {
        return 0.0;
    }

    /* Fixed width for fold column */
    return 20.0;
}

/* Helper to get gutter width based on settings */
double
get_effective_gutter_width(EditorWidget *self)
{
    if (!self->show_line_numbers) return 0.0;
    
    if (!self->doc) return 0;
    size_t total_lines = document_get_line_count(self->doc);
    /* Count digits */
    int digits = 1;
    size_t temp = total_lines;
    while (temp >= 10) {
        temp /= 10;
        digits++;
    }
    
    /* Minimum width for 2 digits to avoid jitter */
    if (digits < 2) digits = 2;
    
    /* Calculate width: digits * char_width + padding */
    double char_w = self->cached_char_width;
    if (char_w < 1.0) char_w = self->line_height * 0.5; /* Fallback */

    /* Digits width + 8px padding + Fold Gutter Width */
    return (digits * char_w) + 8.0 + editor_widget_get_fold_gutter_width(self); 
}

int
get_stable_width(EditorWidget *self)
{
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(self));
    /* If we are inside a ScrolledWindow, the parent width is the stable width to wrap against. */
    if (parent) {
        return gtk_widget_get_width(parent);
    }
    return gtk_widget_get_width(GTK_WIDGET(self));
}

/* UTF-8 grapheme cluster navigation helpers */

/* Move cursor right by one grapheme cluster */
size_t
editor_widget_get_grapheme_boundary(EditorWidget *self, size_t offset, gboolean next)
{
    if (!self || !self->doc) return offset;
    size_t total = document_get_length(self->doc);
    if (next && offset >= total) return total;
    if (!next && offset == 0) return 0;

    size_t line_idx = document_get_line_of_offset(self->doc, offset);
    size_t line_start = document_get_offset_of_line(self->doc, line_idx);
    size_t byte_index = offset - line_start;

    size_t line_len;
    char *text = document_get_line(self->doc, line_idx, &line_len);
    if (!text) return next ? MIN(offset + 1, total) : (offset > 0 ? offset - 1 : 0);

    /* Clamp index to line bounds */
    if (byte_index > line_len) byte_index = line_len;

    size_t result_offset = offset;
    if (next) {
        if (byte_index < line_len) {
            /* Move to next code point */
            const char *p = text + byte_index;
            const char *next_p = g_utf8_next_char(p);
            result_offset = line_start + (next_p - text);
        } else {
            /* We are at the end of the line, move to next byte (newline or next line) */
            result_offset = MIN(offset + 1, total);
        }
    } else {
        if (byte_index > 0) {
            /* Move to previous code point */
            const char *p = text + byte_index;
            const char *prev_p = g_utf8_find_prev_char(text, p);
            if (prev_p) {
                result_offset = line_start + (prev_p - text);
            } else {
                result_offset = offset - 1; /* Should not happen if start of line is handled */
            }
        } else {
            /* We are at the start of the line, move to previous byte */
            result_offset = offset - 1;
        }
    }

    g_free(text);
    return result_offset;
}

size_t
utf8_next_grapheme(EditorWidget *self, size_t offset)
{
    return editor_widget_get_grapheme_boundary(self, offset, TRUE);
}

size_t
utf8_prev_grapheme(EditorWidget *self, size_t offset)
{
    return editor_widget_get_grapheme_boundary(self, offset, FALSE);
}

/* Helper: Check if character at offset is a word character */
gboolean
is_word_char_at(EditorWidget *self, size_t offset)
{
    if (!self->doc) return FALSE;
    size_t total = document_get_length(self->doc);
    if (offset >= total) return FALSE;
    
    char *text = document_get_text_range(self->doc, offset, 4); /* Max UTF-8 char */
    if (!text) return FALSE;
    
    gunichar ch = g_utf8_get_char_validated(text, -1);
    g_free(text);
    
    if (ch == (gunichar)-1 || ch == (gunichar)-2) return FALSE;
    return g_unichar_isalnum(ch) || ch == '_' || (g_unichar_isgraph(ch) && !g_unichar_ispunct(ch));
}

/* Find word boundaries around offset */
void
editor_widget_find_word_boundary(EditorWidget *self, size_t offset, size_t *word_start, size_t *word_end)
{
    size_t line_idx = document_get_line_of_offset(self->doc, offset);
    size_t line_start = document_get_offset_of_line(self->doc, line_idx);
    size_t byte_index = offset - line_start;

    char *text;
    size_t len;
    PangoLayout *layout = create_pango_layout_for_line(self, line_idx, &text, &len);
    if (!layout) {
        *word_start = offset;
        *word_end = offset;
        return;
    }

    int n_attrs;
    const PangoLogAttr *attrs = pango_layout_get_log_attrs_readonly(layout, &n_attrs);
    
    /* Convert byte index to character index */
    int char_index = 0;
    const char *p = text;
    const char *target = text + byte_index;
    while (p < target && *p) {
        p = g_utf8_next_char(p);
        char_index++;
    }

    if (char_index >= n_attrs) char_index = n_attrs - 1;

    /* Scan backwards for word start */
    int start_idx = char_index;
    while (start_idx > 0 && !attrs[start_idx].is_word_start) {
        start_idx--;
    }

    /* Scan forwards for word end */
    int end_idx = char_index;
    while (end_idx < n_attrs && !attrs[end_idx].is_word_end) {
        end_idx++;
    }
    
    /* Fallback: if we are on a non-word segment, just select the grapheme */
    if (start_idx > char_index || end_idx <= char_index) {
        start_idx = char_index;
        end_idx = char_index + 1;
    }

    *word_start = line_start + g_utf8_offset_to_pointer(text, start_idx) - text;
    *word_end = line_start + g_utf8_offset_to_pointer(text, end_idx) - text;

    g_object_unref(layout);
    g_free(text);
}

/* Find line boundaries (start and end of line, excluding trailing newline) */
void
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
size_t
word_next(EditorWidget *self, size_t offset)
{
    size_t total = document_get_length(self->doc);
    
    /* Skip current word characters */
    while (offset < total && is_word_char_at(self, offset)) {
        offset = utf8_next_grapheme(self, offset);
    }
    /* Skip whitespace/non-word to reach next word */
    while (offset < total && !is_word_char_at(self, offset)) {
        offset = utf8_next_grapheme(self, offset);
    }
    return offset;
}

/* Move to start of current/previous word */
size_t
word_prev(EditorWidget *self, size_t offset)
{
    if (offset == 0) return 0;
    
    /* Move back one char first */
    offset = utf8_prev_grapheme(self, offset);
    
    /* Skip non-word characters backwards */
    while (offset > 0 && !is_word_char_at(self, offset)) {
        offset = utf8_prev_grapheme(self, offset);
    }
    /* Find start of current word */
    while (offset > 0) {
        size_t prev = utf8_prev_grapheme(self, offset);
        if (!is_word_char_at(self, prev)) break;
        offset = prev;
    }
    return offset;
}

static gboolean
is_whitespace_char_at(EditorWidget *self, size_t offset)
{
    if (!self->doc) return FALSE;
    size_t total = document_get_length(self->doc);
    if (offset >= total) return FALSE;

    char *text = document_get_text_range(self->doc, offset, 4);
    if (!text) return FALSE;

    gunichar ch = g_utf8_get_char_validated(text, -1);
    g_free(text);

    return g_unichar_isspace(ch);
}

typedef enum {
    SEGMENT_WORD,
    SEGMENT_WHITESPACE,
    SEGMENT_OTHER
} SegmentType;

static SegmentType
get_segment_type_at(EditorWidget *self, size_t offset)
{
    if (is_word_char_at(self, offset)) return SEGMENT_WORD;
    if (is_whitespace_char_at(self, offset)) return SEGMENT_WHITESPACE;
    return SEGMENT_OTHER;
}

void
editor_widget_find_segment_boundary(EditorWidget *self, size_t offset, size_t *start, size_t *end)
{
    size_t total = document_get_length(self->doc);
    if (offset >= total) {
        /* Edge case: end of file */
        if (total > 0) offset = total - 1; /* Check char before EOF */
        else {
             *start = 0; *end = 0; return; 
        }
    }

    SegmentType type = get_segment_type_at(self, offset);
    size_t s = offset;
    size_t e = offset;

    /* Expand backwards */
    while (s > 0) {
        size_t prev = utf8_prev_grapheme(self, s);
        if (get_segment_type_at(self, prev) != type) break;
        s = prev;
    }
    /* Expand forwards */
    while (e < total) {
        if (get_segment_type_at(self, e) != type) break;
        e = utf8_next_grapheme(self, e);
    }
    
    *start = s;
    *end = e;
}

/* Ctrl+Right: End of Word -> End of Next Word */
size_t
word_end_next(EditorWidget *self, size_t offset)
{
    size_t total = document_get_length(self->doc);
    if (offset >= total) return total;

    /* Logic:
       1. Check type of current char.
       2. If WHITESPACE: Consume all WS, then consume the NEXT non-WS segment (Word or Other).
       3. If WORD or OTHER: Consume only THIS segment.
    */
    
    SegmentType type = get_segment_type_at(self, offset);
    size_t curr = offset;
    
    if (type == SEGMENT_WHITESPACE) {
        /* Consume Whitespace */
        while (curr < total && get_segment_type_at(self, curr) == SEGMENT_WHITESPACE) {
            curr = utf8_next_grapheme(self, curr);
        }
        
        /* Now we are at start of next segment (Word or Other). Consume it too. */
        if (curr < total) {
            SegmentType next_type = get_segment_type_at(self, curr);
            /* Safety: Don't consume if it turned into another whitespace (unlikely) or EOF */
            while (curr < total && get_segment_type_at(self, curr) == next_type) {
                curr = utf8_next_grapheme(self, curr);
            }
        }
    } else {
        /* Consume homogeneous segment (Word or Other) */
        while (curr < total && get_segment_type_at(self, curr) == type) {
            curr = utf8_next_grapheme(self, curr);
        }
    }
    
    return curr;
}

/* Ctrl+Left: Start of Segment (If WS, merge Prev Segment) */
size_t
word_start_or_prev_end_left(EditorWidget *self, size_t offset)
{
    if (offset == 0) return 0;
    
    size_t prev_off = utf8_prev_grapheme(self, offset);
    SegmentType type = get_segment_type_at(self, prev_off);
    
    size_t curr = offset;
    
    if (type == SEGMENT_WHITESPACE) {
        /* Consume Whitespace */
        while (curr > 0) {
             size_t p = utf8_prev_grapheme(self, curr);
             if (get_segment_type_at(self, p) != SEGMENT_WHITESPACE) break;
             curr = p;
        }
        /* Now consume previous segment (Word or Other) */
        if (curr > 0) {
             prev_off = utf8_prev_grapheme(self, curr);
             SegmentType prev_type = get_segment_type_at(self, prev_off);
             while (curr > 0) {
                 size_t p = utf8_prev_grapheme(self, curr);
                 if (get_segment_type_at(self, p) != prev_type) break;
                 curr = p;
             }
        }
    } else {
        /* Consume homogeneous segment (Word or Other) */
        while (curr > 0) {
            size_t p = utf8_prev_grapheme(self, curr);
            if (get_segment_type_at(self, p) != type) break;
            curr = p;
        }
    }
    return curr;
}

void
editor_widget_ensure_metrics(EditorWidget *self)
{
    if (self->line_height > 0) return;

    PangoContext *context = gtk_widget_get_pango_context(GTK_WIDGET(self));
    const int min_font_size = 6 * PANGO_SCALE;
    const int max_font_size = 48 * PANGO_SCALE;
    const int default_font_size = 11 * PANGO_SCALE;
    
    /* Free existing font description */
    if (self->font_desc) {
        pango_font_description_free(self->font_desc);
        self->font_desc = NULL;
    }
    
    if (self->use_custom_font) {
        /* Use custom font, default to Monospace 11 if name is NULL */
        const char *name = self->font_name ? self->font_name : "Monospace 11";
        self->font_desc = pango_font_description_from_string(name);
    } else {
        /* Use system monospace font from GNOME settings */
        if (self->interface_settings) {
            char *sys_font = g_settings_get_string(self->interface_settings, "monospace-font-name");
            if (sys_font && *sys_font) {
                self->font_desc = pango_font_description_from_string(sys_font);
                g_free(sys_font);
            } else {
                g_free(sys_font);
                self->font_desc = pango_font_description_from_string("Monospace 11");
            }
        } else {
            self->font_desc = pango_font_description_from_string("Monospace 11");
        }
    }

    int base_size = pango_font_description_get_size(self->font_desc);
    if (base_size <= 0) base_size = default_font_size;

    int min_steps = (min_font_size - base_size) / PANGO_SCALE;
    int max_steps = (max_font_size - base_size) / PANGO_SCALE;
    if (self->font_zoom_steps < min_steps) self->font_zoom_steps = min_steps;
    if (self->font_zoom_steps > max_steps) self->font_zoom_steps = max_steps;

    int adjusted_size = base_size + (self->font_zoom_steps * PANGO_SCALE);

    if (pango_font_description_get_size_is_absolute(self->font_desc)) {
        pango_font_description_set_absolute_size(self->font_desc, adjusted_size);
    } else {
        pango_font_description_set_size(self->font_desc, adjusted_size);
    }
    
    /* Set font on context for measurements */
    pango_context_set_font_description(context, self->font_desc);
    
    PangoFontMetrics *metrics = pango_context_get_metrics(context, self->font_desc, pango_context_get_language(context));
    
    /* Create a temporary layout to measure "Hg" dimensions.
       "Hg" is chosen to capture both the ascender (H) and descender (g) 
       of the standard Latin script, giving a more accurate baseline height. */
    PangoLayout *layout = pango_layout_new(context);
    pango_layout_set_font_description(layout, self->font_desc);
    pango_layout_set_text(layout, "Hg", 2);

    PangoRectangle ink_rect, logical_rect;
    pango_layout_get_extents(layout, &ink_rect, &logical_rect);

    /* Get metrics for "Hg" */
    int logical_top = logical_rect.y;
    int baseline = pango_layout_get_baseline(layout);
    
    double hg_height = (double)logical_rect.height / PANGO_SCALE;
    self->cached_char_width = (double)logical_rect.width / PANGO_SCALE / 2.0; /* Approximate char width */
    double hg_ascent = (double)(baseline - logical_top) / PANGO_SCALE;
    
    g_object_unref(layout);


    /* Strict Latin/Code Priority with Visual Comfort:
       We use the "Hg" layout metrics to include descenders.
    */
    double raw_height = hg_height;
    double raw_ascent = hg_ascent;

    /* Remove excessive padding (5% was too much for Arabic).
       We rely on ceil() to provide integer pixel alignment, which naturally adds
       a tiny fraction of padding (up to 1px). */
    self->line_height = ceil(raw_height);
    
    /* Center the text/caret by distributing the extra space (from rounding)
       equally to the top and bottom. */
    double extra_space = self->line_height - raw_height;
    self->ascent = raw_ascent + floor(extra_space / 2.0);

    pango_font_metrics_unref(metrics);
}

EditorCursor *
editor_widget_get_primary_cursor(EditorWidget *self)
{
    if (!self->cursors || self->cursors->len == 0) return NULL;
    return &g_array_index(self->cursors, EditorCursor, 0);
}

void
editor_widget_clear_cursors(EditorWidget *self)
{
    if (self->cursors->len > 1) {
        g_array_set_size(self->cursors, 1);
    }
}

void
editor_widget_add_cursor(EditorWidget *self, size_t offset)
{
    /* Check for existing cursor at this offset and remove it (Toggle behavior) */
    for (guint i = 0; i < self->cursors->len; i++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, i);
        if (cur->cursor_offset == offset && cur->selection_anchor == offset) {
            if (self->cursors->len > 1) {
                g_array_remove_index(self->cursors, i);
            }
            return;
        }
    }

    EditorCursor c;
    c.cursor_offset = offset;
    c.selection_anchor = offset;
    c.target_x = -1;
    g_array_append_val(self->cursors, c);
}

int
compare_cursors_desc(gconstpointer a, gconstpointer b)
{
    const EditorCursor *ca = (const EditorCursor *)a;
    const EditorCursor *cb = (const EditorCursor *)b;
    /* Sort descending by offset */
    if (ca->cursor_offset > cb->cursor_offset) return -1;
    if (ca->cursor_offset < cb->cursor_offset) return 1;
    return 0;
}

PangoLayout *
create_pango_layout_for_line(EditorWidget *self, size_t line_idx, char **out_text, size_t *out_len)
{
    size_t len;
    char *text = document_get_line_truncated(self->doc, line_idx, &len, MAX_PANGO_LINE_LEN + 1024, NULL);
    if (!text) return NULL;
    
    /* SAFETY: Embedded nulls can confuse Pango if we pass explict length > strlen. */
    size_t true_len = strlen(text);
    if (true_len < len) len = true_len;

    /* OPTIMIZATION: If line is massive (>40KB), truncation is handled by document_get_line_truncated 
       (using MAX_PANGO_LINE_LEN = 10MB). 
       However, 10MB layout is still too slow. 
       If this helper is used for METRICS (e.g. word boundary), we might need the full line conceptually,
       but we can't afford it. 
       We truncate to 4096 for general utility usage to prevent stalls.
       Callers requiring full line access (like renderer) use their own virtualization logic.
    */
    if (len > 4096) {
        len = 4096;
        /* Ensure valid UTF-8 cut */
        while (len > 0 && (text[len] & 0xC0) == 0x80) len--;
        text[len] = '\0';
    }

    if (!g_utf8_validate(text, len, NULL)) {
        char *safe = g_utf8_make_valid(text, len);
        g_free(text);
        text = safe;
        len = strlen(text);
    }

    /* Pango doesn't want the trailing newline */
    while (len > 0 && (text[len-1] == '\n' || text[len-1] == '\r')) {
        len--;
    }
    text[len] = '\0';

    /* Use high-level helper to ensure context is correct */
    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), text);
    pango_layout_set_font_description(layout, self->font_desc);
    
    /* Apply Syntax Highlighting for correct metrics (bold, etc) */
    if (self->syntax_ctx) {
        PangoAttrList *attrs = syntax_highlight_line(self->syntax_ctx, line_idx, text);
        if (attrs) {
            pango_layout_set_attributes(layout, attrs);
            pango_attr_list_unref(attrs);
        }
    }

    double gutter_w = get_effective_gutter_width(self);

    if (self->wrap_lines) {
        int width = get_stable_width(self);
        double minimap_w = 0;
        if (self->minimap_enabled) {
            minimap_w = self->minimap_width;
            if (minimap_w > (double)width / 2.0) minimap_w = (double)width / 2.0;
        }
        int available_w = (int)((double)width - (double)gutter_w - (double)self->padding_left - (double)self->active_right_padding - minimap_w);
        if (available_w < 50) available_w = 50; /* Safe min width */
        pango_layout_set_width(layout, available_w * PANGO_SCALE);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    } else {
        pango_layout_set_width(layout, -1);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    }

    if (out_text) *out_text = text;
    else g_free(text);
    if (out_len) *out_len = len;

    return layout;
}

void
update_target_x(EditorWidget *self)
{
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        
        size_t line_idx = document_get_line_of_offset(self->doc, cur->cursor_offset);
        size_t line_start = document_get_offset_of_line(self->doc, line_idx);
        size_t index_in_line = cur->cursor_offset - line_start;
        
        char *text; size_t len;
        
        /* Optimization for long lines: Estimate target_x instead of full layout */
        if (document_get_line_length(self->doc, line_idx) > 4096) {
             double cw = self->cached_char_width > 1.0 ? self->cached_char_width : 8.0;
             cur->target_x = (double)index_in_line * cw;
             continue;
        }

        PangoLayout *layout = create_pango_layout_for_line(self, line_idx, &text, &len);
        if (!layout) continue;
        
        if (index_in_line > len) index_in_line = len;
        
        size_t effective_len = strlen(pango_layout_get_text(layout));
        size_t safe_idx = MIN(index_in_line, effective_len);

        PangoRectangle strong_pos;
        pango_layout_get_cursor_pos(layout, (int)safe_idx, &strong_pos, NULL);
        cur->target_x = pango_units_to_double(strong_pos.x);
        
        g_object_unref(layout);
        g_free(text);
    }
}

void
editor_widget_add_cursor_vertically(EditorWidget *self, int visual_lines_delta)
{
    if (!self->cursors || self->cursors->len == 0) return;
    
    g_array_sort(self->cursors, compare_cursors_desc);
    
    EditorCursor *ref_cur = NULL;
    if (visual_lines_delta > 0) {
         ref_cur = &g_array_index(self->cursors, EditorCursor, 0); 
    } else {
        ref_cur = &g_array_index(self->cursors, EditorCursor, self->cursors->len - 1);
    }
    
    size_t line_idx = document_get_line_of_offset(self->doc, ref_cur->cursor_offset);
    size_t line_start = document_get_offset_of_line(self->doc, line_idx);
    size_t char_idx = ref_cur->cursor_offset - line_start;
    
    char *text = NULL; size_t len;
    PangoLayout *layout = create_pango_layout_for_line(self, line_idx, &text, &len);
    if (!layout) return;
    
    size_t effective_len = strlen(pango_layout_get_text(layout));
    
    double target_x = ref_cur->target_x;
    if (target_x < 0) {
         PangoRectangle strong_pos;
         size_t safe_idx = MIN(char_idx, effective_len);
         pango_layout_get_cursor_pos(layout, (int)safe_idx, &strong_pos, NULL);
         target_x = pango_units_to_double(strong_pos.x);
         ref_cur->target_x = target_x;
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
    
    size_t new_offset = ref_cur->cursor_offset;
    
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
            pango_layout_line_x_to_index(v_line, (int)(target_x * PANGO_SCALE), &index, &trailing);
            size_t ls = document_get_offset_of_line(self->doc, current_line_idx);
            new_offset = ls + index + trailing;
            pango_layout_iter_free(iter);
            break;
        } else {
            if (local_delta > 0) {
                 int lines_remaining = total_v_lines - current_v_line_idx - 1;
                 local_delta -= (lines_remaining + 1);
                 size_t next_line = current_line_idx;
                 if (!editor_widget_get_next_visible_line(self, current_line_idx, &next_line)) {
                     next_line = (size_t)-1;
                 }
                 if (next_line == (size_t)-1) {
                     iter = pango_layout_get_iter(layout);
                     int last_v = total_v_lines - 1; 
                     for (int i = 0; i < last_v; i++) pango_layout_iter_next_line(iter);
                     PangoLayoutLine *v_line = pango_layout_iter_get_line_readonly(iter);
                     int index, trailing;
                     pango_layout_line_x_to_index(v_line, (int)(target_x * PANGO_SCALE), &index, &trailing);
                     size_t ls = document_get_offset_of_line(self->doc, current_line_idx);
                     new_offset = ls + index + trailing;
                     pango_layout_iter_free(iter);
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
                     prev_line = (size_t)-1;
                 }
                 if (prev_line == (size_t)-1) {
                     iter = pango_layout_get_iter(layout);
                     PangoLayoutLine *v_line = pango_layout_iter_get_line_readonly(iter);
                     int index, trailing;
                     pango_layout_line_x_to_index(v_line, (int)(target_x * PANGO_SCALE), &index, &trailing);
                     size_t ls = document_get_offset_of_line(self->doc, current_line_idx);
                     new_offset = ls + index + trailing;
                     pango_layout_iter_free(iter);
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
    
    gboolean exists = FALSE;
    for (guint i=0; i<self->cursors->len; i++) {
        EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
        if (c->cursor_offset == new_offset) { exists = TRUE; break; }
    }
    
    if (!exists) {
        editor_widget_add_cursor(self, new_offset);
        EditorCursor *new_cur = &g_array_index(self->cursors, EditorCursor, self->cursors->len - 1);
        new_cur->target_x = target_x;
    }
}

gboolean
is_alt_word_char_at(Document *doc, size_t offset)
{
    size_t total = document_get_length(doc);
    if (offset >= total) return FALSE;
    
    char *text = document_get_text_range(doc, offset, 4);
    if (!text) return FALSE;
    
    gunichar ch = g_utf8_get_char_validated(text, -1);
    g_free(text);
    
    if (ch == (gunichar)-1 || ch == (gunichar)-2) return FALSE;
    
    /* For Alt+Arrow, _ is a separator */
    if (ch == '_') return FALSE;
    
    return g_unichar_isalnum(ch);
}

size_t
get_visual_line_count(EditorWidget *self) {
    if (!self->doc) return 0;
    if (self->visible_lines) return compact_matches_count(self->visible_lines);
    if (self->filtered_lines) return compact_matches_count(self->filtered_lines);
    return document_get_line_count(self->doc);
}

size_t
get_physical_line_index(EditorWidget *self, size_t visual_line_idx) {
    if (self->visible_lines) {
        if (visual_line_idx < compact_matches_count(self->visible_lines) && self->visible_lines->data) {
            return self->visible_lines->data[visual_line_idx];
        }
        return (size_t)-1;
    }
    if (self->filtered_lines) {
        if (visual_line_idx < compact_matches_count(self->filtered_lines) && self->filtered_lines->data) {
            return self->filtered_lines->data[visual_line_idx];
        }
        return (size_t)-1;
    }
    return visual_line_idx;
}

static gint
fold_range_compare(gconstpointer a, gconstpointer b)
{
    const FoldRange *ra = (const FoldRange *)a;
    const FoldRange *rb = (const FoldRange *)b;
    if (ra->start_line < rb->start_line) return -1;
    if (ra->start_line > rb->start_line) return 1;
    return 0;
}

static gboolean
editor_widget_line_has_text(const char *text, size_t len, int tab_width, int *out_indent)
{
    int col = 0;
    size_t i = 0;
    while (i < len) {
        char c = text[i];
        if (c == ' ') {
            col++;
            i++;
            continue;
        }
        if (c == '\t') {
            int step = tab_width > 0 ? tab_width : 4;
            int rem = col % step;
            col += (step - rem);
            i++;
            continue;
        }
        if (c == '\r' || c == '\n') {
            i++;
            continue;
        }
        if (out_indent) *out_indent = col;
        return TRUE;
    }
    if (out_indent) *out_indent = col;
    return FALSE;
}

gboolean
editor_widget_get_fold_range(EditorWidget *self, size_t line_idx, FoldRange *out_range)
{
    if (!self->fold_ranges || self->fold_ranges->len == 0) return FALSE;
    size_t low = 0;
    size_t high = self->fold_ranges->len;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        FoldRange r = g_array_index(self->fold_ranges, FoldRange, mid);
        if (r.start_line == line_idx) {
            if (out_range) *out_range = r;
            return TRUE;
        }
        if (r.start_line < line_idx) low = mid + 1;
        else high = mid;
    }
    return FALSE;
}

gboolean
editor_widget_is_fold_collapsed(EditorWidget *self, size_t line_idx)
{
    if (!self->fold_collapsed || self->fold_collapsed->len == 0) return FALSE;
    size_t low = 0;
    size_t high = self->fold_collapsed->len;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        size_t v = g_array_index(self->fold_collapsed, size_t, mid);
        if (v == line_idx) return TRUE;
        if (v < line_idx) low = mid + 1;
        else high = mid;
    }
    return FALSE;
}

static void
editor_widget_compact_collapsed_ranges(EditorWidget *self)
{
    if (self->collapsed_ranges) {
        g_array_set_size(self->collapsed_ranges, 0);
    } else {
        self->collapsed_ranges = g_array_new(FALSE, FALSE, sizeof(FoldRange));
    }

    if (!self->fold_collapsed || self->fold_collapsed->len == 0) return;
    if (!self->fold_ranges || self->fold_ranges->len == 0) return;

    GArray *ranges = g_array_new(FALSE, FALSE, sizeof(FoldRange));
    for (guint i = 0; i < self->fold_collapsed->len; i++) {
        size_t start = g_array_index(self->fold_collapsed, size_t, i);
        FoldRange r;
        if (editor_widget_get_fold_range(self, start, &r)) {
            g_array_append_val(ranges, r);
        }
    }

    if (ranges->len == 0) {
        g_array_free(ranges, TRUE);
        return;
    }

    g_array_sort(ranges, (GCompareFunc)fold_range_compare);

    FoldRange current = g_array_index(ranges, FoldRange, 0);
    for (guint i = 1; i < ranges->len; i++) {
        FoldRange next = g_array_index(ranges, FoldRange, i);
        if (next.start_line <= current.end_line + 1) {
            if (next.end_line > current.end_line) current.end_line = next.end_line;
        } else {
            g_array_append_val(self->collapsed_ranges, current);
            current = next;
        }
    }
    g_array_append_val(self->collapsed_ranges, current);
    g_array_free(ranges, TRUE);
}

static gboolean
editor_widget_line_hidden_by_fold(EditorWidget *self, size_t phys_line, FoldRange *out_range)
{
    if (!self->collapsed_ranges || self->collapsed_ranges->len == 0) return FALSE;
    size_t low = 0;
    size_t high = self->collapsed_ranges->len;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        FoldRange r = g_array_index(self->collapsed_ranges, FoldRange, mid);
        if (phys_line <= r.start_line) {
            high = mid;
        } else if (phys_line > r.end_line) {
            low = mid + 1;
        } else {
            if (out_range) *out_range = r;
            return TRUE;
        }
    }
    return FALSE;
}

void
editor_widget_rebuild_folding(EditorWidget *self)
{
    if (!self->doc) return;

    /* Disable for non-code files */
    if (self->syntax_ctx && syntax_context_get_language(self->syntax_ctx) == LANG_NONE) {
        /* Clear any existing folds */
        if (self->fold_ranges) g_array_set_size(self->fold_ranges, 0);
        if (self->fold_collapsed) g_array_set_size(self->fold_collapsed, 0);
        /* Ensure visible lines are rebuilt to show everything */
        editor_widget_rebuild_visible_lines(self);
        return;
    }

    if (self->fold_ranges) {
        g_array_set_size(self->fold_ranges, 0);
    } else {
        self->fold_ranges = g_array_new(FALSE, FALSE, sizeof(FoldRange));
    }

    size_t total = document_get_line_count(self->doc);
    size_t prev_nonblank = (size_t)-1;
    int prev_indent = 0;

    GArray *stack_lines = g_array_new(FALSE, FALSE, sizeof(size_t));
    GArray *stack_indents = g_array_new(FALSE, FALSE, sizeof(int));

    for (size_t i = 0; i < total; i++) {
        size_t len = 0;
        char *line = document_get_line(self->doc, i, &len);
        if (!line) continue;
        int indent = 0;
        gboolean has_text = editor_widget_line_has_text(line, len, self->tab_width, &indent);
        g_free(line);
        if (!has_text) continue;

        while (stack_lines->len > 0) {
            int top_indent = g_array_index(stack_indents, int, stack_indents->len - 1);
            if (indent > top_indent) break;
            size_t start_line = g_array_index(stack_lines, size_t, stack_lines->len - 1);
            if (prev_nonblank != (size_t)-1 && prev_nonblank > start_line) {
                FoldRange r = {start_line, prev_nonblank};
                g_array_append_val(self->fold_ranges, r);
            }
            g_array_set_size(stack_lines, stack_lines->len - 1);
            g_array_set_size(stack_indents, stack_indents->len - 1);
        }

        if (prev_nonblank != (size_t)-1 && indent > prev_indent) {
            g_array_append_val(stack_lines, prev_nonblank);
            g_array_append_val(stack_indents, prev_indent);
        }

        prev_nonblank = i;
        prev_indent = indent;
    }

    while (stack_lines->len > 0) {
        size_t start_line = g_array_index(stack_lines, size_t, stack_lines->len - 1);
        if (prev_nonblank != (size_t)-1 && prev_nonblank > start_line) {
            FoldRange r = {start_line, prev_nonblank};
            g_array_append_val(self->fold_ranges, r);
        }
        g_array_set_size(stack_lines, stack_lines->len - 1);
        g_array_set_size(stack_indents, stack_indents->len - 1);
    }

    g_array_free(stack_lines, TRUE);
    g_array_free(stack_indents, TRUE);

    if (self->fold_ranges->len > 1) {
        g_array_sort(self->fold_ranges, (GCompareFunc)fold_range_compare);
    }

    if (self->fold_collapsed && self->fold_collapsed->len > 0) {
        for (gint i = (gint)self->fold_collapsed->len - 1; i >= 0; i--) {
            size_t start = g_array_index(self->fold_collapsed, size_t, i);
            if (!editor_widget_get_fold_range(self, start, NULL)) {
                g_array_remove_index(self->fold_collapsed, i);
            }
        }
    }

    editor_widget_compact_collapsed_ranges(self);
    editor_widget_rebuild_visible_lines(self);
}

void
editor_widget_rebuild_visible_lines(EditorWidget *self)
{
    if (!self->doc) return;
    if (self->visible_lines) {
        compact_matches_free(self->visible_lines);
        self->visible_lines = NULL;
    }

    gboolean has_filter = (self->filtered_lines && self->filtered_lines->data && compact_matches_count(self->filtered_lines) > 0);
    gboolean has_folds = (self->collapsed_ranges && self->collapsed_ranges->len > 0);

    if (!has_filter && !has_folds) {
        return;
    }

    self->visible_lines = compact_matches_new(1);

    if (has_filter) {
        size_t count = compact_matches_count(self->filtered_lines);
        size_t *data = self->filtered_lines->data;
        for (size_t i = 0; i < count; i++) {
            size_t line = data[i];
            FoldRange r;
            if (editor_widget_line_hidden_by_fold(self, line, &r)) {
                continue;
            }
            compact_matches_append(self->visible_lines, line);
        }
    } else {
        size_t total = document_get_line_count(self->doc);
        for (size_t line = 0; line < total; line++) {
            FoldRange r;
            if (editor_widget_line_hidden_by_fold(self, line, &r)) {
                continue;
            }
            compact_matches_append(self->visible_lines, line);
        }
    }
}

gboolean
editor_widget_toggle_fold(EditorWidget *self, size_t line_idx)
{
    FoldRange r;
    if (!editor_widget_get_fold_range(self, line_idx, &r)) return FALSE;

    /* 1. Capture State Before Toggle */
    size_t old_visual_line = 0;
    gboolean was_visible = editor_widget_get_visual_line_for_physical(self, line_idx, &old_visual_line);
    
    double scroll_y = gtk_adjustment_get_value(self->vadjustment);
    
    /* Calculate Old Y robustly */
    double old_line_y = 0.0;
    if (self->line_y_offsets && old_visual_line < self->line_y_offsets->len) {
        old_line_y = g_array_index(self->line_y_offsets, double, old_visual_line);
    } else {
        double factor = (self->wrap_lines && self->avg_visual_lines > 1.0) ? self->avg_visual_lines : 1.0;
        old_line_y = (double)old_visual_line * self->line_height * factor;
    }
    
    double screen_offset = old_line_y - scroll_y; /* Distance from top of viewport */

    if (!self->fold_collapsed) {
        self->fold_collapsed = g_array_new(FALSE, FALSE, sizeof(size_t));
    }

    gboolean is_collapsed = editor_widget_is_fold_collapsed(self, line_idx);
    if (is_collapsed) {
        for (guint i = 0; i < self->fold_collapsed->len; i++) {
            size_t v = g_array_index(self->fold_collapsed, size_t, i);
            if (v == line_idx) {
                g_array_remove_index(self->fold_collapsed, i);
                break;
            }
        }
    } else {
        size_t insert_at = 0;
        while (insert_at < self->fold_collapsed->len) {
            size_t v = g_array_index(self->fold_collapsed, size_t, insert_at);
            if (v > line_idx) break;
            insert_at++;
        }
        g_array_insert_val(self->fold_collapsed, insert_at, line_idx);
    }

    editor_widget_compact_collapsed_ranges(self);
    editor_widget_rebuild_visible_lines(self);

    if (self->collapsed_ranges && self->collapsed_ranges->len > 0) {
        for (guint c = 0; c < self->cursors->len; c++) {
            EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
            size_t line = document_get_line_of_offset(self->doc, cur->cursor_offset);
            FoldRange fr;
            if (editor_widget_line_hidden_by_fold(self, line, &fr)) {
                size_t start_off = document_get_offset_of_line(self->doc, fr.start_line);
                cur->cursor_offset = start_off;
                cur->selection_anchor = start_off;
                cur->target_x = -1;
            }
        }
    }
    
    /* 2. Restore Scroll Position */
    /* Ensure adjustments are updated - this recalculates offsets/averages */
    editor_widget_update_adjustments(self, -1, -1);
    
    if (was_visible) {
        size_t new_visual_line = 0;
        /* The line itself *should* still be visible (it's the header) */
        if (editor_widget_get_visual_line_for_physical(self, line_idx, &new_visual_line)) {
            
            /* Calculate New Y robustly */
            double new_line_y = 0.0;
            if (self->line_y_offsets && new_visual_line < self->line_y_offsets->len) {
                new_line_y = g_array_index(self->line_y_offsets, double, new_visual_line);
            } else {
                double factor = (self->wrap_lines && self->avg_visual_lines > 1.0) ? self->avg_visual_lines : 1.0;
                new_line_y = (double)new_visual_line * self->line_height * factor;
            }
            
            double target_scroll_y = new_line_y - screen_offset;
            
            /* Clamp to valid range */
            double max_val = gtk_adjustment_get_upper(self->vadjustment) - gtk_adjustment_get_page_size(self->vadjustment);
            if (target_scroll_y < 0) target_scroll_y = 0;
            if (target_scroll_y > max_val) target_scroll_y = max_val;
            
            gtk_adjustment_set_value(self->vadjustment, target_scroll_y);
        }
    }

    return TRUE;
}

gboolean
editor_widget_get_visual_line_for_physical(EditorWidget *self, size_t phys_line, size_t *out_visual)
{
    if (!self->doc) return FALSE;
    if (!self->visible_lines) {
        if (out_visual) *out_visual = phys_line;
        return TRUE;
    }
    size_t count = compact_matches_count(self->visible_lines);
    if (count == 0 || !self->visible_lines->data) return FALSE;
    size_t *data = self->visible_lines->data;

    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (data[mid] < phys_line) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low < count && data[low] == phys_line) {
        if (out_visual) *out_visual = low;
        return TRUE;
    }
    return FALSE;
}

gboolean
editor_widget_get_next_visible_line(EditorWidget *self, size_t phys_line, size_t *out_line)
{
    if (!self->visible_lines || !self->visible_lines->data) {
        size_t next = phys_line + 1;
        if (next >= document_get_line_count(self->doc)) return FALSE;
        *out_line = next;
        return TRUE;
    }

    size_t count = compact_matches_count(self->visible_lines);
    if (count == 0) return FALSE;
    size_t *data = self->visible_lines->data;

    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (data[mid] <= phys_line) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low >= count) return FALSE;
    *out_line = data[low];
    return TRUE;
}

gboolean
editor_widget_get_prev_visible_line(EditorWidget *self, size_t phys_line, size_t *out_line)
{
    if (!self->visible_lines || !self->visible_lines->data) {
        if (phys_line == 0) return FALSE;
        *out_line = phys_line - 1;
        return TRUE;
    }

    size_t count = compact_matches_count(self->visible_lines);
    if (count == 0) return FALSE;
    size_t *data = self->visible_lines->data;

    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (data[mid] < phys_line) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    if (low == 0) return FALSE;
    *out_line = data[low - 1];
    return TRUE;
}

void
editor_widget_reset_cursor_to_start(EditorWidget *self)
{
    if (self->cursors) {
        g_array_set_size(self->cursors, 0);
        EditorCursor c = {0};
        c.target_x = -1;
        g_array_append_val(self->cursors, c);
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

char *
editor_widget_get_selected_text(EditorWidget *self)
{
    EditorCursor *c = editor_widget_get_primary_cursor(self);
    if (!c) return NULL;
    
    size_t start = MIN(c->cursor_offset, c->selection_anchor);
    size_t end = MAX(c->cursor_offset, c->selection_anchor);
    
    if (start == end) return NULL;
    
    if (!self->doc) return NULL;
    
    return document_get_text_range(self->doc, start, end - start);
}

void
editor_widget_get_cursor_position(EditorWidget *self, size_t *line, size_t *col)
{
    if (!self->cursors || self->cursors->len == 0) {
        if (line) *line = 0;
        if (col) *col = 0;
        return;
    }
    
    EditorCursor *c = &g_array_index(self->cursors, EditorCursor, 0);
    size_t line_idx = document_get_line_of_offset(self->doc, c->cursor_offset);
    
    if (line) *line = line_idx;
    
    if (col) {
        size_t line_start = document_get_offset_of_line(self->doc, line_idx);
        size_t offset_in_line = c->cursor_offset - line_start;
        
        size_t len;
        char *line_text = document_get_line(self->doc, line_idx, &len);
        if (line_text) {
             long char_count = g_utf8_pointer_to_offset(line_text, line_text + MIN(offset_in_line, len));
             *col = (char_count >= 0) ? (size_t)char_count : 0;
             g_free(line_text);
        } else {
             *col = 0;
        }
    }
}
