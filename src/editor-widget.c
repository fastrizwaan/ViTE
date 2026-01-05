#include "editor-widget.h"
#include "syntax.h"
#include <math.h>

/* Fold Region - represents a collapsible block */
typedef struct {
    size_t start_line;    /* Line where fold starts (the "head") */
    size_t end_line;      /* Line where fold ends (inclusive) */
    gboolean is_folded;   /* TRUE if currently collapsed */
} FoldRegion;

typedef struct {
    size_t pos;        /* Cursor position (head) */
    size_t anchor;     /* Selection anchor (tail) */
    double target_x;   /* Target X for vertical movement */
} EditorCursor;

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
    double cached_char_width;
    PangoFontDescription *font_desc;
    
    /* State */
    GArray *cursors; /* Array of EditorCursor */
    
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

    gboolean alt_word_mode; /* TRUE if selection was auto-created for word swapping */

    /* Advanced Drag and Drop */
    double drag_x, drag_y;
    size_t drag_drop_offset;
    gboolean drag_copy_mode;
    PangoLayout *drag_ghost_layout;
    gboolean is_dnd_active; /* TRUE only after passing 8px threshold */
    
    /* Autoscroll timer for smooth edge scrolling */
    guint autoscroll_timer_id;
    int autoscroll_direction; /* -1 = up, 0 = none, 1 = down */
    double autoscroll_speed;  /* Lines per tick */
    guint autoscroll_tick_count; /* For throttling hit-tests */
    
    /* Viewport padding */
    int padding_left;
    int padding_top;

    /* Configuration Properties */
    gboolean show_line_numbers;
    gboolean highlight_current_line;

    gboolean show_right_margin;
    int right_margin_position;
    gboolean wrap_lines;
    gboolean auto_indent;
    int indent_style; /* 0 = Space, 1 = Tab */
    int tab_width;
    int indent_width;
    gboolean use_custom_font;
    char *font_name;

    /* Cached scroll upper bound (recalculated only when dimensions change) */
    double cached_scroll_upper;
    int cached_width;
    int cached_height;
    size_t cached_line_count;
    
    /* Cache for Y positions of all lines (accumulated height) */
    GArray *line_y_offsets;
    
    /* Idle resize handler to prevent blocking UI on every frame */
    guint idle_resize_id;
    
    /* Statistical Scroll for Large Files */
    double avg_visual_lines;
    
    /* Code Folding - High Performance */
    GHashTable *fold_heads;           /* line_idx -> FoldRegion* for fold start lines */
    guint8 *hidden_lines_cache;       /* Bitfield: 1 = hidden */
    size_t hidden_cache_size;         /* Allocated bytes */
    gboolean hidden_cache_valid;      /* TRUE if cache is current */
    size_t last_analyzed_line;        /* Lazy analysis watermark */
    
    /* System font monitoring */
    GSettings *interface_settings;
};

/* Helper to access primary cursor (last added usually, or index 0) */
static EditorCursor *
get_primary_cursor(EditorWidget *self)
{
    if (!self->cursors || self->cursors->len == 0) return NULL;
    /* Use the last one as primary? Or 0? 
       For Alt+Click, the new one is appended. So last is active. */
    return &g_array_index(self->cursors, EditorCursor, self->cursors->len - 1);
}

static void
editor_widget_add_cursor(EditorWidget *self, size_t pos)
{
    EditorCursor c;
    c.pos = pos;
    c.anchor = pos;
    c.target_x = -1;
    g_array_append_val(self->cursors, c);
}

static void
editor_widget_clear_cursors(EditorWidget *self)
{
    if (self->cursors->len > 1) {
        /* Keep the ORIGINAL one (index 0) */
        EditorCursor original = g_array_index(self->cursors, EditorCursor, 0);
        g_array_set_size(self->cursors, 0);
        g_array_append_val(self->cursors, original);
    }
}


static void editor_widget_scrollable_init (GtkScrollableInterface *iface);
static void editor_widget_ensure_metrics(EditorWidget *self);
static void editor_widget_reset_cursor_blink(EditorWidget *self);
static double get_effective_gutter_width(EditorWidget *self);
static void editor_widget_drag_drop_finish(EditorWidget *self, size_t drop_off);
static void update_target_x(EditorWidget *self);
static void editor_widget_copy(EditorWidget *self);
static void editor_widget_cut(EditorWidget *self);
static void editor_widget_paste(EditorWidget *self);
static void editor_widget_paste_primary(EditorWidget *self);
static void move_cursor(EditorWidget *self, int visual_lines_delta);
static void editor_widget_update_im_cursor_location(EditorWidget *self);
static PangoLayout *create_pango_layout_for_line(EditorWidget *self, size_t line_idx, char **out_text, size_t *out_len);
static void update_selection_extension(EditorWidget *self, size_t off);

static double
get_gutter_width(EditorWidget *self)
{
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

    return (digits * char_w) + 24.0; /* 4px left + 4px right + 16px fold icon */
}

/* Helper to get gutter width based on settings */
static double
get_effective_gutter_width(EditorWidget *self)
{
    if (!self->show_line_numbers) return 0.0;
    return get_gutter_width(self);
}

G_DEFINE_TYPE_WITH_CODE (EditorWidget, editor_widget, GTK_TYPE_WIDGET,
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, editor_widget_scrollable_init))

enum {
    PROP_0,
    PROP_HADJUSTMENT,
    PROP_VADJUSTMENT,
    PROP_HSCROLL_POLICY,
    PROP_VSCROLL_POLICY,
    PROP_SHOW_LINE_NUMBERS,
    PROP_HIGHLIGHT_CURRENT_LINE,

    PROP_SHOW_RIGHT_MARGIN,
    PROP_RIGHT_MARGIN_POSITION,
    PROP_WRAP_LINES,
    PROP_AUTO_INDENT,
    PROP_INDENT_STYLE,
    PROP_TAB_WIDTH,
    PROP_INDENT_WIDTH,
    PROP_USE_CUSTOM_FONT,
    PROP_FONT_NAME,
    N_PROPS
};

/* ========== HIGH-PERFORMANCE CODE FOLDING ========== */

/* Get indentation level of a line (O(line_length), cached by caller) */
static int
get_line_indent(EditorWidget *self, size_t line_idx)
{
    size_t len;
    char *text = document_get_line(self->doc, line_idx, &len);
    if (!text || len == 0) {
        g_free(text);
        return -1;  /* Empty/whitespace-only returns -1 */
    }
    
    int indent = 0;
    gboolean has_content = FALSE;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == ' ') {
            indent++;
        } else if (text[i] == '\t') {
            indent += self->tab_width;
        } else if (text[i] != '\n' && text[i] != '\r') {
            has_content = TRUE;
            break;
        }
    }
    
    g_free(text);
    return has_content ? indent : -1;
}

/* Free a FoldRegion (for hash table) */
static void
fold_region_free(gpointer data)
{
    g_slice_free(FoldRegion, data);
}

/* Initialize fold hash table if needed */
static void
ensure_fold_table(EditorWidget *self)
{
    if (!self->fold_heads) {
        self->fold_heads = g_hash_table_new_full(
            g_direct_hash, g_direct_equal, NULL, fold_region_free);
    }
}

/* O(1) hidden line check using bitset cache */
static gboolean
is_line_hidden(EditorWidget *self, size_t line_idx)
{
    if (!self->hidden_cache_valid || !self->hidden_lines_cache) return FALSE;
    if (line_idx >= self->hidden_cache_size * 8) return FALSE;
    return (self->hidden_lines_cache[line_idx / 8] >> (line_idx % 8)) & 1;
}

/* Rebuild hidden lines cache from fold_heads (only when needed) */
static void
rebuild_hidden_cache(EditorWidget *self)
{
    if (!self->doc) return;
    
    size_t line_count = document_get_line_count(self->doc);
    size_t bytes_needed = (line_count + 7) / 8;
    
    /* Reallocate if needed */
    if (self->hidden_cache_size < bytes_needed) {
        g_free(self->hidden_lines_cache);
        self->hidden_lines_cache = g_malloc0(bytes_needed);
        self->hidden_cache_size = bytes_needed;
    } else {
        memset(self->hidden_lines_cache, 0, bytes_needed);
    }
    
    if (!self->fold_heads) {
        self->hidden_cache_valid = TRUE;
        return;
    }
    
    /* Mark hidden lines in bitfield */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->fold_heads);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        FoldRegion *r = (FoldRegion *)value;
        if (r->is_folded) {
            /* Lines start_line+1 through end_line are hidden */
            for (size_t line = r->start_line + 1; line <= r->end_line && line < line_count; line++) {
                self->hidden_lines_cache[line / 8] |= (1 << (line % 8));
            }
        }
    }
    self->hidden_cache_valid = TRUE;
}

/* Get fold region at line (O(1) hash lookup) */
static FoldRegion *
get_fold_at_line(EditorWidget *self, size_t line_idx)
{
    if (!self->fold_heads) return NULL;
    return g_hash_table_lookup(self->fold_heads, GSIZE_TO_POINTER(line_idx));
}

/* Lazy fold analysis - only analyze up to end_line if not already done */
static void
ensure_folds_for_range(EditorWidget *self, size_t start_line, size_t end_line)
{
    if (!self->doc) return;
    
    size_t line_count = document_get_line_count(self->doc);
    if (end_line >= line_count) end_line = line_count - 1;
    
    /* Already analyzed past this point */
    if (end_line < self->last_analyzed_line) return;
    
    ensure_fold_table(self);
    
    /* Stack-based fold detection from last_analyzed_line */
    typedef struct { size_t start; int indent; } FoldScope;
    GArray *stack = g_array_new(FALSE, FALSE, sizeof(FoldScope));
    
    /* Restore context from last analysis point */
    int last_indent = 0;
    size_t last_non_empty = (size_t)-1;
    
    /* If resuming, we need context from before last_analyzed_line */
    if (self->last_analyzed_line > 0 && self->last_analyzed_line < line_count) {
        /* Find last non-empty line before our start point for context */
        for (size_t i = self->last_analyzed_line; i > 0; i--) {
            int indent = get_line_indent(self, i - 1);
            if (indent >= 0) {
                last_indent = indent;
                last_non_empty = i - 1;
                break;
            }
        }
    }
    
    size_t analyze_start = self->last_analyzed_line;
    
    for (size_t i = analyze_start; i <= end_line && i < line_count; i++) {
        int indent = get_line_indent(self, i);
        if (indent < 0) continue;  /* Skip empty lines */
        
        /* Close folds that ended (dedent) */
        while (stack->len > 0) {
            FoldScope *top = &g_array_index(stack, FoldScope, stack->len - 1);
            if (indent < top->indent) {
                /* Fold ended at previous non-empty line */
                if (last_non_empty != (size_t)-1 && last_non_empty > top->start) {
                    FoldRegion *region = g_slice_new(FoldRegion);
                    region->start_line = top->start;
                    region->end_line = last_non_empty;
                    region->is_folded = FALSE;
                    
                    /* Don't overwrite existing fold (preserve fold state) */
                    if (!g_hash_table_contains(self->fold_heads, GSIZE_TO_POINTER(top->start))) {
                        g_hash_table_insert(self->fold_heads, GSIZE_TO_POINTER(top->start), region);
                    } else {
                        g_slice_free(FoldRegion, region);
                    }
                }
                g_array_remove_index(stack, stack->len - 1);
            } else {
                break;
            }
        }
        
        /* Open new fold (indent) */
        if (indent > last_indent && last_non_empty != (size_t)-1) {
            FoldScope scope = { last_non_empty, indent };
            g_array_append_val(stack, scope);
        }
        
        last_non_empty = i;
        last_indent = indent;
    }
    
    /* Close remaining open folds at analysis end */
    while (stack->len > 0) {
        FoldScope *top = &g_array_index(stack, FoldScope, stack->len - 1);
        if (last_non_empty != (size_t)-1 && last_non_empty > top->start) {
            FoldRegion *region = g_slice_new(FoldRegion);
            region->start_line = top->start;
            region->end_line = last_non_empty;
            region->is_folded = FALSE;
            
            if (!g_hash_table_contains(self->fold_heads, GSIZE_TO_POINTER(top->start))) {
                g_hash_table_insert(self->fold_heads, GSIZE_TO_POINTER(top->start), region);
            } else {
                g_slice_free(FoldRegion, region);
            }
        }
        g_array_remove_index(stack, stack->len - 1);
    }
    
    g_array_free(stack, TRUE);
    
    self->last_analyzed_line = end_line + 1;
}

/* Invalidate folds from a line onwards (called on document edit) */
static void
invalidate_folds_from_line(EditorWidget *self, size_t line_idx)
{
    if (line_idx < self->last_analyzed_line) {
        self->last_analyzed_line = line_idx;
    }
    self->hidden_cache_valid = FALSE;
}

/* Toggle fold at line */
static void
toggle_fold_at_line(EditorWidget *self, size_t line_idx)
{
    FoldRegion *fold = get_fold_at_line(self, line_idx);
    if (fold) {
        fold->is_folded = !fold->is_folded;
        self->hidden_cache_valid = FALSE;
        rebuild_hidden_cache(self);
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

/* ========== END CODE FOLDING ========== */

/* Scroll Calculation Helper */
typedef struct {
    double current_y;
    GArray *offsets;
    int chars_per_line;
    double line_height;
} ScrollCalcState;

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

static void
editor_widget_update_adjustments(EditorWidget *self, int widget_width, int widget_height)
{
    if (!self->vadjustment || !self->doc) return;
    
    /* If called with -1, use current */
    if (widget_width < 0) widget_width = gtk_widget_get_width(GTK_WIDGET(self));
    if (widget_height < 0) widget_height = gtk_widget_get_height(GTK_WIDGET(self));

    editor_widget_ensure_metrics(self);

    size_t total_lines = document_get_line_count(self->doc);
    
    double content_height = 0;

    if (!self->wrap_lines || total_lines == 0) {
        /* Simple calculation for no-wrap mode or empty doc */
        content_height = (double)total_lines * self->line_height + self->padding_top * 2;
        
        /* Optimization: For linear files, clear the offset cache to save memory and time O(1).
           Snapshot and scroll_to_cursor will fallback to arithmetic. */
        if (self->line_y_offsets) {
             g_array_set_size(self->line_y_offsets, 0);
        }
    } else {
        /* Accurate calculation for wrapped lines */
        double text_start_x = get_effective_gutter_width(self) + self->padding_left;
        double wrap_width = (double)widget_width - text_start_x;
        
        if (wrap_width < 1.0) wrap_width = 1.0;
        
        /* Estimate chars per line using cached char width */
        int chars_per_line = (int)(wrap_width / self->cached_char_width);
        if (chars_per_line < 1) chars_per_line = 1;


        /* Prepare offsets array */
        g_array_set_size(self->line_y_offsets, 0);
        
        /* Strategy Switch: Exact vs Statistical */
        if (total_lines > 50000) {
            /* Statistical Scroll Mode (O(1)) */
            /* Do not populate line_y_offsets. Estimate avg visual lines. */
            size_t total_bytes = document_get_length(self->doc);
            double avg_bytes = (total_lines > 0) ? (double)total_bytes / total_lines : 0;
            
            /* Estimate visual lines per logical line */
            // average bytes / chars_per_line gives raw rows.
            // But real text isn't a solid block. 
            // Factor of 1.0 is safe lower bound.
            double est_rows = avg_bytes / (double)chars_per_line;
            if (est_rows < 1.0) est_rows = 1.0;
            
            self->avg_visual_lines = est_rows;
            
            /* Calculate total height estimate */
            content_height = (double)total_lines * self->avg_visual_lines * self->line_height + self->padding_top * 2;
             
        } else {
            /* Exact Scan Mode (O(N)) */
            self->avg_visual_lines = 1.0; /* Reset */
            
            /* Pre-allocate estimation? total_lines is known, but exact count not strict req for GArray */
            /* g_array_set_size handles growth, no manual pre-alloc needed */
            
            ScrollCalcState state;
            state.current_y = 0;
            state.offsets = self->line_y_offsets;
            state.chars_per_line = chars_per_line;
            state.line_height = self->line_height;
            
            /* O(N) Linear Scan using Piece Table Traversal */
            document_foreach_line(self->doc, calculate_line_height_cb, &state);
            
            /* Backfill any missed lines (e.g. trailing empty line "A\n" counts as 2 lines but callback only runs once) */
            while (self->line_y_offsets->len < total_lines) {
                g_array_append_val(self->line_y_offsets, state.current_y);
                state.current_y += self->line_height;
            }
            
            g_array_append_val(self->line_y_offsets, state.current_y);
            content_height = state.current_y + self->padding_top * 2;
        }
    }
    
    /* Ensure we can scroll enough to show the last line at the bottom? 
       Actually, standard is: upper = content_height. 
       Max scroll = upper - page_size.
       If content_height < widget_height, max scroll = 0.
       If content_height > widget_height, we can scroll until bottom of content aligns with bottom of viewport.
    */

    /* Allow overscroll: let user scroll past the end so last line is at top */
    double overscroll = widget_height * 0.1;
    double upper = MAX(content_height + overscroll, widget_height);

    gtk_adjustment_configure(self->vadjustment,
                             gtk_adjustment_get_value(self->vadjustment),
                             0,
                             upper,
                             self->line_height,      /* step = one line height */
                             widget_height,          /* page = viewport height */
                             widget_height);         /* page_size = viewport height */
}

/* UTF-8 grapheme cluster navigation helpers */

/* Move cursor right by one grapheme cluster */
static size_t
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

static size_t
utf8_next_grapheme(EditorWidget *self, size_t offset)
{
    return editor_widget_get_grapheme_boundary(self, offset, TRUE);
}

static size_t
utf8_prev_grapheme(EditorWidget *self, size_t offset)
{
    return editor_widget_get_grapheme_boundary(self, offset, FALSE);
}

/* Helper: Check if character at offset is a word character */
static gboolean
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
static void
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
static size_t
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
/* Callback for system font changes from GNOME settings */
static void
on_system_font_changed(GSettings *settings, const char *key, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->use_custom_font) {
        /* Force update */
        self->line_height = 0;
        editor_widget_ensure_metrics(self);
        gtk_widget_queue_resize(GTK_WIDGET(self));
    }
}

static void
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
    int ascent = pango_font_metrics_get_ascent(metrics);
    int descent = pango_font_metrics_get_descent(metrics);
    
    /* Calculate line height from metrics ensuring space for all scripts */
    self->line_height = (double)(ascent + descent) / PANGO_SCALE;

    /* Measure '0' for more accurate char width in code/monospace scenarios */
    /* Use logical extents to avoid HiDPI scaling issues with get_pixel_size */
    PangoLayout *layout = pango_layout_new(context);
    pango_layout_set_font_description(layout, self->font_desc);
    pango_layout_set_text(layout, "0", 1);
    
    PangoRectangle logical_rect;
    pango_layout_get_extents(layout, NULL, &logical_rect);
    g_object_unref(layout);
    
    self->cached_char_width = (double)logical_rect.width / PANGO_SCALE;
    if (self->cached_char_width < 1.0) self->cached_char_width = 1.0;
    
    /* Add line spacing (leading) for readability */
    /* 1.15x is compact but comfortable */
    self->line_height = ceil(self->line_height * 1.15);

    pango_font_metrics_unref(metrics);
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

    /* Find start_line using binary search on line_y_offsets */
    size_t start_line = 0;
    double partial_y = 0;
    size_t total_lines = document_get_line_count(self->doc);
    
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
        if (start_line >= total_lines) start_line = total_lines - 1; /* clamp */
        
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
    size_t max_lines = document_get_line_count(self->doc);

    PangoContext *context = gtk_widget_get_pango_context(widget);
    
    /* Lazy fold analysis for visible range + lookahead */
    size_t lookahead = (size_t)(height / self->line_height) + 50;
    ensure_folds_for_range(self, start_line, start_line + lookahead);
    
    /* Rebuild hidden cache if needed */
    if (!self->hidden_cache_valid) {
        rebuild_hidden_cache(self);
    }

    /* Cursor line calc moved to loop */


    double current_y_pos = -partial_y; /* Start with calculated offset */
    double text_start_x = gutter_w + self->padding_left;

    size_t visible_lines_drawn = 0;
    size_t line_idx = start_line;
    while (visible_lines_drawn < count_lines && line_idx < max_lines) {
        /* Skip hidden (folded) lines */
        if (is_line_hidden(self, line_idx)) {
            line_idx++;
            continue;
        }

        size_t len;
        char *text = document_get_line(self->doc, line_idx, &len);

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
        pango_layout_set_text(layout, text, (int)len);
        
        /* Word Wrap - account for gutter and padding */
        if (self->wrap_lines) {
            int available_w = width - text_start_x;
            if (available_w < 50) available_w = 50; /* Safe min width */
            pango_layout_set_width(layout, available_w * PANGO_SCALE);
            pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        } else {
            pango_layout_set_width(layout, -1);
            pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        }
        
        /* Syntax highlight */
        PangoAttrList *attrs = syntax_highlight_line(self->syntax_ctx, line_idx, text);
        pango_layout_set_attributes(layout, attrs);
        pango_attr_list_unref(attrs);
        
        /* Calculate height of this layout */
        int pixel_h;
        pango_layout_get_pixel_size(layout, NULL, &pixel_h);
        double layout_h = (double)pixel_h;
        if (layout_h < self->line_height) layout_h = self->line_height; /* Min height */
        
        /* Apply start offset if first line */
        /* Draw Current Line Highlight - only when no selection */
        /* Draw Current Line Highlight - check all cursors */
        if (self->highlight_current_line) {
             gboolean is_current = FALSE;
             for (guint i = 0; i < self->cursors->len; i++) {
                 EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
                 size_t c_line = document_get_line_of_offset(self->doc, c->pos);
                 if (c_line == line_idx && c->pos == c->anchor) {
                     is_current = TRUE;
                     break;
                 }
             }
             
             if (is_current) {
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
            snprintf(lnum_buf, sizeof(lnum_buf), "%zu", line_idx + 1);
            
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
            /* Translate X=4 for 4px left padding */
            gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(4, current_y_pos + self->padding_top));
            gtk_snapshot_append_layout(snapshot, lnum_layout, &gutter_fg);
            gtk_snapshot_restore(snapshot);
            
            g_object_unref(lnum_layout);
            
            /* Draw Fold Icon if this line is a fold head */
            FoldRegion *fold = get_fold_at_line(self, line_idx);
            if (fold) {
                const char *icon = fold->is_folded ? "▶" : "▼";
                PangoLayout *fold_layout = pango_layout_new(context);
                pango_layout_set_font_description(fold_layout, self->font_desc);
                pango_layout_set_text(fold_layout, icon, -1);
                
                GdkRGBA fold_color = self->color_text;
                fold_color.alpha = 0.6;
                
                gtk_snapshot_save(snapshot);
                /* Position at right edge of gutter (gutter_w - 16 for icon width) */
                gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT((float)(gutter_w - 14), current_y_pos + self->padding_top));
                gtk_snapshot_append_layout(snapshot, fold_layout, &fold_color);
                gtk_snapshot_restore(snapshot);
                
                g_object_unref(fold_layout);
            }
        }

        gtk_snapshot_save(snapshot);
        gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(text_start_x, current_y_pos + self->padding_top));
        
        /* Draw Line Background if selected */
        /* Selection rendering across lines is complex. 
           Simplified: If line is fully selected or partially.
        */
        
        /* Draw Selection */
        size_t line_start_off = document_get_offset_of_line(self->doc, line_idx);
        size_t raw_line_end = line_start_off + len + 1;

        for (guint c_i = 0; c_i < self->cursors->len; c_i++) {
            EditorCursor *c = &g_array_index(self->cursors, EditorCursor, c_i);
            size_t start_sel = MIN(c->pos, c->anchor);
            size_t end_sel = MAX(c->pos, c->anchor);
            
            if (start_sel < raw_line_end && end_sel > line_start_off && start_sel != end_sel) {
                size_t sel_in_line_start = MAX(start_sel, line_start_off) - line_start_off;
                size_t sel_in_line_end = MIN(end_sel, (line_start_off + len)) - line_start_off;
                
                if (len == 0) {
                    gtk_snapshot_append_color(snapshot, 
                                              &(GdkRGBA){0.2, 0.4, 0.8, 0.35},
                                              &GRAPHENE_RECT_INIT(0, 0, (float)width, (float)layout_h));
                } else {
                    PangoLayoutIter *iter = pango_layout_get_iter(layout);
                    do {
                        PangoLayoutLine *p_line = pango_layout_iter_get_line_readonly(iter);
                        int line_start_index = p_line->start_index;
                        int line_end_index = line_start_index + p_line->length;
                        
                        PangoRectangle line_rect;
                        pango_layout_iter_get_line_extents(iter, NULL, &line_rect);
                        double ry = pango_units_to_double(line_rect.y);
                        double rh;
                        
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
                            int *ranges;
                            int n_ranges;
                            int range_start = (int)MAX(sel_in_line_start, (size_t)line_start_index);
                            int range_end = (int)MIN(sel_in_line_end, (size_t)line_end_index);
                            
                            pango_layout_line_get_x_ranges(p_line, range_start, range_end, &ranges, &n_ranges);
                            for (int r = 0; r < n_ranges; r++) {
                                double rx = pango_units_to_double(ranges[2 * r]);
                                double rw = pango_units_to_double(ranges[2 * r + 1] - ranges[2 * r]);
                                if (rw > 0) {
                                    gtk_snapshot_append_color(snapshot, 
                                                              &(GdkRGBA){0.2, 0.4, 0.8, 0.35},
                                                              &GRAPHENE_RECT_INIT((float)rx, (float)ry, (float)rw, (float)rh));
                                }
                            }
                            g_free(ranges);
                            
                            if (end_sel > line_start_off + (size_t)line_end_index) {
                                int x_pos;
                                pango_layout_line_index_to_x(p_line, line_end_index, FALSE, &x_pos);
                                double dx = pango_units_to_double(x_pos);
                                double ex, ew;
                                if (p_line->resolved_dir == PANGO_DIRECTION_RTL) {
                                    ex = 0; ew = dx;
                                } else {
                                    ex = dx; ew = width - dx;
                                }
                                if (ew > 0) {
                                    gtk_snapshot_append_color(snapshot, 
                                                              &(GdkRGBA){0.2, 0.4, 0.8, 0.35},
                                                              &GRAPHENE_RECT_INIT((float)ex, (float)ry, (float)ew, (float)rh));
                                }
                            }
                        }
                    } while (pango_layout_iter_next_line(iter));
                    pango_layout_iter_free(iter);
                }
            }
        }

        gtk_snapshot_append_layout(snapshot, layout, &self->color_text);
        
        /* Draw Cursors */
        for (guint c_i = 0; c_i < self->cursors->len; c_i++) {
            EditorCursor *c = &g_array_index(self->cursors, EditorCursor, c_i);
            size_t c_line = document_get_line_of_offset(self->doc, c->pos);
            gboolean has_selection = (c->pos != c->anchor);
            
            if (line_idx == c_line && self->cursor_alpha > 0.01 && !has_selection && !self->is_dragging_selection) {
                 if (c->pos >= line_start_off) {
                     size_t index_in_line = c->pos - line_start_off;
                     if (index_in_line > len) index_in_line = len;
                     
                     PangoRectangle strong_pos;
                     pango_layout_get_cursor_pos(layout, (int)index_in_line, &strong_pos, NULL);
                     
                     GdkRGBA cursor_color = self->color_cursor;
                     cursor_color.alpha = self->cursor_alpha;
                     
                     int cursor_x = (int)(pango_units_to_double(strong_pos.x) + 0.5);
                     int cursor_y = (int)(pango_units_to_double(strong_pos.y) + 0.5);
                     int cursor_h = (int)self->line_height;
                     
                     gtk_snapshot_append_color(snapshot, 
                                               &cursor_color,
                                               &GRAPHENE_RECT_INIT(cursor_x, cursor_y, 1, cursor_h));
                 }
            }
        }
        
        if (self->is_dragging_selection && self->drag_drop_offset != (size_t)-1) {
            /* Drop caret logic remains similar, usually for singular drop, but could be multi-drop */
            size_t drop_line = document_get_line_of_offset(self->doc, self->drag_drop_offset);
             if (line_idx == drop_line) {
                size_t index_in_line = self->drag_drop_offset - line_start_off;
                if (index_in_line > len) index_in_line = len;

                PangoRectangle strong_pos;
                pango_layout_get_cursor_pos(layout, (int)index_in_line, &strong_pos, NULL);

                GdkRGBA caret_color = self->drag_copy_mode ? (GdkRGBA){0.18, 0.76, 0.49, 1.0} : (GdkRGBA){1.0, 0.647, 0.0, 1.0};
                int caret_x = (int)(self->drag_x - text_start_x);
                if (caret_x < 0) caret_x = 0;
                
                int caret_y = (int)(pango_units_to_double(strong_pos.y) + 0.5);
                int caret_h = (int)(pango_units_to_double(strong_pos.height) + 0.5);
                if (caret_h < 1) caret_h = (int)self->line_height;

                gtk_snapshot_append_color(snapshot, 
                                          &caret_color,
                                          &GRAPHENE_RECT_INIT(caret_x, caret_y, 1, caret_h));
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
        
        line_idx++;
        visible_lines_drawn++;
    }

    /* Draw DnD Overlays */
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

static gboolean
resize_idle_cb(gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    self->idle_resize_id = 0;
    
    /* Pass -1, -1 to use current size */
    editor_widget_update_adjustments(self, -1, -1);
    
    return G_SOURCE_REMOVE;
}

static void
editor_widget_size_allocate(GtkWidget *widget, int width, int height, int baseline)
{
    EditorWidget *self = EDITOR_WIDGET(widget);
    
    // Check if size actually changed to avoid redundant work (though GTK usually handles this)
    if (width == self->cached_width && height == self->cached_height) {
        GTK_WIDGET_CLASS(editor_widget_parent_class)->size_allocate(widget, width, height, baseline);
        return; 
    }
    
    self->cached_width = width;
    self->cached_height = height;

    /* Schedule update on idle if not already scheduled */
    if (self->idle_resize_id == 0) {
        self->idle_resize_id = g_idle_add(resize_idle_cb, self);
    }
    
    GTK_WIDGET_CLASS(editor_widget_parent_class)->size_allocate(widget, width, height, baseline);
}

static void scroll_to_cursor(EditorWidget *self);

static void
editor_widget_get_offset_at_point(EditorWidget *self, double x, double y, size_t *out_offset)
{
    if (!self->doc) return;
    
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
        char *text = document_get_line(self->doc, line_idx, &len);
        
        if (!g_utf8_validate(text, len, NULL)) {
             char *safe = g_utf8_make_valid(text, len);
             g_free(text); text = safe; len = strlen(text);
        }
        while (len > 0 && (text[len-1] == '\n' || text[len-1] == '\r')) {
            len--;
        }
        
        PangoLayout *layout = pango_layout_new(context);
        pango_layout_set_font_description(layout, self->font_desc);
        pango_layout_set_text(layout, text, (int)len);
        
        if (self->wrap_lines) {
            int available_w = width - text_start_x;
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
            double local_x = x - text_start_x;
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

static gint
compare_cursors(gconstpointer a, gconstpointer b)
{
    const EditorCursor *ca = (const EditorCursor *)a;
    const EditorCursor *cb = (const EditorCursor *)b;
    if (ca->pos < cb->pos) return -1;
    if (ca->pos > cb->pos) return 1;
    return 0;
}

static void
editor_widget_sync_anchors(EditorWidget *self)
{
    for (guint i = 0; i < self->cursors->len; i++) {
        EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
        c->anchor = c->pos;
    }
}

/* Mouse wheel scroll handler (works even when scrollbar is hidden) */
static gboolean
on_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    (void)controller;
    (void)dx;
    
    if (!self->vadjustment) return GDK_EVENT_PROPAGATE;
    
    double current = gtk_adjustment_get_value(self->vadjustment);
    double upper = gtk_adjustment_get_upper(self->vadjustment);
    double page = gtk_adjustment_get_page_size(self->vadjustment);
    
    /* Reverted to line-based scrolling as per user request */
    double step = self->line_height * 4; /* Scroll 4 lines per wheel tick */
    
    double new_val = current + (dy * step);
    new_val = CLAMP(new_val, 0, upper - page);
    
    gtk_adjustment_set_value(self->vadjustment, new_val);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    
    return GDK_EVENT_STOP; /* Event handled */
}

static void
on_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    gtk_widget_grab_focus(GTK_WIDGET(self));
    
    if (!self->doc) return;

    /* Check if click is in gutter fold icon area */
    double gutter_w = get_effective_gutter_width(self);
    if (x < gutter_w && x > gutter_w - 16 && n_press == 1) {
        /* Calculate which line was clicked */
        double scroll_y = 0;
        if (self->vadjustment)
            scroll_y = gtk_adjustment_get_value(self->vadjustment);
        
        double click_y = y + scroll_y - self->padding_top;
        size_t line_idx = (size_t)(click_y / self->line_height);
        
        /* Ensure folds are analyzed for this line */
        ensure_folds_for_range(self, line_idx, line_idx + 1);
        
        /* Check if this line has a fold and toggle it */
        FoldRegion *fold = get_fold_at_line(self, line_idx);
        if (fold) {
            toggle_fold_at_line(self, line_idx);
            return;
        }
    }

    size_t off;
    editor_widget_get_offset_at_point(self, x, y, &off);
    
    editor_widget_reset_cursor_blink(self);
    
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    
    /* Handle Alt+Click for Multi-Cursor */
    if (state & GDK_ALT_MASK) {
        editor_widget_add_cursor(self, off);
        /* Don't reset other cursors */
        self->alt_word_mode = FALSE;
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return;
    }
    
    if (n_press == 1 && !(state & GDK_SHIFT_MASK)) {
         /* Standard click - clear others */
         editor_widget_clear_cursors(self);
    }
    
    EditorCursor *primary = get_primary_cursor(self);
    if (!primary) return; /* Should not happen */

    if (n_press == 2) {
        /* Double click - select word */
        size_t total = document_get_length(self->doc);
        gboolean is_newline = FALSE;
        if (off < total) {
            char *ctext = document_get_text_range(self->doc, off, 1);
            if (ctext && ctext[0] == '\n') is_newline = TRUE;
            g_free(ctext);
        }
        
        if (is_newline) {
            size_t sel_start = off;
            while (sel_start > 0) {
                char *c = document_get_text_range(self->doc, sel_start - 1, 1);
                if (c && (c[0] == ' ' || c[0] == '\t')) {
                    sel_start--;
                    g_free(c);
                } else {
                    g_free(c);
                    break;
                }
            }
            
            size_t sel_end = off + 1;
            if (off + 1 < total) {
                size_t next_line_start, next_line_end;
                find_line_at_offset(self->doc, off + 1, &next_line_start, &next_line_end);
                sel_end = next_line_end;
            }
            primary->anchor = sel_end;
            primary->pos = sel_start;
        } else {
            gboolean is_whitespace = FALSE;
            if (off < total) {
                char *ctext = document_get_text_range(self->doc, off, 1);
                if (ctext && (ctext[0] == ' ' || ctext[0] == '\t')) is_whitespace = TRUE;
                g_free(ctext);
            }
            
            if (is_whitespace) {
                size_t ws_start = off;
                size_t ws_end = off;
                while (ws_start > 0) {
                    char *c = document_get_text_range(self->doc, ws_start - 1, 1);
                    if (c && (c[0] == ' ' || c[0] == '\t')) ws_start--;
                    else { g_free(c); break; }
                    g_free(c);
                }
                while (ws_end < total) {
                    char *c = document_get_text_range(self->doc, ws_end, 1);
                    if (c && (c[0] == ' ' || c[0] == '\t')) ws_end++;
                    else { g_free(c); break; }
                    g_free(c);
                }
                primary->anchor = ws_end;
                primary->pos = ws_start;
            } else {
                size_t word_start, word_end;
                editor_widget_find_word_boundary(self, off, &word_start, &word_end);
                primary->anchor = word_end;
                primary->pos = word_start;
            }
        }
        self->alt_word_mode = TRUE;
        self->multi_click_selection = TRUE;
        self->multi_click_mode = 2;
        self->multi_click_start = MIN(primary->anchor, primary->pos);
        self->multi_click_end = MAX(primary->anchor, primary->pos);
    } else if (n_press == 3) {
        size_t line_start, line_end;
        find_line_at_offset(self->doc, off, &line_start, &line_end);
        primary->anchor = line_end;
        primary->pos = line_start;
        self->alt_word_mode = TRUE;
        self->multi_click_selection = TRUE;
        self->multi_click_mode = 3;
        self->multi_click_start = line_start;
        self->multi_click_end = line_end;
    } else {
        self->multi_click_selection = FALSE;
        /* state already retrieved */
        
        size_t sel_start = MIN(primary->pos, primary->anchor);
        size_t sel_end = MAX(primary->pos, primary->anchor);
        gboolean has_selection = (sel_start != sel_end);
        gboolean click_in_selection = has_selection && (off >= sel_start && off < sel_end);
        
        if (state & GDK_SHIFT_MASK) {
             /* Shift+Click extends selection of primary cursor */
            if (click_in_selection) {
                primary->pos = off;
            } else {
                if (off >= sel_end) {
                    primary->anchor = sel_start;
                    primary->pos = off;
                } else if (off < sel_start) {
                    primary->anchor = sel_end;
                    primary->pos = off;
                } else {
                    primary->pos = off;
                }
            }
            self->alt_word_mode = FALSE;
        } else if (click_in_selection) {
            self->is_dragging_selection = TRUE;
            self->drag_start_offset = off;
        } else {
            /* Handled at top (clear_cursors) */
            primary->pos = off;
            primary->anchor = off;
            self->alt_word_mode = FALSE;
        }

        if (gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture)) == 2) {
            editor_widget_paste_primary(self);
        }
    }
    
    update_target_x(self);
    editor_widget_update_im_cursor_location(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    
    /* Claim the sequence to prevent ScrolledWindow from intercepting it for scrolling */
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    
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
    
    EditorCursor *c = get_primary_cursor(self);
    if (!c) return;

    size_t sel_start = MIN(c->pos, c->anchor);
    size_t sel_end = MAX(c->pos, c->anchor);
    
    if (sel_start != sel_end && off >= sel_start && off < sel_end) {
        /* Starting drag on existing selection - prepare for DnD */
        self->is_dragging_selection = TRUE;
        self->drag_start_offset = off;

        /* Create ghost layout for the selected text */
        char *text = document_get_text_range(self->doc, sel_start, sel_end - sel_start);
        if (text) {
            if (self->drag_ghost_layout) g_object_unref(self->drag_ghost_layout);
            self->drag_ghost_layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), text);
            pango_layout_set_font_description(self->drag_ghost_layout, self->font_desc);
            g_free(text);
        }
        self->is_dnd_active = FALSE;
    } else {
        /* Normal click/drag - selection handled by on_click_pressed and on_drag_update */
        self->is_dragging_selection = FALSE;
        self->is_dnd_active = FALSE;
    }
}

/* Autoscroll timer callback for smooth edge scrolling */
static void stop_autoscroll(EditorWidget *self);

static gboolean
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
    
    /* Scale speed based on visual height of current top line to maintain constant visual speed */
    double scale = 1.0;
    size_t top_line = (size_t)(current_val / self->line_height);
    if (top_line < document_get_line_count(self->doc)) {
        char *text = NULL;
        size_t len;
        PangoLayout *layout = create_pango_layout_for_line(self, top_line, &text, &len);
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
        /* Met boundary - stop scrolling to save CPU */
        stop_autoscroll(self);
        return G_SOURCE_REMOVE;
    }
    
    /* Throttle hit-testing to ~15ms (60fps) to avoid freezing on long lines */
    /* Timer fires every 1ms. 15 ticks = 15ms */
    if (self->autoscroll_tick_count % 15 == 0) {
        if (self->is_dnd_active) {
            /* Update drop offset based on new scroll position */
            size_t drop_off;
            editor_widget_get_offset_at_point(self, self->drag_x, self->drag_y, &drop_off);
            
            EditorCursor *c = get_primary_cursor(self);
            if (c) {
                size_t sel_start = MIN(c->pos, c->anchor);
                size_t sel_end = MAX(c->pos, c->anchor);
                
                if (drop_off >= sel_start && drop_off < sel_end) {
                    self->drag_drop_offset = (size_t)-1;
                } else {
                    self->drag_drop_offset = drop_off;
                }
            }
        } else {
             /* Update selection extension based on new scroll position */
             size_t off;
             editor_widget_get_offset_at_point(self, self->drag_x, self->drag_y, &off);
             update_selection_extension(self, off);
        }
    }
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
    
    return G_SOURCE_CONTINUE;
}

static void
stop_autoscroll(EditorWidget *self)
{
    if (self->autoscroll_timer_id) {
        g_source_remove(self->autoscroll_timer_id);
        self->autoscroll_timer_id = 0;
    }
    self->autoscroll_direction = 0;
    self->autoscroll_speed = 0;
}

static void
start_autoscroll(EditorWidget *self, int direction, double speed)
{
    self->autoscroll_direction = direction;
    self->autoscroll_speed = speed;
    self->autoscroll_tick_count = 0;
    
    if (!self->autoscroll_timer_id) {
        /* Timer fires every 16ms (approx 60fps) */
        self->autoscroll_timer_id = g_timeout_add(16, autoscroll_tick, self);
    }
}

static void
update_selection_extension(EditorWidget *self, size_t off)
{
    EditorCursor *c = get_primary_cursor(self);
    if (!c) return;

    if (self->multi_click_selection) {
        /* Multi-click drag - extend selection while keeping original word/line as minimum */
        size_t current_start = off;
        size_t current_end = off;
        
        if (self->multi_click_mode == 2) {
            /* Word mode */
            editor_widget_find_word_boundary(self, off, &current_start, &current_end);
        } else if (self->multi_click_mode == 3) {
            /* Line mode */
            find_line_at_offset(self->doc, off, &current_start, &current_end);
        }
        
        if (off < self->multi_click_start) {
            /* Dragging before the original selection - extend backwards */
            c->anchor = self->multi_click_end;
            c->pos = current_start;
        } else if (off > self->multi_click_end) {
            /* Dragging after the original selection - extend forwards */
            c->anchor = self->multi_click_start;
            c->pos = current_end;
        } else {
            /* Still within original selection - keep original bounds */
            c->anchor = self->multi_click_start;
            c->pos = self->multi_click_end;
        }
        self->alt_word_mode = TRUE; /* Dragging a multi-click selection preserves word-like behavior */
    } else {
        /* Normal drag selection - extend selection to current position */
        /* Only update cursor, anchor was set by on_click_pressed */
        c->pos = off;
        self->alt_word_mode = FALSE; /* Manual drag selection is character-based */
    }
}

static void
on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->doc) return;
    
    double start_x, start_y;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
    
    self->drag_x = start_x + offset_x;
    self->drag_y = start_y + offset_y;

    size_t off;
    editor_widget_get_offset_at_point(self, self->drag_x, self->drag_y, &off);
    
    gboolean is_dnd_mode = (self->is_dragging_selection && !self->multi_click_selection);
    
    if (is_dnd_mode) {
        /* Dragging selection for DnD - visual feedback handled in snapshot */
        gboolean has_movement = (fabs(offset_x) > 8 || fabs(offset_y) > 8);
        
        if (has_movement) {
            self->is_dnd_active = TRUE;
            
            /* Detect copy mode (Ctrl held) */
            GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
            self->drag_copy_mode = (state & GDK_CONTROL_MASK) != 0;

            /* Calculate drop insertion point */
            /* Calculate drop insertion point */
            size_t drop_off;
            editor_widget_get_offset_at_point(self, self->drag_x, self->drag_y, &drop_off);
            
            EditorCursor *c = get_primary_cursor(self);
            if (!c) {
                 self->drag_drop_offset = (size_t)-1;
            } else {
                size_t sel_start = MIN(c->pos, c->anchor);
                size_t sel_end = MAX(c->pos, c->anchor);

                /* Rule: Never show caret inside or overlap selected range */
                if (drop_off >= sel_start && drop_off < sel_end) {
                    self->drag_drop_offset = (size_t)-1; /* Suppress caret */
                } else {
                    self->drag_drop_offset = drop_off;
                }
            }
        } else {
            /* Not moved enough yet - suppress feedback */
            self->drag_drop_offset = (size_t)-1;
            self->is_dnd_active = FALSE;
        }
    } else {
        /* Standard or Multi-Click Selection Extension */
        update_selection_extension(self, off);
        self->is_dnd_active = FALSE;
        self->drag_drop_offset = (size_t)-1;
    }
    
    /* Unified Autoscroll Logic */
    gboolean allow_autoscroll = TRUE;
    if (is_dnd_mode && !self->is_dnd_active) allow_autoscroll = FALSE;
    
    if (allow_autoscroll) {
        int widget_height = gtk_widget_get_height(GTK_WIDGET(self));
        int edge_zone = 100; /* Pixels from edge to trigger scroll */
        
        if (self->drag_y < edge_zone) {
            /* Top edge */
            double proximity = (edge_zone - self->drag_y) / (double)edge_zone;
            double speed_factor = 0.001 + proximity * proximity * 0.25; 
            double speed = speed_factor * self->line_height;
            start_autoscroll(self, -1, speed);
        } else if (self->drag_y > widget_height - edge_zone) {
            /* Bottom edge */
            double distance_from_edge = widget_height - self->drag_y;
            double proximity = (edge_zone - distance_from_edge) / (double)edge_zone;
            double speed_factor = 0.001 + proximity * proximity * 0.25;
            double speed = speed_factor * self->line_height;
            start_autoscroll(self, 1, speed);
        } else {
            stop_autoscroll(self);
        }
    } else {
        stop_autoscroll(self);
    }
    
    /* Disable manual scroll_to_cursor during drag to rely on smooth autoscroll */
    /* and avoid fighting/jumping */

    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
editor_widget_drag_drop_finish(EditorWidget *self, size_t drop_off)
{
    if (drop_off == (size_t)-1 || !self->doc) return;

    EditorCursor *c = get_primary_cursor(self);
    if (!c) return;

    size_t sel_start = MIN(c->pos, c->anchor);
    size_t sel_end = MAX(c->pos, c->anchor);
    size_t sel_len = sel_end - sel_start;
    
    char *text = document_get_text_range(self->doc, sel_start, sel_len);
    if (!text) return;
    
    document_begin_undo_group(self->doc);
    
    if (!self->drag_copy_mode) {
        document_delete(self->doc, sel_start, sel_len);
        if (drop_off > sel_end) {
            drop_off -= sel_len;
        }
    }
    
    document_insert(self->doc, drop_off, text, sel_len);
    
    /* Select dropped text */
    c->anchor = drop_off;
    c->pos = drop_off + sel_len;
    self->alt_word_mode = FALSE;
    
    document_end_undo_group(self->doc);
    
    g_free(text);
    
    editor_widget_update_adjustments(self, -1, -1);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
on_drag_end(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->doc) return;
    
    /* Always stop autoscroll when drag ends */
    stop_autoscroll(self);
    
    /* If this was a multi-click selection (double/triple-click), 
       just clear the flag and preserve the selection */
    if (self->multi_click_selection) {
        self->multi_click_selection = FALSE;
        self->is_dragging_selection = FALSE;
        return;
    }
    
    if (self->is_dragging_selection) {
        self->is_dragging_selection = FALSE;

        if (self->is_dnd_active && self->drag_drop_offset != (size_t)-1) {
            /* Perform move/copy */
            editor_widget_drag_drop_finish(self, self->drag_drop_offset);
        } else if (!self->is_dnd_active) {
            /* Clicked inside selection without dragging -> Clear selection */
            EditorCursor *c = get_primary_cursor(self);
            if (c) {
                c->pos = self->drag_start_offset;
                c->anchor = self->drag_start_offset;
            }
            self->alt_word_mode = FALSE;
        }
        self->is_dnd_active = FALSE;
        if (self->drag_ghost_layout) {
            g_object_unref(self->drag_ghost_layout);
            self->drag_ghost_layout = NULL;
        }
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
    
    stop_autoscroll(self);
    editor_widget_update_adjustments(self, -1, -1);
}

static size_t
editor_widget_delete_selection(EditorWidget *self)
{
    g_array_sort(self->cursors, compare_cursors);
    size_t total_len = 0;
    long accumulated_shift = 0;
    
    for (guint i = 0; i < self->cursors->len; i++) {
        EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
        
        /* Apply shift from previous deletions */
        /* Note: casting logic requires care with signed/unsigned */
        /* Assuming cursors are within reasonable bounds */
        if (accumulated_shift < 0) {
            size_t shift = (size_t)(-accumulated_shift);
            if (c->pos >= shift) c->pos -= shift; else c->pos = 0;
            if (c->anchor >= shift) c->anchor -= shift; else c->anchor = 0;
        } else {
            c->pos += accumulated_shift;
            c->anchor += accumulated_shift;
        }
        
        if (c->pos == c->anchor) continue;
        
        size_t start = MIN(c->pos, c->anchor);
        size_t end = MAX(c->pos, c->anchor);
        size_t len = end - start;
        
        document_delete(self->doc, start, len);
        c->pos = start;
        c->anchor = start;
        total_len += len;
        accumulated_shift -= len;
    }
    return total_len;
}

static void
editor_widget_copy(EditorWidget *self)
{
    GString *str = g_string_new("");
    g_array_sort(self->cursors, compare_cursors);
    
    gboolean first = TRUE;
    for (guint i = 0; i < self->cursors->len; i++) {
        EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
        size_t start = MIN(c->pos, c->anchor);
        size_t end = MAX(c->pos, c->anchor);
        if (start != end) {
             char *t = document_get_text_range(self->doc, start, end - start);
             if (!first) g_string_append_c(str, '\n');
             g_string_append(str, t);
             g_free(t);
             first = FALSE;
        }
    }
    
    if (str->len > 0) {
        GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
        gdk_clipboard_set_text(clipboard, str->str);
    }
    g_string_free(str, TRUE);
}

static void
editor_widget_cut(EditorWidget *self)
{
    if (!self->doc) return;
    editor_widget_copy(self);
    if (editor_widget_delete_selection(self)) {
        editor_widget_reset_cursor_blink(self);
        gtk_widget_queue_draw(GTK_WIDGET(self));
        editor_widget_update_adjustments(self, -1, -1);
    }
}

static void
on_paste_text_received(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
    char *text = gdk_clipboard_read_text_finish(clipboard, res, NULL);
    if (!text) return;
    
    size_t len = strlen(text);
    if (len > 0) {
        document_begin_undo_group(self->doc);
        editor_widget_delete_selection(self);
        
        /* Insert at all cursors (Reverse order) */
        g_array_sort(self->cursors, compare_cursors);
        for (int i = (int)self->cursors->len - 1; i >= 0; i--) {
             EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
             document_insert(self->doc, c->pos, text, len);
             c->pos += len;
             c->anchor = c->pos;
        }
        
        document_end_undo_group(self->doc);
        
        editor_widget_reset_cursor_blink(self);
        editor_widget_update_adjustments(self, -1, -1);
        scroll_to_cursor(self);
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
    g_free(text);
}

static void
editor_widget_paste(EditorWidget *self)
{
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
    gdk_clipboard_read_text_async(clipboard, NULL, on_paste_text_received, self);
}

static void
on_primary_paste_received(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
    char *text = gdk_clipboard_read_text_finish(clipboard, res, NULL);
    
    if (text) {
        size_t len = strlen(text);
        if (len > 0) {
            document_begin_undo_group(self->doc);
            
            g_array_sort(self->cursors, compare_cursors);
            for (int i = (int)self->cursors->len - 1; i >= 0; i--) {
                 EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
                 document_insert(self->doc, c->pos, text, len);
                 c->pos += len;
                 c->anchor = c->pos;
            }
            
            document_end_undo_group(self->doc);
            
            editor_widget_reset_cursor_blink(self);
            editor_widget_update_adjustments(self, -1, -1);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
        }
        g_free(text);
    }
}

static void
editor_widget_paste_primary(EditorWidget *self)
{
    GdkClipboard *clipboard = gdk_display_get_primary_clipboard(gtk_widget_get_display(GTK_WIDGET(self)));
    gdk_clipboard_read_text_async(clipboard, NULL, on_primary_paste_received, self);
}



static void
scroll_to_cursor(EditorWidget *self)
{
    if (!self->vadjustment || !self->doc) return;
    
    EditorCursor *c = get_primary_cursor(self);
    if (!c) return;

    size_t cursor_line = document_get_line_of_offset(self->doc, c->pos);
    double cursor_line_y;

    /* Get Y of cursor line */
    if (self->line_y_offsets && self->line_y_offsets->len > cursor_line) {
        double *offsets = (double*)self->line_y_offsets->data;
        cursor_line_y = offsets[cursor_line];
    } else {
        /* Fallback: use average visual line height */
        double multiplier = (self->wrap_lines) ? self->avg_visual_lines : 1.0;
        if (multiplier < 1.0) multiplier = 1.0;
        cursor_line_y = (double)cursor_line * multiplier * self->line_height;
    }
    
    /* Calculate precise local Y of cursor within the line */
    char *text; size_t len;
    PangoLayout *layout = create_pango_layout_for_line(self, cursor_line, &text, &len);
    if (!layout) return;
    
    size_t line_start = document_get_offset_of_line(self->doc, cursor_line);
    size_t idx = c->pos - line_start;
    PangoRectangle pos;
    pango_layout_get_cursor_pos(layout, MIN((int)idx, (int)len), &pos, NULL);
    
    double local_y = pango_units_to_double(pos.y);
    double cursor_h = pango_units_to_double(pos.height);
    if (cursor_h < 1) cursor_h = self->line_height;
    
    g_object_unref(layout); g_free(text);
    
    /* Absolute Y in document space */
    double abs_top = cursor_line_y + local_y;
    double abs_bottom = abs_top + cursor_h;
    
    double scroll_y = gtk_adjustment_get_value(self->vadjustment);
    double page = gtk_adjustment_get_page_size(self->vadjustment);
    
    double top_margin = self->padding_top; 
    double bottom_margin = page - self->padding_top; 
    
    /* Calculate visible viewport Y range */
    double viewport_top = scroll_y + top_margin;
    double viewport_bottom = scroll_y + bottom_margin;
    
    if (abs_top < viewport_top) {
        /* Scroll UP to make abs_top visible */
        gtk_adjustment_set_value(self->vadjustment, abs_top - top_margin);
    } else if (abs_bottom > viewport_bottom) {
        /* Scroll DOWN to make abs_bottom visible */
        gtk_adjustment_set_value(self->vadjustment, abs_bottom - bottom_margin);
    }
    
    editor_widget_update_im_cursor_location(self);
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

    /* Pango doesn't want the trailing newline */
    while (len > 0 && (text[len-1] == '\n' || text[len-1] == '\r')) {
        len--;
    }

    PangoContext *context = gtk_widget_get_pango_context(GTK_WIDGET(self));
    PangoLayout *layout = pango_layout_new(context);
    pango_layout_set_font_description(layout, self->font_desc);
    pango_layout_set_text(layout, text, (int)len);
    
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

static void
update_target_x(EditorWidget *self)
{
    for (guint i = 0; i < self->cursors->len; i++) {
        EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
        size_t line_idx = document_get_line_of_offset(self->doc, c->pos);
        size_t line_start = document_get_offset_of_line(self->doc, line_idx);
        size_t index_in_line = c->pos - line_start;
        
        char *text;
        size_t len;
        PangoLayout *layout = create_pango_layout_for_line(self, line_idx, &text, &len);
        if (!layout) continue;
        
        if (index_in_line > len) index_in_line = len;
        
        PangoRectangle strong_pos;
        pango_layout_get_cursor_pos(layout, (int)index_in_line, &strong_pos, NULL);
        c->target_x = pango_units_to_double(strong_pos.x);
        
        g_object_unref(layout);
        g_free(text);
    }
}

static void
move_cursor(EditorWidget *self, int visual_lines_delta)
{
    if (visual_lines_delta == 0) return;

    for (guint c_i = 0; c_i < self->cursors->len; c_i++) {
        EditorCursor *c = &g_array_index(self->cursors, EditorCursor, c_i);
        size_t line_idx = document_get_line_of_offset(self->doc, c->pos);
        size_t line_start = document_get_offset_of_line(self->doc, line_idx);
        size_t char_idx = c->pos - line_start;

        char *text = NULL;
        size_t len;
        PangoLayout *layout = create_pango_layout_for_line(self, line_idx, &text, &len);
        if (!layout) continue;

        /* If target_x isn't set, set it from current position */
        if (c->target_x < 0) {
            PangoRectangle strong_pos;
            pango_layout_get_cursor_pos(layout, (int)MIN(char_idx, len), &strong_pos, NULL);
            c->target_x = pango_units_to_double(strong_pos.x);
        }

        /* Find current visual line index based on cursor Y position */
        PangoRectangle cursor_pos;
        pango_layout_get_cursor_pos(layout, (int)MIN(char_idx, len), &cursor_pos, NULL);
        
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
        
        if (!found_v_line) {
            current_v_line_idx = MAX(0, pango_layout_get_line_count(layout) - 1);
        }

        /* Temp delta for this cursor */
        int delta = visual_lines_delta;

        /* Loop to consume delta across logical lines */
        while (TRUE) {
            int total_v_lines = pango_layout_get_line_count(layout);
            int target_v_line = current_v_line_idx + delta;

            if (target_v_line >= 0 && target_v_line < total_v_lines) {
                /* Target reached in this line */
                iter = pango_layout_get_iter(layout);
                for (int i = 0; i < target_v_line; i++) pango_layout_iter_next_line(iter);
                PangoLayoutLine *v_line = pango_layout_iter_get_line_readonly(iter);
                
                int index, trailing;
                pango_layout_line_x_to_index(v_line, (int)(c->target_x * PANGO_SCALE), &index, &trailing);
                line_start = document_get_offset_of_line(self->doc, line_idx);
                c->pos = line_start + index + trailing;
                pango_layout_iter_free(iter);
                
                delta = 0; /* Done */
                break;
            } else {
                /* Target is outside current logical line */
                if (delta > 0) {
                    /* Moving down */
                    int lines_remaining = total_v_lines - current_v_line_idx - 1;
                    delta -= (lines_remaining + 1); /* +1 to jump to next line */
                    
                    size_t next_line = line_idx + 1;
                    if (next_line >= document_get_line_count(self->doc)) {
                        /* End of doc - clamp to last visual line */
                        delta = 0;
                        /* Apply clamp to end of current line */
                        iter = pango_layout_get_iter(layout);
                        int last_v = total_v_lines - 1;
                        for (int i = 0; i < last_v; i++) pango_layout_iter_next_line(iter);
                        PangoLayoutLine *v_line = pango_layout_iter_get_line_readonly(iter);
                        int index, trailing;
                        pango_layout_line_x_to_index(v_line, (int)(c->target_x * PANGO_SCALE), &index, &trailing);
                        line_start = document_get_offset_of_line(self->doc, line_idx);
                        c->pos = line_start + index + trailing;
                        pango_layout_iter_free(iter);
                        break;
                    }
                    
                    /* Advance to next logical line */
                    line_idx = next_line;
                    g_object_unref(layout);
                    g_free(text);
                    layout = create_pango_layout_for_line(self, line_idx, &text, &len);
                    current_v_line_idx = 0; /* Start at top of next line */
                } else {
                    /* Moving up */
                    int lines_above = current_v_line_idx;
                    delta += (lines_above + 1); /* +1 to jump to prev line */
                    
                    if (line_idx == 0) {
                        /* Top of doc - clamp to first visual line */
                        delta = 0;
                        /* Apply clamp to start of current line */
                        iter = pango_layout_get_iter(layout);
                        /* iter is already at 0 */
                        PangoLayoutLine *v_line = pango_layout_iter_get_line_readonly(iter);
                        int index, trailing;
                        pango_layout_line_x_to_index(v_line, (int)(c->target_x * PANGO_SCALE), &index, &trailing);
                        line_start = document_get_offset_of_line(self->doc, line_idx);
                        c->pos = line_start + index + trailing;
                        pango_layout_iter_free(iter);
                        break;
                    }
                    
                    /* Retreat to prev logical line */
                    line_idx = line_idx - 1;
                    g_object_unref(layout);
                    g_free(text);
                    layout = create_pango_layout_for_line(self, line_idx, &text, &len);
                    /* Start at bottom of prev line */
                    current_v_line_idx = pango_layout_get_line_count(layout) - 1; 
                }
            }
        }

        g_object_unref(layout);
        g_free(text);
    }
    editor_widget_update_im_cursor_location(self);
}

static gboolean
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

static void
editor_widget_move_selection_horizontally(EditorWidget *self, int delta)
{
    EditorCursor *c = get_primary_cursor(self);
    if (!c) return;
    
    size_t total = document_get_length(self->doc);
    size_t s = MIN(c->pos, c->anchor);
    size_t e = MAX(c->pos, c->anchor);
    
    /* If no selection, auto-select word at caret precisely as in swap_words */
    if (s == e) {
        self->alt_word_mode = TRUE;
        size_t off = c->pos;
        if (off > 0 && !is_alt_word_char_at(self->doc, off) && is_alt_word_char_at(self->doc, utf8_prev_grapheme(self, off))) {
            off = utf8_prev_grapheme(self, off);
        }
        if (!is_alt_word_char_at(self->doc, off)) {
            if (delta > 0) {
                while (off < total && !is_alt_word_char_at(self->doc, off)) off = utf8_next_grapheme(self, off);
            } else {
                while (off > 0 && !is_alt_word_char_at(self->doc, off)) off = utf8_prev_grapheme(self, off);
            }
        }
        if (off >= total || !is_alt_word_char_at(self->doc, off)) return;
        
        s = off;
        while (s > 0 && is_alt_word_char_at(self->doc, utf8_prev_grapheme(self, s)))
            s = utf8_prev_grapheme(self, s);
        e = off;
        while (e < total && is_alt_word_char_at(self->doc, e))
            e = utf8_next_grapheme(self, e);
    } else if (!self->alt_word_mode) {
        /* Manual selection: move by exactly one character (shift) */
        char *sel_text = document_get_text_range(self->doc, s, e - s);
        if (delta > 0) {
            if (e < total) {
                size_t next_gap = utf8_next_grapheme(self, e);
                size_t gap_len = next_gap - e;
                char *gap_text = document_get_text_range(self->doc, e, gap_len);
                document_delete(self->doc, s, (e - s) + gap_len);
                document_insert(self->doc, s, gap_text, gap_len);
                document_insert(self->doc, s + gap_len, sel_text, e - s);
                c->anchor = s + gap_len;
                c->pos = c->anchor + (e - s);
                g_free(gap_text);
            }
        } else {
            if (s > 0) {
                size_t prev_gap = utf8_prev_grapheme(self, s);
                size_t gap_len = s - prev_gap;
                char *gap_text = document_get_text_range(self->doc, prev_gap, gap_len);
                document_delete(self->doc, prev_gap, gap_len + (e - s));
                document_insert(self->doc, prev_gap, sel_text, e - s);
                document_insert(self->doc, prev_gap + (e - s), gap_text, gap_len);
                c->anchor = prev_gap;
                c->pos = c->anchor + (e - s);
                g_free(gap_text);
            }
        }
        g_free(sel_text);
        update_target_x(self);
        return;
    }
    
    size_t sel_len = e - s;
    if (delta > 0) {
        /* Move Right: [SEL][SEP][W2] -> [W2][SEP][SEL] */
        size_t sep_start = e;
        size_t sep_end = sep_start;
        while (sep_end < total && !is_alt_word_char_at(self->doc, sep_end))
            sep_end = utf8_next_grapheme(self, sep_end);
            
        if (sep_end >= total) return;
        
        size_t w2_start = sep_end;
        size_t w2_end = w2_start;
        while (w2_end < total && is_alt_word_char_at(self->doc, w2_end))
            w2_end = utf8_next_grapheme(self, w2_end);
            
        size_t w2_len = w2_end - w2_start;
        size_t sep_len = w2_start - e;
        
        char *sel_text = document_get_text_range(self->doc, s, sel_len);
        char *sep_text = document_get_text_range(self->doc, e, sep_len);
        char *w2_text = document_get_text_range(self->doc, w2_start, w2_len);
        
        document_delete(self->doc, s, sel_len + sep_len + w2_len);
        document_insert(self->doc, s, w2_text, w2_len);
        document_insert(self->doc, s + w2_len, sep_text, sep_len);
        document_insert(self->doc, s + w2_len + sep_len, sel_text, sel_len);
        
        c->anchor = s + w2_len + sep_len;
        c->pos = c->anchor + sel_len;
        
        g_free(sel_text); g_free(sep_text); g_free(w2_text);
    } else {
        /* Move Left: [W2][SEP][SEL] -> [SEL][SEP][W2] */
        size_t sep_end = s;
        size_t sep_start = sep_end;
        while (sep_start > 0 && !is_alt_word_char_at(self->doc, utf8_prev_grapheme(self, sep_start)))
            sep_start = utf8_prev_grapheme(self, sep_start);
            
        if (sep_start == 0 && !is_alt_word_char_at(self->doc, 0)) return;
        
        size_t w2_end = sep_start;
        size_t w2_start = w2_end;
        while (w2_start > 0 && is_alt_word_char_at(self->doc, utf8_prev_grapheme(self, w2_start)))
            w2_start = utf8_prev_grapheme(self, w2_start);
            
        if (w2_start == w2_end) return;
        
        size_t w2_len = w2_end - w2_start;
        size_t sep_len = s - w2_end;
        
        char *w2_text = document_get_text_range(self->doc, w2_start, w2_len);
        char *sep_text = document_get_text_range(self->doc, w2_end, sep_len);
        char *sel_text = document_get_text_range(self->doc, s, sel_len);
        
        document_delete(self->doc, w2_start, w2_len + sep_len + sel_len);
        document_insert(self->doc, w2_start, sel_text, sel_len);
        document_insert(self->doc, w2_start + sel_len, sep_text, sep_len);
        document_insert(self->doc, w2_start + sel_len + sep_len, w2_text, w2_len);
        
        c->anchor = w2_start;
        c->pos = w2_start + sel_len;
        
        g_free(sel_text); g_free(sep_text); g_free(w2_text);
    }
    update_target_x(self);
}

static void
editor_widget_move_lines_vertically(EditorWidget *self, int delta)
{
    EditorCursor *c = get_primary_cursor(self);
    if (!c) return;
    
    size_t start_off = MIN(c->pos, c->anchor);
    size_t end_off = MAX(c->pos, c->anchor);
    
    size_t start_line = document_get_line_of_offset(self->doc, start_off);
    size_t end_line = document_get_line_of_offset(self->doc, end_off);
    
    /* If selection ends exactly at start of next line, don't include that line */
    if (end_off > start_off && end_off == document_get_offset_of_line(self->doc, end_line) && end_line > start_line) {
        end_line--;
    }
    
    size_t total_lines = document_get_line_count(self->doc);
    document_begin_undo_group(self->doc);
    
    if (delta < 0 && start_line > 0) {
        /* Move Up: Swap [prev_line] with [range] */
        size_t prev_line = start_line - 1;
        size_t prev_start = document_get_offset_of_line(self->doc, prev_line);
        size_t range_start = document_get_offset_of_line(self->doc, start_line);
        size_t range_end = (end_line + 1 < total_lines) ? document_get_offset_of_line(self->doc, end_line + 1) : document_get_length(self->doc);
        
        size_t prev_len = range_start - prev_start;
        size_t range_len = range_end - range_start;
        
        char *prev_text = document_get_text_range(self->doc, prev_start, prev_len);
        char *range_text = document_get_text_range(self->doc, range_start, range_len);
        
        /* If range is at EOF and has no newline, but will be moved up, it MUST have a newline */
        gboolean range_needs_newline = (range_len > 0 && range_text[range_len-1] != '\n');
        
        document_delete(self->doc, prev_start, prev_len + range_len);
        
        size_t current_pos = prev_start;
        document_insert(self->doc, current_pos, range_text, range_len);
        current_pos += range_len;
        if (range_needs_newline) {
            document_insert(self->doc, current_pos, "\n", 1);
            current_pos += 1;
        }
        document_insert(self->doc, current_pos, prev_text, prev_len);
        
        c->anchor = prev_start;
        c->pos = prev_start + range_len + (range_needs_newline ? 1 : 0);
        
        g_free(prev_text);
        g_free(range_text);
    } else if (delta > 0 && end_line + 1 < total_lines) {
        /* Move Down: Swap [range] with [next_line] */
        size_t next_line = end_line + 1;
        size_t range_start = document_get_offset_of_line(self->doc, start_line);
        size_t range_end = document_get_offset_of_line(self->doc, next_line);
        size_t next_end = (next_line + 1 < total_lines) ? document_get_offset_of_line(self->doc, next_line + 1) : document_get_length(self->doc);
        
        size_t range_len = range_end - range_start;
        size_t next_len = next_end - range_end;
        
        char *range_text = document_get_text_range(self->doc, range_start, range_len);
        char *next_text = document_get_text_range(self->doc, range_end, next_len);
        
        /* If next_line is at EOF and has no newline, but will be moved up, it MUST have a newline */
        gboolean next_needs_newline = (next_len > 0 && next_text[next_len-1] != '\n');

        document_delete(self->doc, range_start, range_len + next_len);
        
        size_t current_pos = range_start;
        document_insert(self->doc, current_pos, next_text, next_len);
        current_pos += next_len;
        if (next_needs_newline) {
            document_insert(self->doc, current_pos, "\n", 1);
            current_pos += 1;
        }
        document_insert(self->doc, current_pos, range_text, range_len);
        
        c->anchor = range_start + next_len + (next_needs_newline ? 1 : 0);
        c->pos = c->anchor + range_len;
        
        g_free(range_text);
        g_free(next_text);
    }
    
    document_end_undo_group(self->doc);
    update_target_x(self);
}

static void
editor_widget_add_cursor_vertically(EditorWidget *self, int dir)
{
    if (!self->cursors || self->cursors->len == 0) return;
    
    /* Find the extremum cursor (Top-most for Up, Bottom-most for Down) */
    EditorCursor *ref_c = NULL;
    size_t ref_line = 0;
    
    for (guint i = 0; i < self->cursors->len; i++) {
        EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
        size_t line = document_get_line_of_offset(self->doc, c->pos);
        
        if (ref_c == NULL) {
            ref_c = c;
            ref_line = line;
        } else {
            if (dir < 0) {
                /* Moving Up: find min line */
                if (line < ref_line) {
                    ref_c = c;
                    ref_line = line;
                }
            } else {
                /* Moving Down: find max line */
                if (line > ref_line) {
                    ref_c = c;
                    ref_line = line;
                }
            }
        }
    }
    
    if (!ref_c) return;

    size_t new_line_idx = ref_line + dir;
    if (new_line_idx >= document_get_line_count(self->doc)) return;

    /* Determine target_x from ref cursor */
    double target_x = ref_c->target_x;
    if (target_x < 0) {
        char *text; size_t len;
        PangoLayout *layout = create_pango_layout_for_line(self, ref_line, &text, &len);
        if (layout) {
            size_t line_start = document_get_offset_of_line(self->doc, ref_line);
            size_t idx = ref_c->pos - line_start;
            PangoRectangle pos;
            pango_layout_get_cursor_pos(layout, MIN((int)idx, (int)len), &pos, NULL);
            target_x = pango_units_to_double(pos.x);
            ref_c->target_x = target_x; /* Cache it */
            g_object_unref(layout); g_free(text);
        }
    }
    
    /* Find pos in new line */
    char *text; size_t len;
    PangoLayout *layout = create_pango_layout_for_line(self, new_line_idx, &text, &len);
    if (!layout) return;
    
    int index, trailing;
    pango_layout_xy_to_index(layout, (int)(target_x * PANGO_SCALE), 0, &index, &trailing); 
    
    size_t new_line_start = document_get_offset_of_line(self->doc, new_line_idx);
    size_t new_pos = new_line_start + index;
    
    g_object_unref(layout); g_free(text);
    
    editor_widget_add_cursor(self, new_pos);
    
    /* Set target_x for new cursor */
    EditorCursor *new_c = get_primary_cursor(self);
    new_c->target_x = target_x;
}




static gboolean
get_cursor_screen_coordinates(EditorWidget *self, double *out_x, double *out_y)
{
    if (!self->doc || !self->vadjustment) return FALSE;
    
    EditorCursor *c = get_primary_cursor(self);
    if (!c) return FALSE;

    size_t cursor_line = document_get_line_of_offset(self->doc, c->pos);
    double cursor_line_y;

    if (self->line_y_offsets && self->line_y_offsets->len > cursor_line) {
        cursor_line_y = g_array_index(self->line_y_offsets, double, cursor_line);
    } else {
        double m = (self->wrap_lines) ? self->avg_visual_lines : 1.0;
        if (m < 1.0) m = 1.0;
        cursor_line_y = (double)cursor_line * m * self->line_height;
    }
    
    char *text; size_t len;
    PangoLayout *layout = create_pango_layout_for_line(self, cursor_line, &text, &len);
    if (!layout) return FALSE;
    
    size_t line_start = document_get_offset_of_line(self->doc, cursor_line);
    size_t idx = c->pos - line_start;
    PangoRectangle pos;
    pango_layout_get_cursor_pos(layout, MIN((int)idx, (int)len), &pos, NULL);
    
    double local_y = pango_units_to_double(pos.y);
    double local_x = pango_units_to_double(pos.x);
    
    g_object_unref(layout); g_free(text);
    
    double scroll_y = gtk_adjustment_get_value(self->vadjustment);
    
    if (out_x) *out_x = local_x + self->padding_left;
    if (out_y) *out_y = (cursor_line_y + local_y) - scroll_y + self->padding_top;
    
    return TRUE;
}

/* Indentation helpers */
static void
editor_widget_indent_selection(EditorWidget *self)
{
    if (!self->doc) return;

    /* Indent for all cursors. Sort to avoid index shift issues? 
       Insertion shifts offsets. So we should process from end to start?
       No, indentation adds characters.
       If we process cursor A (10) and add 2 chars. Cursor B (20) needs to be 22.
       But if we just iterate and use 'insert', the document logic handles content shift?
       No, the cursor positions need to be updated.
       
       For block indentation, we usually indent lines.
       If multiple cursors are on the same line, we process that line once.
    */
    
    document_begin_undo_group(self->doc);
    g_array_sort(self->cursors, compare_cursors); 
    
    /* Use a set or flags to avoid indenting same line twice? */
    /* Or just iterate line by line? */
    /* Simplest: iterate cursors, check if line already processed. */
    
    /* But 'indent_selection' logic (lines 2477) was checking selection range. 
       If we have multiple selections, we handle each range.
    */
    
    /* We need to be careful about overlapping selections. 
       Simplification: Just loop cursors and indent their ranges.
       Process reverse order to avoid invalidating later cursor offsets.
    */
    
    /* Reverse iteration */
    for (int i = (int)self->cursors->len - 1; i >= 0; i--) {
        EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
        size_t start = MIN(c->pos, c->anchor);
        size_t end = MAX(c->pos, c->anchor);
        
        size_t start_line = document_get_line_of_offset(self->doc, start);
        size_t end_line = document_get_line_of_offset(self->doc, end);
        
        if (end > start && end == document_get_offset_of_line(self->doc, end_line)) {
            if (end_line > start_line) end_line--;
        }
        
        /* Indent Str */
        char *indent_str;
        size_t indent_len;
        if (self->indent_style == 0) {
            indent_len = self->indent_width;
            indent_str = g_malloc(indent_len + 1);
            memset(indent_str, ' ', indent_len);
            indent_str[indent_len] = '\0';
        } else {
            indent_len = 1;
            indent_str = g_strdup("\t");
        }
        
        /* Insert and update THIS cursor. 
           But this insertion shifts cursors occurring AFTER this block (which we already processed? No).
           We are processing reversed. Cursors with HIGHER offsets are already done?
           Actually, if we work backwards (High to Low):
           - Processing cursor at 100. Insert at 100.
           - Cursor at 10 is unaffected.
           So Reverse Order is correct for insertions.
        */
        
        size_t bytes_added = 0;
        for (size_t l = start_line; l <= end_line; l++) {
            size_t line_start = document_get_offset_of_line(self->doc, l);
            document_insert(self->doc, line_start, indent_str, indent_len);
            bytes_added += indent_len;
        }
        
        /* Update self */
        size_t new_start = document_get_offset_of_line(self->doc, start_line);
        size_t new_end = end + bytes_added;
        
        if (c->pos == start) { c->pos = new_start; c->anchor = new_end; }
        else { c->pos = new_end; c->anchor = new_start; }
        
        g_free(indent_str);
    }
    
    document_end_undo_group(self->doc);
    /* Re-sync anchors? The loop updated them. */
}

static void
editor_widget_unindent_selection(EditorWidget *self)
{
    if (!self->doc) return;

    document_begin_undo_group(self->doc);
    g_array_sort(self->cursors, compare_cursors); 
    
    /* Reverse iteration */
    for (int i = (int)self->cursors->len - 1; i >= 0; i--) {
        EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
        size_t start = MIN(c->pos, c->anchor);
        size_t end = MAX(c->pos, c->anchor);
        
        size_t start_line = document_get_line_of_offset(self->doc, start);
        size_t end_line = document_get_line_of_offset(self->doc, end);
        
        if (end > start && end == document_get_offset_of_line(self->doc, end_line)) {
            if (end_line > start_line) end_line--;
        }
        
        size_t total_removed = 0;
        
        for (size_t l = start_line; l <= end_line; l++) {
            size_t line_off = document_get_offset_of_line(self->doc, l);
            size_t len;
            char *line_text = document_get_line(self->doc, l, &len);
            
            gboolean can_unindent = FALSE;
            size_t delete_len = 0;
            
            if (len > 0) {
                if (line_text[0] == '\t') {
                    can_unindent = TRUE;
                    delete_len = 1;
                } else if (line_text[0] == ' ') {
                    size_t spaces = 0;
                    while (spaces < self->indent_width && spaces < len && line_text[spaces] == ' ') {
                        spaces++;
                    }
                    if (spaces > 0) {
                        can_unindent = TRUE;
                        delete_len = spaces;
                    }
                }
            }
            g_free(line_text);
            
            if (can_unindent) {
                document_delete(self->doc, line_off, delete_len);
                total_removed += delete_len;
            }
        }
        
        /* Adjust selection */
        size_t new_start = document_get_offset_of_line(self->doc, start_line);
        size_t new_end = (end > total_removed) ? end - total_removed : 0;
        
        if (c->pos == start) { c->pos = new_start; c->anchor = new_end; }
        else { c->pos = new_end; c->anchor = new_start; }
    }
    
    document_end_undo_group(self->doc);
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

    if (keyval == GDK_KEY_Escape) {
        editor_widget_clear_cursors(self);
        EditorCursor *c = get_primary_cursor(self);
        c->anchor = c->pos;
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return TRUE;
    }

    if (self->is_dragging_selection && (keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R)) {
        self->drag_copy_mode = TRUE;
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return TRUE;
    }
    
    switch (keyval) {
        case GDK_KEY_Up:
            if ((state & GDK_ALT_MASK) && (state & GDK_SHIFT_MASK)) {
                editor_widget_add_cursor_vertically(self, -1);
            } else if (state & GDK_ALT_MASK) {
                editor_widget_move_lines_vertically(self, -1);
            } else {
                self->alt_word_mode = FALSE;
                move_cursor(self, -1);
                if (!(state & GDK_SHIFT_MASK)) editor_widget_sync_anchors(self);
            }
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Down:
            if ((state & GDK_ALT_MASK) && (state & GDK_SHIFT_MASK)) {
                editor_widget_add_cursor_vertically(self, 1);
            } else if (state & GDK_ALT_MASK) {
                editor_widget_move_lines_vertically(self, 1);
            } else {
                self->alt_word_mode = FALSE;
                move_cursor(self, 1);
                if (!(state & GDK_SHIFT_MASK)) editor_widget_sync_anchors(self);
            }
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Left:
            if (state & GDK_ALT_MASK) {
                editor_widget_move_selection_horizontally(self, -1);
            } else {
                self->alt_word_mode = FALSE;
                for(guint i=0; i<self->cursors->len; i++) {
                    EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
                    if (state & GDK_CONTROL_MASK) {
                        c->pos = word_prev(self, c->pos);
                    } else {
                        c->pos = utf8_prev_grapheme(self, c->pos);
                    }
                    if (!(state & GDK_SHIFT_MASK)) c->anchor = c->pos;
                }
                update_target_x(self);
            }
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Right:
            if (state & GDK_ALT_MASK) {
                editor_widget_move_selection_horizontally(self, 1);
            } else {
                self->alt_word_mode = FALSE;
                for(guint i=0; i<self->cursors->len; i++) {
                    EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
                    if (state & GDK_CONTROL_MASK) {
                         c->pos = word_next(self, c->pos);
                    } else {
                         c->pos = utf8_next_grapheme(self, c->pos);
                    }
                    if (!(state & GDK_SHIFT_MASK)) c->anchor = c->pos;
                }
                update_target_x(self);
            }
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Home:
        {
            for(guint i=0; i<self->cursors->len; i++) {
                EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
                if (state & GDK_CONTROL_MASK) {
                    c->pos = 0;
                } else {
                    size_t line = document_get_line_of_offset(self->doc, c->pos);
                    c->pos = document_get_offset_of_line(self->doc, line);
                }
                if (!(state & GDK_SHIFT_MASK)) c->anchor = c->pos;
            }
            update_target_x(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        }
        case GDK_KEY_End:
        {
            size_t doc_len = document_get_length(self->doc);
            for(guint i=0; i<self->cursors->len; i++) {
                EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
                if (state & GDK_CONTROL_MASK) {
                    c->pos = doc_len;
                } else {
                    size_t line = document_get_line_of_offset(self->doc, c->pos);
                    size_t len;
                    char *t = document_get_line(self->doc, line, &len);
                    size_t start = document_get_offset_of_line(self->doc, line);
                    size_t real_len = len;
                    if (len > 0) {
                        if (t[len-1] == '\n') real_len--;
                    }
                    g_free(t);
                    c->pos = start + real_len;
                }
                if (!(state & GDK_SHIFT_MASK)) c->anchor = c->pos;
            }
            update_target_x(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        }
        case GDK_KEY_Page_Up:
        {
             /* PageUp on Multi-Cursor is complex. Just move all by N visual lines? */
             int page_lines = 20; /* fallback */
             if (self->vadjustment) {
                 double page_px = gtk_adjustment_get_page_size(self->vadjustment);
                 page_lines = (int)(page_px / self->line_height);
                 if (page_lines<1) page_lines=1;
                 
                 /* Scroll */
                 double current = gtk_adjustment_get_value(self->vadjustment);
                 gtk_adjustment_set_value(self->vadjustment, current - page_px);
             }
             move_cursor(self, -page_lines);
             if (!(state & GDK_SHIFT_MASK)) editor_widget_sync_anchors(self);
             scroll_to_cursor(self);
             gtk_widget_queue_draw(GTK_WIDGET(self));
             break;
        }
        case GDK_KEY_Page_Down:
        {
             int page_lines = 20;
             if (self->vadjustment) {
                 double page_px = gtk_adjustment_get_page_size(self->vadjustment);
                 page_lines = (int)(page_px / self->line_height);
                 if (page_lines<1) page_lines=1;
                 
                 double current = gtk_adjustment_get_value(self->vadjustment);
                 gtk_adjustment_set_value(self->vadjustment, current + page_px);
             }
             move_cursor(self, page_lines);
             if (!(state & GDK_SHIFT_MASK)) editor_widget_sync_anchors(self);
             scroll_to_cursor(self);
             gtk_widget_queue_draw(GTK_WIDGET(self));
             break;   
        }
        case GDK_KEY_Return:
            editor_widget_delete_selection(self);
            
            document_begin_undo_group(self->doc);
            
            /* Sort cursors for forward processing */
            g_array_sort(self->cursors, compare_cursors);
            
            {
                long accumulated_shift = 0;
                
                for (guint i = 0; i < self->cursors->len; i++) {
                    EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
                    
                    /* Apply Shift from previous insertions */
                    if (accumulated_shift >= 0) {
                        c->pos += accumulated_shift;
                        c->anchor += accumulated_shift;
                    }

                    char *indent = NULL;
                    if (self->auto_indent) {
                        size_t line = document_get_line_of_offset(self->doc, c->pos);
                        size_t line_len;
                        char *text = document_get_line(self->doc, line, &line_len);
                        if (text) {
                            size_t k = 0;
                            while (k < line_len && (text[k] == ' ' || text[k] == '\t')) k++;
                            if (k > 0) indent = g_strndup(text, k);
                            g_free(text);
                        }
                    }
                    
                    document_insert(self->doc, c->pos, "\n", 1);
                    c->pos++;
                    accumulated_shift++;
                    
                    if (indent) {
                        size_t len = strlen(indent);
                        document_insert(self->doc, c->pos, indent, len);
                        c->pos += len;
                        accumulated_shift += len;
                        g_free(indent);
                    }
                    c->anchor = c->pos;
                }
            }
            document_end_undo_group(self->doc);
            
            editor_widget_reset_cursor_blink(self);
            editor_widget_update_adjustments(self, -1, -1);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Tab:
        case GDK_KEY_ISO_Left_Tab:
        {
            gboolean shift = (state & GDK_SHIFT_MASK) || (keyval == GDK_KEY_ISO_Left_Tab);
            
            if (shift) {
                editor_widget_unindent_selection(self);
            } else {
                gboolean any_selection = FALSE;
                for(guint i=0; i<self->cursors->len; i++) {
                    EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
                    if (c->pos != c->anchor) { any_selection = TRUE; break; }
                }
                
                if (any_selection) {
                    editor_widget_indent_selection(self);
                } else {
                    /* Insert Tab/Space at each cursor */
                    document_begin_undo_group(self->doc);
                    g_array_sort(self->cursors, compare_cursors);
                    
                    long accumulated_shift = 0;
                    
                    /* Forward Iteration for Insertion */
                    for (guint i = 0; i < self->cursors->len; i++) {
                         EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
                         
                         /* Apply Shift */
                         if (accumulated_shift >= 0) {
                             c->pos += accumulated_shift;
                             c->anchor += accumulated_shift;
                         }

                         if (self->indent_style == 0) {
                              char *sp = g_malloc(self->indent_width + 1);
                              memset(sp, ' ', self->indent_width);
                              sp[self->indent_width] = 0;
                              document_insert(self->doc, c->pos, sp, self->indent_width);
                              c->pos += self->indent_width;
                              accumulated_shift += self->indent_width;
                              g_free(sp);
                         } else {
                              document_insert(self->doc, c->pos, "\t", 1);
                              c->pos++;
                              accumulated_shift++;
                         }
                         c->anchor = c->pos;
                    }
                    document_end_undo_group(self->doc);
                }
            }
            editor_widget_reset_cursor_blink(self);
            editor_widget_update_adjustments(self, -1, -1);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        }
        case GDK_KEY_BackSpace:
            document_begin_undo_group(self->doc);
            g_array_sort(self->cursors, compare_cursors);
            {
                long accumulated_shift = 0;
                for (guint i = 0; i < self->cursors->len; i++) {
                     EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
                     
                     /* Apply Shift */
                     if (accumulated_shift < 0) {
                        size_t s = (size_t)(-accumulated_shift);
                        if (c->pos >= s) c->pos -= s; else c->pos = 0;
                        if (c->anchor >= s) c->anchor -= s; else c->anchor = 0;
                     }
                     
                     if (c->pos != c->anchor) {
                         /* Delete selection */
                         size_t start = MIN(c->pos, c->anchor);
                         size_t end = MAX(c->pos, c->anchor);
                         size_t del_len = end - start;
                         document_delete(self->doc, start, del_len);
                         c->pos = start;
                         c->anchor = start;
                         accumulated_shift -= del_len;
                     } else if (c->pos > 0) {
                         /* Delete previous char */
                         size_t prev = utf8_prev_grapheme(self, c->pos);
                         size_t del_len = c->pos - prev;
                         document_delete(self->doc, prev, del_len);
                         c->pos = prev;
                         c->anchor = c->pos;
                         accumulated_shift -= del_len;
                     }
                }
            }
            document_end_undo_group(self->doc);
            editor_widget_reset_cursor_blink(self);
            editor_widget_update_adjustments(self, -1, -1);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Delete:
            document_begin_undo_group(self->doc);
            g_array_sort(self->cursors, compare_cursors);
            
            {
                long accumulated_shift = 0;
                size_t doc_len = document_get_length(self->doc); // Need dynamic length check inside loop? 
                /* delete reduces length, so doc_len decreases. 
                   We should check against current length? Or implied by pos check?
                   Actually pos check needs to account for accumulated_shift.
                */
                
                for (guint i = 0; i < self->cursors->len; i++) {
                     EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
                     
                     /* Apply Shift */
                     if (accumulated_shift < 0) {
                        size_t s = (size_t)(-accumulated_shift);
                        if (c->pos >= s) c->pos -= s; else c->pos = 0;
                        if (c->anchor >= s) c->anchor -= s; else c->anchor = 0;
                     }
                     
                     size_t current_len = document_get_length(self->doc);
                     
                     if (c->pos != c->anchor) {
                         /* Delete selection */
                         size_t start = MIN(c->pos, c->anchor);
                         size_t end = MAX(c->pos, c->anchor);
                         size_t del_len = end - start;
                         document_delete(self->doc, start, del_len);
                         c->pos = start;
                         c->anchor = start;
                         accumulated_shift -= del_len;
                     } else if (c->pos < current_len) {
                         /* Delete next char */
                         size_t next = utf8_next_grapheme(self, c->pos);
                         size_t del_len = next - c->pos;
                         document_delete(self->doc, c->pos, del_len);
                         /* pos stays same */
                         c->anchor = c->pos;
                         accumulated_shift -= del_len;
                     }
                }
            }
            document_end_undo_group(self->doc);
            editor_widget_reset_cursor_blink(self);
            editor_widget_update_adjustments(self, -1, -1);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_z:
            if (state & GDK_CONTROL_MASK) {
                 UndoInfo info = document_undo(self->doc);
                 if (info.success) {
                     editor_widget_clear_cursors(self);
                     EditorCursor *c = get_primary_cursor(self);
                     if (info.is_insert) {
                         /* Text restored/inserted */
                         c->pos = info.start + info.length;
                         c->anchor = info.start;
                     } else {
                         /* Text deleted (undoing an insert) */
                         c->pos = info.start;
                         c->anchor = info.start;
                     }
                     scroll_to_cursor(self);
                 }
                 gtk_widget_queue_draw(GTK_WIDGET(self));
            }
            break;
        case GDK_KEY_y:
             if (state & GDK_CONTROL_MASK) {
                  UndoInfo info = document_redo(self->doc);
                  if (info.success) {
                      editor_widget_clear_cursors(self);
                      EditorCursor *c = get_primary_cursor(self);
                      if (info.is_insert) {
                          c->pos = info.start + info.length;
                          c->anchor = info.start;
                      } else {
                          c->pos = info.start;
                          c->anchor = info.start;
                      }
                      scroll_to_cursor(self);
                  }
                  gtk_widget_queue_draw(GTK_WIDGET(self));
             }
             break;
        case GDK_KEY_a:
             if (state & GDK_CONTROL_MASK) {
                  editor_widget_clear_cursors(self);
                  EditorCursor *c = get_primary_cursor(self);
                  c->pos = document_get_length(self->doc);
                  c->anchor = 0;
                  gtk_widget_queue_draw(GTK_WIDGET(self));
             }
             break;
        case GDK_KEY_c:
             if (state & GDK_CONTROL_MASK) editor_widget_copy(self);
             break;
        case GDK_KEY_x:
             if (state & GDK_CONTROL_MASK) editor_widget_cut(self);
             break;
        case GDK_KEY_v:
             if (state & GDK_CONTROL_MASK) editor_widget_paste(self);
             break;
        /* Duplicate GDK_KEY_a removed */
        /* IME handles letters */
        default:
            handled = FALSE;
            break;
    }
    
    return handled;
}

static gboolean
on_key_released(GtkEventControllerKey *controller,
                guint                  keyval,
                guint                  keycode,
                GdkModifierType        state,
                gpointer               user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    
    if (self->is_dragging_selection && (keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R)) {
        self->drag_copy_mode = FALSE;
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return TRUE;
    }
    
    return FALSE;
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
    
    /* First delete any selections at any cursor (updates cursors internally) */
    editor_widget_delete_selection(self);
    
    size_t len = strlen(str);
    if (len > 0) {
        document_begin_undo_group(self->doc);
        
        /* Sort cursors for forward processing */
        g_array_sort(self->cursors, compare_cursors);
        
        long accumulated_shift = 0;
        
        for (guint i = 0; i < self->cursors->len; i++) {
             EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
             
             /* Apply shift from prev insertions */
             if (accumulated_shift >= 0) {
                 c->pos += accumulated_shift;
                 c->anchor += accumulated_shift;
             }
             
             document_insert(self->doc, c->pos, str, len);
             c->pos += len;
             c->anchor = c->pos;
             
             accumulated_shift += len;
        }
        
        document_end_undo_group(self->doc);
    }
    self->alt_word_mode = FALSE;
    
    editor_widget_update_im_cursor_location(self);
    editor_widget_reset_cursor_blink(self);
    editor_widget_update_adjustments(self, -1, -1);
    scroll_to_cursor(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
editor_widget_update_im_cursor_location(EditorWidget *self)
{
    if (!self->im_context || !self->doc) return;

    EditorCursor *c = get_primary_cursor(self);
    if (!c) return;

    size_t line_idx = document_get_line_of_offset(self->doc, c->pos);
    size_t line_start = document_get_offset_of_line(self->doc, line_idx);
    size_t index_in_line = c->pos - line_start;

    char *text;
    size_t len;
    PangoLayout *layout = create_pango_layout_for_line(self, line_idx, &text, &len);
    if (!layout) return;

    if (index_in_line > len) index_in_line = len;

    PangoRectangle strong_pos;
    pango_layout_get_cursor_pos(layout, (int)index_in_line, &strong_pos, NULL);

    /* Fast approximation of Y offset. 
       Full precision requires iterating layouts, which is too slow for large files.
       Approx: line_idx * line_height. Subtract scroll position.
    */
    double y_off = (double)line_idx * self->line_height;
    double start_y = 0;
    if (self->vadjustment) start_y = gtk_adjustment_get_value(self->vadjustment);

    GdkRectangle area;
    area.x = (int)pango_units_to_double(strong_pos.x);
    area.y = (int)(y_off - (start_y * self->line_height) + pango_units_to_double(strong_pos.y));
    area.width = 1;
    area.height = (int)pango_units_to_double(strong_pos.height);

    gtk_im_context_set_cursor_location(self->im_context, &area);

    g_object_unref(layout);
    g_free(text);
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
    
    /* 
       Auto-scrolling during drag-and-drop 
    */
    if (self->is_dnd_active && self->vadjustment) {
        double height = gtk_widget_get_height(widget);
        double threshold = 30.0;
        double scroll_delta = 0;
        
        if (self->drag_y < threshold && self->drag_y >= 0) {
            /* Scroll up */
            scroll_delta = -((threshold - self->drag_y) / threshold) * 5.0;
        } else if (self->drag_y > height - threshold && self->drag_y <= height) {
            /* Scroll down */
            scroll_delta = ((self->drag_y - (height - threshold)) / threshold) * 5.0;
        }
        
        if (scroll_delta != 0) {
            double old_val = gtk_adjustment_get_value(self->vadjustment);
            double new_val = old_val + scroll_delta;
            double upper = gtk_adjustment_get_upper(self->vadjustment);
            double page_size = gtk_adjustment_get_page_size(self->vadjustment);
            
            if (new_val < 0) new_val = 0;
            if (new_val > upper - page_size) new_val = upper - page_size;
            
            if (new_val != old_val) {
                gtk_adjustment_set_value(self->vadjustment, new_val);
                
                /* Recalculate drop offset since viewport moved */
                size_t drop_off;
                editor_widget_get_offset_at_point(self, self->drag_x, self->drag_y, &drop_off);
                
                EditorCursor *c = get_primary_cursor(self);
                if (c) {
                    size_t sel_start = MIN(c->pos, c->anchor);
                    size_t sel_end = MAX(c->pos, c->anchor);

                    if (drop_off >= sel_start && drop_off < sel_end) {
                        self->drag_drop_offset = (size_t)-1;
                    } else {
                        self->drag_drop_offset = drop_off;
                    }
                }
                gtk_widget_queue_draw(widget);
            }
        }
    }
    
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
    if (self->font_desc) {
        pango_font_description_free(self->font_desc);
        self->font_desc = NULL;
    }
    if (self->im_context) g_object_unref(self->im_context);
    if (self->syntax_ctx) syntax_context_free(self->syntax_ctx);
    if (self->font_name) g_free(self->font_name);
    if (self->line_y_offsets) g_array_free(self->line_y_offsets, TRUE);
    if (self->idle_resize_id) {
        g_source_remove(self->idle_resize_id);
        self->idle_resize_id = 0;
    }
    
    /* Cleanup folding resources */
    if (self->fold_heads) {
        g_hash_table_destroy(self->fold_heads);
        self->fold_heads = NULL;
    }
    if (self->hidden_lines_cache) {
        g_free(self->hidden_lines_cache);
        self->hidden_lines_cache = NULL;
    }
    
    /* Disconnect GSettings signal and cleanup */
    if (self->interface_settings) {
        g_signal_handlers_disconnect_by_func(self->interface_settings, on_system_font_changed, self);
        g_clear_object(&self->interface_settings);
    }
    
    if (self->cursors) g_array_free(self->cursors, TRUE);
    
    G_OBJECT_CLASS(editor_widget_parent_class)->dispose(object);
}

static void
editor_widget_init(EditorWidget *self)
{
    /* Initialize custom font name to default (used when custom font is enabled) */
    self->font_name = g_strdup("Monospace 11");
    self->use_custom_font = FALSE;
    
    /* Monitor system font changes from GNOME settings */
    self->interface_settings = g_settings_new("org.gnome.desktop.interface");
    g_signal_connect(self->interface_settings, "changed::monospace-font-name", 
                     G_CALLBACK(on_system_font_changed), self);
    
    /* Initialize font_desc to NULL; ensure_metrics will set it based on use_custom_font */
    self->font_desc = NULL;
    
    self->cursors = g_array_new(FALSE, FALSE, sizeof(EditorCursor));
    EditorCursor c = {0, 0, -1};
    g_array_append_val(self->cursors, c);
    
    self->line_y_offsets = g_array_new(FALSE, FALSE, sizeof(double));
    
    /* Initialize cursor blink animation */
    self->cursor_alpha = 1.0;
    self->cursor_blink_start_time = 0;
    self->cursor_blink_tick_id = gtk_widget_add_tick_callback(
        GTK_WIDGET(self), cursor_blink_tick_callback, NULL, NULL);
    
    self->drag_drop_offset = (size_t)-1;
    self->drag_copy_mode = FALSE;
    self->drag_ghost_layout = NULL;
    
    /* Autoscroll timer initialization */
    self->autoscroll_timer_id = 0;
    self->autoscroll_direction = 0;
    self->autoscroll_speed = 0;
    
    /* Viewport padding */
    self->padding_left = 4;
    self->padding_top = 8;
    
    /* Config defaults */
    self->show_line_numbers = TRUE;
    self->highlight_current_line = TRUE;
    self->show_right_margin = FALSE;
    self->right_margin_position = 80;
    self->wrap_lines = TRUE;
    self->auto_indent = TRUE;
    self->indent_style = 0; /* Space */
    self->tab_width = 4;
    self->indent_width = 4;
    
    GtkEventController *controller = gtk_event_controller_key_new();
    g_signal_connect(controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(controller, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), controller);
    
    self->im_context = gtk_im_multicontext_new();
    gtk_im_context_set_client_widget(self->im_context, GTK_WIDGET(self));
    g_signal_connect(self->im_context, "commit", G_CALLBACK(on_im_commit), self);

    GtkEventController *focus_controller = gtk_event_controller_focus_new();
    g_signal_connect(focus_controller, "enter", G_CALLBACK(on_focus_enter), self);
    g_signal_connect(focus_controller, "leave", G_CALLBACK(on_focus_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), focus_controller);
    
    self->syntax_ctx = syntax_context_new();
    
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); /* Listen to all buttons */
    g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), self);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
    g_signal_connect(drag, "drag-end", G_CALLBACK(on_drag_end), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(drag));
    
    /* Scroll controller for mouse wheel (works even when scrollbar is hidden) */
    GtkEventController *scroll_ctrl = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll_ctrl, "scroll", G_CALLBACK(on_scroll), self);
    gtk_widget_add_controller(GTK_WIDGET(self), scroll_ctrl);
    
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
                 /* Update adjustments (with current size) */
    editor_widget_update_adjustments(self, -1, -1);
            }
            break;
        case PROP_HSCROLL_POLICY:
            self->hscroll_policy = g_value_get_enum(value);
            break;
        case PROP_VSCROLL_POLICY:
            self->vscroll_policy = g_value_get_enum(value);
            break;
        case PROP_SHOW_LINE_NUMBERS:
            self->show_line_numbers = g_value_get_boolean(value);
            gtk_widget_queue_resize(GTK_WIDGET(self));
            break;
        case PROP_HIGHLIGHT_CURRENT_LINE:
            self->highlight_current_line = g_value_get_boolean(value);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case PROP_SHOW_RIGHT_MARGIN:
            self->show_right_margin = g_value_get_boolean(value);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case PROP_RIGHT_MARGIN_POSITION:
            self->right_margin_position = g_value_get_int(value);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case PROP_WRAP_LINES:
            self->wrap_lines = g_value_get_boolean(value);
            gtk_widget_queue_resize(GTK_WIDGET(self)); /* Affects layout width */
            break;
        case PROP_AUTO_INDENT:
            self->auto_indent = g_value_get_boolean(value);
            break;
        case PROP_INDENT_STYLE:
            self->indent_style = g_value_get_int(value);
            break;
        case PROP_TAB_WIDTH:
            self->tab_width = g_value_get_int(value);
            break;
        case PROP_INDENT_WIDTH:
            self->indent_width = g_value_get_int(value);
            break;
        case PROP_USE_CUSTOM_FONT:
            self->use_custom_font = g_value_get_boolean(value);
            self->line_height = 0; /* Force re-calculation */
            editor_widget_ensure_metrics(self);
            gtk_widget_queue_resize(GTK_WIDGET(self));
            break;
        case PROP_FONT_NAME:
            g_free(self->font_name);
            self->font_name = g_value_dup_string(value);
            self->line_height = 0; /* Force re-calculation */
            editor_widget_ensure_metrics(self);
            gtk_widget_queue_resize(GTK_WIDGET(self));
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
        case PROP_SHOW_LINE_NUMBERS:
            g_value_set_boolean(value, self->show_line_numbers);
            break;
        case PROP_HIGHLIGHT_CURRENT_LINE:
            g_value_set_boolean(value, self->highlight_current_line);
            break;
        case PROP_SHOW_RIGHT_MARGIN:
            g_value_set_boolean(value, self->show_right_margin);
            break;
        case PROP_RIGHT_MARGIN_POSITION:
            g_value_set_int(value, self->right_margin_position);
            break;
        case PROP_WRAP_LINES:
            g_value_set_boolean(value, self->wrap_lines);
            break;
        case PROP_AUTO_INDENT:
            g_value_set_boolean(value, self->auto_indent);
            break;
        case PROP_INDENT_STYLE:
            g_value_set_int(value, self->indent_style);
            break;
        case PROP_TAB_WIDTH:
            g_value_set_int(value, self->tab_width);
            break;
        case PROP_INDENT_WIDTH:
            g_value_set_int(value, self->indent_width);
            break;
        case PROP_USE_CUSTOM_FONT:
            g_value_set_boolean(value, self->use_custom_font);
            break;
        case PROP_FONT_NAME:
            g_value_set_string(value, self->font_name);
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
    
    g_object_class_install_property(object_class, PROP_SHOW_LINE_NUMBERS,
        g_param_spec_boolean("show-line-numbers", "Show Line Numbers", "Show Line Numbers", TRUE, G_PARAM_READWRITE));
    
    g_object_class_install_property(object_class, PROP_HIGHLIGHT_CURRENT_LINE,
        g_param_spec_boolean("highlight-current-line", "Highlight Current Line", "Highlight Current Line", TRUE, G_PARAM_READWRITE));
        
    g_object_class_install_property(object_class, PROP_SHOW_RIGHT_MARGIN,
        g_param_spec_boolean("show-right-margin", "Show Right Margin", "Show Right Margin", FALSE, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_RIGHT_MARGIN_POSITION,
        g_param_spec_int("right-margin-position", "Right Margin Position", "Right Margin Position", 1, 200, 80, G_PARAM_READWRITE));
        
    g_object_class_install_property(object_class, PROP_WRAP_LINES,
        g_param_spec_boolean("wrap-lines", "Wrap Lines", "Wrap Lines", TRUE, G_PARAM_READWRITE));
        
    g_object_class_install_property(object_class, PROP_AUTO_INDENT,
        g_param_spec_boolean("auto-indent", "Auto Indentation", "Auto Indentation", TRUE, G_PARAM_READWRITE));
        
    g_object_class_install_property(object_class, PROP_INDENT_STYLE,
        g_param_spec_int("indent-style", "Indent Style", "Indent Style", 0, 1, 0, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_TAB_WIDTH,
        g_param_spec_int("tab-width", "Tab Width", "Tab Width", 1, 16, 4, G_PARAM_READWRITE));
        
    g_object_class_install_property(object_class, PROP_INDENT_WIDTH,
        g_param_spec_int("indent-width", "Indent Width", "Indent Width", 1, 16, 4, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_USE_CUSTOM_FONT,
        g_param_spec_boolean("use-custom-font", "Use Custom Font", "Use Custom Font", FALSE, G_PARAM_READWRITE));
        
    g_object_class_install_property(object_class, PROP_FONT_NAME,
        g_param_spec_string("font-name", "Font Name", "Font Name", "Monospace 11", G_PARAM_READWRITE));
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
    
    /* Reset cursors */
    editor_widget_clear_cursors(self);
    EditorCursor *c = get_primary_cursor(self);
    if (c) {
        c->pos = 0;
        c->anchor = 0;
        c->target_x = -1;
    }
    
    /* Removed large file auto-disable. Relying on efficient O(N) linear scan now. */
    
    /* Update adjustments (with current size) */
    editor_widget_update_adjustments(self, -1, -1);
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
