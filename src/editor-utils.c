#include "editor-internal.h"
#include <math.h>

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

    return (digits * char_w) + 8.0; /* 4px left + 4px right */
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
    
    /* Set font on context for measurements */
    pango_context_set_font_description(context, self->font_desc);
    
    PangoFontMetrics *metrics = pango_context_get_metrics(context, self->font_desc, pango_context_get_language(context));
    // int ascent = pango_font_metrics_get_ascent(metrics); /* Unused variable alert? It was in original code but unused? Original line 855: int ascent = ... */
    /* original: int ascent = pango_font_metrics_get_ascent(metrics); */
    
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
    char *text = document_get_line_truncated(self->doc, line_idx, &len, MAX_PANGO_LINE_LEN + 1024);
    if (!text) return NULL;
    
    /* SAFETY: Embedded nulls can confuse Pango if we pass explict length > strlen. */
    size_t true_len = strlen(text);
    if (true_len < len) len = true_len;

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
    
    int width = gtk_widget_get_width(GTK_WIDGET(self));
    double gutter_w = get_effective_gutter_width(self);

    if (self->wrap_lines) {
        int available_w = width - (gutter_w + self->padding_left);
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
                 if (self->filtered_lines && self->filtered_lines->data) {
                     size_t count = compact_matches_count(self->filtered_lines);
                     size_t *data = self->filtered_lines->data;
                     size_t low = 0, high = count;
                     while (low < high) {
                         size_t mid = low + (high - low) / 2;
                         if (data[mid] <= current_line_idx) low = mid + 1;
                         else high = mid;
                     }
                     if (low < count) next_line = data[low];
                     else next_line = (size_t)-1;
                 } else {
                     next_line = current_line_idx + 1;
                     if (next_line >= document_get_line_count(self->doc)) next_line = (size_t)-1;
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
                 if (self->filtered_lines && self->filtered_lines->data) {
                     size_t count = compact_matches_count(self->filtered_lines);
                     size_t *data = self->filtered_lines->data;
                     size_t low = 0, high = count;
                     while (low < high) {
                         size_t mid = low + (high - low) / 2;
                         if (data[mid] < current_line_idx) low = mid + 1;
                         else high = mid;
                     }
                     if (low > 0) prev_line = data[low - 1];
                     else prev_line = (size_t)-1;
                 } else {
                     if (current_line_idx == 0) prev_line = (size_t)-1;
                     else prev_line = current_line_idx - 1;
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
    if (self->filtered_lines) return compact_matches_count(self->filtered_lines);
    return document_get_line_count(self->doc);
}

size_t
get_physical_line_index(EditorWidget *self, size_t visual_line_idx) {
    if (self->filtered_lines) {
        if (visual_line_idx < compact_matches_count(self->filtered_lines) && self->filtered_lines->data) {
            return self->filtered_lines->data[visual_line_idx];
        }
        return (size_t)-1;
    }
    return visual_line_idx;
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
