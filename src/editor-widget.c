#include "editor-widget.h"
#include "syntax.h"
#include <math.h>

typedef struct {
    size_t cursor_offset;
    size_t selection_anchor;
    double target_x;
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
    double ascent;
    double cached_char_width;
    PangoFontDescription *font_desc;
    
    /* State */
    /* State */
    /* State */
    GArray *cursors; /* Array of EditorCursor */
    
    /* Primary cursor cache (for compatibility with existing logic) */
    size_t cursor_offset;
    size_t selection_anchor;
    double target_x;
    
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
    
    /* System font monitoring */
    GSettings *interface_settings;
};

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

static EditorCursor *
editor_widget_get_primary_cursor(EditorWidget *self)
{
    if (!self->cursors || self->cursors->len == 0) return NULL;
    return &g_array_index(self->cursors, EditorCursor, 0);
}

static void
editor_widget_clear_cursors(EditorWidget *self)
{
    if (self->cursors->len > 1) {
        g_array_set_size(self->cursors, 1);
    }
}

static void
editor_widget_add_cursor(EditorWidget *self, size_t offset)
{
    /* Check for existing cursor at this offset and remove it (Toggle behavior) */
    for (guint i = 0; i < self->cursors->len; i++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, i);
        if (cur->cursor_offset == offset && cur->selection_anchor == offset) {
            /* Found exact match - remove it */
            /* If it's the only cursor (primary loop), don't remove? 
               Usually Alt+Click on LAST cursor removes it? 
               If we remove the last remaining cursor, we have 0 cursors. 
               We should prevent removing if len == 1. 
            */
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

    return (digits * char_w) + 8.0; /* 4px left + 4px right */
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

    /* Horizontal Adjustment */
    double content_width = 0;
    
    if (self->wrap_lines) {
        content_width = (double)widget_width;
    } else {
        /* Approximate width based on max line length */
        if (total_lines < 50000) {
            MaxWidthCalcState mw_state = {0};
            document_foreach_line(self->doc, calculate_max_line_len_cb, &mw_state);
            
            double gutter = get_effective_gutter_width(self);
            /* Approximate: max bytes * char width + padding. 
               (For exactness, we'd need to measure the specific longest line with Pango, 
                but bytes * char_width is a decent O(N) proxy for monospace). */
            content_width = mw_state.max_len_bytes * self->cached_char_width + gutter + self->padding_left * 2 + 50; /* +50 extra buffer */
        } else {
            /* Large file fallback: don't scan. Allow some fixed scroll range or just widget width */
            /* Let's give a generous default for large files so they can at least scroll a bit if needed */
            content_width = widget_width * 2.0; 
        }
    }
    
    if (content_width < widget_width) content_width = widget_width;

    if (self->hadjustment) {
        gtk_adjustment_configure(self->hadjustment,
                                 gtk_adjustment_get_value(self->hadjustment),
                                 0,
                                 content_width,
                                 self->cached_char_width, /* step */
                                 widget_width,           /* page */
                                 widget_width);          /* page_size */
    }
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

    double scroll_x = 0;
    if (self->hadjustment)
        scroll_x = gtk_adjustment_get_value(self->hadjustment);



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
        cursor_lines[c] = document_get_line_of_offset(self->doc, cur->cursor_offset);
    }


    double current_y_pos = -partial_y; /* Start with calculated offset */
    double text_start_x = gutter_w + self->padding_left;

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
                 if (cursor_lines[c] == line_idx && cur->cursor_offset == cur->selection_anchor) {
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
            /* Translate X=4 for 4px left padding. Use centering_offset for consistent alignment. */
            gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(4, current_y_pos + self->padding_top + centering_offset));
            gtk_snapshot_append_layout(snapshot, lnum_layout, &gutter_fg);
            gtk_snapshot_restore(snapshot);
            
            g_object_unref(lnum_layout);
        }

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
        size_t line_start_off = document_get_offset_of_line(self->doc, line_idx);
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
                
                if (self->cursor_alpha > 0.01 && !has_selection && !self->is_dragging_selection) {
                     size_t index_in_line = cur->cursor_offset - line_start_off;
                     /* Safety clamp */
                     if (index_in_line > len) index_in_line = len;
                     
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
            if (line_idx == drop_line) {
                size_t line_start_off = document_get_offset_of_line(self->doc, line_idx);
                size_t index_in_line = self->drag_drop_offset - line_start_off;
                if (index_in_line > len) index_in_line = len;

                PangoRectangle strong_pos;
                pango_layout_get_cursor_pos(layout, (int)index_in_line, &strong_pos, NULL);

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

    /* Cleanup cursor_lines if heap allocated */
    if (cursor_lines != cursor_lines_stack) {
        g_free(cursor_lines);
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

    size_t off;
    editor_widget_get_offset_at_point(self, x, y, &off);
    
    editor_widget_reset_cursor_blink(self);
    
    if (n_press == 2) {
        /* Double click - select word */
        /* Always operate on the NEW primary cursor (or the single one if cleared) */
        
        gboolean alt_held = (gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)) & GDK_ALT_MASK);
        
        if (!alt_held) {
             editor_widget_clear_cursors(self);
             /* Primary cursor will be moved/expanded below */
        } else {
             /* Add new cursor if not close to existing? For now just add */
             editor_widget_add_cursor(self, off);
        }
        
        EditorCursor *primary = editor_widget_get_primary_cursor(self);
        if (!primary) return; /* Should not happen */

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
            primary->selection_anchor = sel_end;
            primary->cursor_offset = sel_start;
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
                primary->selection_anchor = ws_end;
                primary->cursor_offset = ws_start;
            } else {
                size_t word_start, word_end;
                editor_widget_find_word_boundary(self, off, &word_start, &word_end);
                primary->selection_anchor = word_end;
                primary->cursor_offset = word_start;
            }
        }
        self->alt_word_mode = TRUE;
        self->multi_click_selection = TRUE;
        self->multi_click_mode = 2;
        self->multi_click_start = MIN(primary->selection_anchor, primary->cursor_offset);
        self->multi_click_end = MAX(primary->selection_anchor, primary->cursor_offset);
        
    } else if (n_press == 3) {
        /* Triple click */
        gboolean alt_held = (gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture)) & GDK_ALT_MASK);
        if (!alt_held) {
             editor_widget_clear_cursors(self);
        } else {
             editor_widget_add_cursor(self, off);
        }
        EditorCursor *primary = editor_widget_get_primary_cursor(self);
        
        size_t line_start, line_end;
        find_line_at_offset(self->doc, off, &line_start, &line_end);
        primary->selection_anchor = line_end;
        primary->cursor_offset = line_start;
        self->alt_word_mode = TRUE;
        self->multi_click_selection = TRUE;
        self->multi_click_mode = 3;
        self->multi_click_start = line_start;
        self->multi_click_end = line_end;
    } else {
        /* Single click */
        self->multi_click_selection = FALSE;
        GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
        
        if (state & GDK_ALT_MASK) {
            /* Alt+Click: Add cursor (or toggle) */
            editor_widget_add_cursor(self, off);
             /* Do NOT move primary cursor here! add_cursor creates new one at off. */
             self->alt_word_mode = FALSE;
        } else {
             /* Check if clicking inside selection of ANY cursor */
             gboolean click_in_selection = FALSE;
             EditorCursor *clicked_cursor = NULL;
             
             for (guint c = 0; c < self->cursors->len; c++) {
                 EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                 size_t sel_start = MIN(cur->cursor_offset, cur->selection_anchor);
                 size_t sel_end = MAX(cur->cursor_offset, cur->selection_anchor);
                 if (sel_start != sel_end && off >= sel_start && off < sel_end) {
                     click_in_selection = TRUE;
                     clicked_cursor = cur;
                     break; 
                 }
             }

             if (state & GDK_SHIFT_MASK) {
                 /* Shift+Click: collapse to single selection extending from primary anchor */
                 editor_widget_clear_cursors(self);
                 EditorCursor *primary = editor_widget_get_primary_cursor(self);
                 
                 /* Smart Pivot on Primary */
                 size_t p_start = MIN(primary->cursor_offset, primary->selection_anchor);
                 size_t p_end = MAX(primary->cursor_offset, primary->selection_anchor);
                 
                 /* Update primary to extend to 'off' */
                 if (off >= p_end) {
                     /* Extend forward: Anchor stays at start */
                     primary->selection_anchor = p_start;
                     primary->cursor_offset = off;
                 } else if (off < p_start) {
                     /* Extend backward: Anchor moves to end */
                     primary->selection_anchor = p_end;
                     primary->cursor_offset = off;
                 } else {
                     /* Clicking inside: shrinks but keeps anchor direction usually? 
                        VS Code logic: Anchor assumes the "pivot" point. 
                        If we were selecting [10, 20] (caret at 20) and click 15 -> [10, 15] (caret 15).
                        Anchor (10) stays.
                     */
                     primary->cursor_offset = off;
                 }
                 /* Sync cache */
                 self->cursor_offset = primary->cursor_offset;
                 self->selection_anchor = primary->selection_anchor;
                 
                 self->alt_word_mode = FALSE;
             } else if (click_in_selection) {
                 /* Drag start logic */
                 self->is_dragging_selection = TRUE;
                 self->drag_start_offset = off;
                 /* Which cursor are we dragging? 
                    For now, treat "dragging selection" as valid if any cursor selected it.
                    We don't modify cursors yet.
                 */
             } else {
                 /* Normal click: Clear all, move primary to here */
                 editor_widget_clear_cursors(self);
                 EditorCursor *primary = editor_widget_get_primary_cursor(self);
                 primary->cursor_offset = off;
                 primary->selection_anchor = off;
                 primary->target_x = -1.0; /* Will be updated by update_target_x */
                 
                 /* Sync cache to ensure drag selection uses correct anchor */
                 self->cursor_offset = primary->cursor_offset;
                 self->selection_anchor = primary->selection_anchor;
                 self->target_x = primary->target_x;
                 self->alt_word_mode = FALSE;
             }
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
    
    /* Check if we're starting a drag on a selection (iterate all cursors) */
    size_t off;
    editor_widget_get_offset_at_point(self, x, y, &off);
    
    gboolean in_selection = FALSE;
    EditorCursor *found_cur = NULL;
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        size_t s = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t e = MAX(cur->cursor_offset, cur->selection_anchor);
        if (s != e && off >= s && off < e) {
            in_selection = TRUE;
            found_cur = cur;
            break;
        }
    }
    
    if (in_selection && found_cur) {
        /* Starting drag on existing selection - prepare for DnD */
        self->is_dragging_selection = TRUE;
        self->drag_start_offset = off;

        /* Create ghost layout for the selected text */
        size_t s = MIN(found_cur->cursor_offset, found_cur->selection_anchor);
        size_t e = MAX(found_cur->cursor_offset, found_cur->selection_anchor);
        
        char *text = document_get_text_range(self->doc, s, e - s);
        if (text) {
            if (self->drag_ghost_layout) g_object_unref(self->drag_ghost_layout);
            self->drag_ghost_layout = gtk_widget_create_pango_layout(GTK_WIDGET(self), text);
            pango_layout_set_font_description(self->drag_ghost_layout, self->font_desc);
            g_free(text);
        }
        self->is_dnd_active = FALSE; /* Will be set to true if drag threshold passed */
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
            
            size_t sel_start = MIN(self->cursor_offset, self->selection_anchor);
            size_t sel_end = MAX(self->cursor_offset, self->selection_anchor);
            
            if (drop_off >= sel_start && drop_off < sel_end) {
                self->drag_drop_offset = (size_t)-1;
            } else {
                self->drag_drop_offset = drop_off;
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

static int
compare_cursors_desc(gconstpointer a, gconstpointer b)
{
    const EditorCursor *ca = (const EditorCursor *)a;
    const EditorCursor *cb = (const EditorCursor *)b;
    /* Sort descending by offset */
    if (ca->cursor_offset > cb->cursor_offset) return -1;
    if (ca->cursor_offset < cb->cursor_offset) return 1;
    return 0;
}

static void
editor_widget_backspace(EditorWidget *self)
{
    if (!self->cursors || self->cursors->len == 0) return;
    
    /* Sort cursors descending to avoid index shifting issues */
    g_array_sort(self->cursors, compare_cursors_desc);
    
    document_begin_undo_group(self->doc);
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        
        long delta = 0;
        
        if (start != end) {
             /* Delete selection */
             document_delete(self->doc, start, end - start);
             cur->cursor_offset = start;
             cur->selection_anchor = start;
             delta -= (long)(end - start);
        } else {
             /* Delete char before */
             if (cur->cursor_offset > 0) {
                 document_delete(self->doc, cur->cursor_offset - 1, 1);
                 cur->cursor_offset--;
                 cur->selection_anchor = cur->cursor_offset;
                 delta = -1;
             }
        }
        
        if (delta != 0) {
            for (guint j = 0; j < c; j++) {
                EditorCursor *prev = &g_array_index(self->cursors, EditorCursor, j);
                if ((long)prev->cursor_offset + delta < 0) prev->cursor_offset = 0;
                else prev->cursor_offset += delta;
                
                if ((long)prev->selection_anchor + delta < 0) prev->selection_anchor = 0;
                else prev->selection_anchor += delta;
            }
        }
    }
    
    document_end_undo_group(self->doc);
    
    editor_widget_reset_cursor_blink(self);
    editor_widget_update_adjustments(self, -1, -1);
    scroll_to_cursor(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
editor_widget_delete(EditorWidget *self)
{
    if (!self->cursors || self->cursors->len == 0) return;
    
    /* Sort cursors descending */
    g_array_sort(self->cursors, compare_cursors_desc);
    
    document_begin_undo_group(self->doc);
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        
        long delta = 0;
        
        if (start != end) {
             document_delete(self->doc, start, end - start);
             cur->cursor_offset = start;
             cur->selection_anchor = start;
             delta -= (long)(end - start);
        } else {
             /* Delete char after */
             size_t total = document_get_length(self->doc);
             if (cur->cursor_offset < total) {
                 document_delete(self->doc, cur->cursor_offset, 1);
                 /* Offset stays same */
                 delta = -1;
             }
        }
        
        if (delta != 0) {
            for (guint j = 0; j < c; j++) {
                EditorCursor *prev = &g_array_index(self->cursors, EditorCursor, j);
                if ((long)prev->cursor_offset + delta < 0) prev->cursor_offset = 0;
                else prev->cursor_offset += delta;
                
                if ((long)prev->selection_anchor + delta < 0) prev->selection_anchor = 0;
                else prev->selection_anchor += delta;
            }
        }
    }
    
    document_end_undo_group(self->doc);
    
    editor_widget_reset_cursor_blink(self);
    editor_widget_update_adjustments(self, -1, -1);
    scroll_to_cursor(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
static void
update_selection_extension(EditorWidget *self, size_t off)
{
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
            size_t drop_off;
            editor_widget_get_offset_at_point(self, self->drag_x, self->drag_y, &drop_off);
            
            EditorCursor *primary = editor_widget_get_primary_cursor(self);
            size_t sel_start = MIN(primary->cursor_offset, primary->selection_anchor);
            size_t sel_end = MAX(primary->cursor_offset, primary->selection_anchor);

            /* Rule: Never show caret inside or overlap selected range */
            if (drop_off >= sel_start && drop_off < sel_end) {
                self->drag_drop_offset = (size_t)-1; /* Suppress caret */
            } else {
                self->drag_drop_offset = drop_off;
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

    EditorCursor *primary = editor_widget_get_primary_cursor(self);
    size_t sel_start = MIN(primary->cursor_offset, primary->selection_anchor);
    size_t sel_end = MAX(primary->cursor_offset, primary->selection_anchor);
    size_t sel_len = sel_end - sel_start;
    
    char *text = document_get_text_range(self->doc, sel_start, sel_len);
    if (!text) return;
    
    document_begin_undo_group(self->doc);
    
    /* If moving, delete original first */
    /* Be careful if drop_off is after delete point, it shifts */
    if (!self->drag_copy_mode) {
        document_delete(self->doc, sel_start, sel_len);
        if (drop_off > sel_end) {
            drop_off -= sel_len;
        }
    }
    
    document_insert(self->doc, drop_off, text, sel_len);
    
    /* Select dropped text */
    /* Select dropped text */
    primary = editor_widget_get_primary_cursor(self);
    primary->selection_anchor = drop_off;
    primary->cursor_offset = drop_off + sel_len;
    self->alt_word_mode = FALSE;
    
    document_end_undo_group(self->doc);
    
    g_free(text);
    
    /* Force update */
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
        /* Finalize drag drop? handled in drag_drop signal usually? 
           Local DnD is simulated here. */
        if (self->is_dnd_active && self->drag_drop_offset != (size_t)-1) {
            /* Perform move/copy */
            editor_widget_drag_drop_finish(self, self->drag_drop_offset);
        } else if (!self->is_dnd_active) {
            /* Clicked inside selection without dragging -> Clear selection */
            EditorCursor *primary = editor_widget_get_primary_cursor(self);
            primary->cursor_offset = self->drag_start_offset;
            primary->selection_anchor = self->drag_start_offset;
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

/* Helper for comparing cursors - need forward declaration or static here */
static int compare_cursors_desc(gconstpointer a, gconstpointer b);

static size_t
editor_widget_delete_selection(EditorWidget *self)
{
    /* This function handles bulk deletion of all selections */
    if (!self->cursors || self->cursors->len == 0) return 0;
    
    g_array_sort(self->cursors, compare_cursors_desc);
    
    size_t total_deleted = 0;
    document_begin_undo_group(self->doc);
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        
        if (start != end) {
            size_t len = end - start;
            document_delete(self->doc, start, len);
            total_deleted += len;
            cur->cursor_offset = start;
            cur->selection_anchor = start;
        }
    }
    
    document_end_undo_group(self->doc);
    return total_deleted;
}

static void
editor_widget_copy(EditorWidget *self)
{
    /* Copy logic for multi-cursor:
       VS Code joins selection with newlines. 
    */
    GString *clip_text = g_string_new("");
    
    /* Sort explicit ascending for copy order */
    /* Or just iterate default? Default array order seems insertion order. */
    /* Usually we want document order. */
    /* Let's dup and sort ascending. */
    
    GArray *sorted = g_array_sized_new(FALSE, FALSE, sizeof(EditorCursor), self->cursors->len);
    g_array_append_vals(sorted, self->cursors->data, self->cursors->len);
    /* Ascending sort needed? We did descending earlier. */
    /* Implement compare_cursors_asc inline or helper? */
    /* Simple bubble sort for small N or just iterate finding min? */
    /* Actually we need a real sort. */
    /* Let's just traverse array. If we want doc order, we should sort. */
    
    /* Assume sorted for now or TODO implement sort. */
    
    for (guint c = 0; c < self->cursors->len; c++) {
         EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
         size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
         size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
         if (start == end) continue;
         
         char *text = document_get_text_range(self->doc, start, end - start);
         if (clip_text->len > 0) g_string_append_c(clip_text, '\n');
         g_string_append(clip_text, text);
         g_free(text);
    }
    
    if (clip_text->len > 0) {
        GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
        gdk_clipboard_set_text(clipboard, clip_text->str);
    }
    g_string_free(clip_text, TRUE);
    g_array_free(sorted, TRUE);
}

static void
editor_widget_cut(EditorWidget *self)
{
    if (!self->doc) return;
    editor_widget_copy(self);
    editor_widget_delete_selection(self); 
    /* delete_selection implements bulk delete sorted descending */
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
        /* If we have a selection, delete it first */
        editor_widget_delete_selection(self);
        
        /* Insert at all cursors (delete_selection already reduced to insertion points) */
        /* Actually delete_selection might simplify things but paste usually inserts at all cursors */
        /* If we just did delete_selection, cursors are at correct spots. */
        
        for (guint c = 0; c < self->cursors->len; c++) {
             EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
             document_insert(self->doc, cur->cursor_offset, text, len);
             cur->cursor_offset += len;
             cur->selection_anchor = cur->cursor_offset;
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
            /* Simple insert at cursor (or should it respect primary selection rules? Standard is insert at click? 
               But this is usually called by middle click logic which sets cursor first) */
            
            /* Wait, middle click sets cursor_offset in on_click_pressed. 
               So we insert at primary cursor. */
               
            EditorCursor *primary = editor_widget_get_primary_cursor(self);
            document_begin_undo_group(self->doc);
            document_insert(self->doc, primary->cursor_offset, text, len);
            
            /* Advance cursor */
            primary->cursor_offset += len;
            primary->selection_anchor = primary->cursor_offset;
            
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
    
    EditorCursor *primary = editor_widget_get_primary_cursor(self);
    if (!primary) return;
    
    /* SYNC CACHE from Primary Cursor */
    self->cursor_offset = primary->cursor_offset;
    self->selection_anchor = primary->selection_anchor;
    self->target_x = primary->target_x;
    
    size_t cursor_line = document_get_line_of_offset(self->doc, primary->cursor_offset);
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
    size_t idx = self->cursor_offset - line_start;
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
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        
        size_t line_idx = document_get_line_of_offset(self->doc, cur->cursor_offset);
        size_t line_start = document_get_offset_of_line(self->doc, line_idx);
        size_t index_in_line = cur->cursor_offset - line_start;
        
        char *text; size_t len;
        PangoLayout *layout = create_pango_layout_for_line(self, line_idx, &text, &len);
        if (!layout) continue;
        
        if (index_in_line > len) index_in_line = len;
        
        PangoRectangle strong_pos;
        pango_layout_get_cursor_pos(layout, (int)index_in_line, &strong_pos, NULL);
        cur->target_x = pango_units_to_double(strong_pos.x);
        
        g_object_unref(layout);
        g_free(text);
    }
}

static void
move_cursor(EditorWidget *self, int visual_lines_delta)
{
    if (visual_lines_delta == 0) return;
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        
        size_t line_idx = document_get_line_of_offset(self->doc, cur->cursor_offset);
        size_t line_start = document_get_offset_of_line(self->doc, line_idx);
        size_t char_idx = cur->cursor_offset - line_start;
        
        char *text = NULL; size_t len;
        PangoLayout *layout = create_pango_layout_for_line(self, line_idx, &text, &len);
        if (!layout) continue;

        if (cur->target_x < 0) {
             PangoRectangle strong_pos;
             pango_layout_get_cursor_pos(layout, (int)MIN(char_idx, len), &strong_pos, NULL);
             cur->target_x = pango_units_to_double(strong_pos.x);
        }
        
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
        
        if (!found_v_line) current_v_line_idx = MAX(0, pango_layout_get_line_count(layout) - 1);
        
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
                pango_layout_line_x_to_index(v_line, (int)(cur->target_x * PANGO_SCALE), &index, &trailing);
                size_t ls = document_get_offset_of_line(self->doc, current_line_idx);
                cur->cursor_offset = ls + index + trailing;
                pango_layout_iter_free(iter);
                break;
            } else {
                if (local_delta > 0) {
                     int lines_remaining = total_v_lines - current_v_line_idx - 1;
                     local_delta -= (lines_remaining + 1);
                     size_t next_line = current_line_idx + 1;
                     if (next_line >= document_get_line_count(self->doc)) {
                         iter = pango_layout_get_iter(layout);
                         int last_v = total_v_lines - 1; 
                         for (int i = 0; i < last_v; i++) pango_layout_iter_next_line(iter);
                         PangoLayoutLine *v_line = pango_layout_iter_get_line_readonly(iter);
                         int index, trailing;
                         pango_layout_line_x_to_index(v_line, (int)(cur->target_x * PANGO_SCALE), &index, &trailing);
                         size_t ls = document_get_offset_of_line(self->doc, current_line_idx);
                         cur->cursor_offset = ls + index + trailing;
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
                     if (current_line_idx == 0) {
                         iter = pango_layout_get_iter(layout);
                         PangoLayoutLine *v_line = pango_layout_iter_get_line_readonly(iter);
                         int index, trailing;
                         pango_layout_line_x_to_index(v_line, (int)(cur->target_x * PANGO_SCALE), &index, &trailing);
                         size_t ls = document_get_offset_of_line(self->doc, current_line_idx);
                         cur->cursor_offset = ls + index + trailing;
                         pango_layout_iter_free(iter);
                         break;
                     }
                     current_line_idx--;
                     g_object_unref(layout); g_free(text);
                     layout = create_pango_layout_for_line(self, current_line_idx, &text, &len);
                     current_v_line_idx = pango_layout_get_line_count(layout) - 1; 
                }
            }
        }
        g_object_unref(layout);
        g_free(text);
    }
    
    editor_widget_update_im_cursor_location(self);
}

static void
editor_widget_add_cursor_vertically(EditorWidget *self, int visual_lines_delta)
{
    if (!self->cursors || self->cursors->len == 0) return;
    
    /* Sort to find extreme cursor */
    g_array_sort(self->cursors, compare_cursors_desc);
    
    EditorCursor *ref_cur = NULL;
    if (visual_lines_delta > 0) {
        /* Down: Use last cursor (index 0 is MAX in descending sort? Wait check sort) */
        /* compare_cursors_desc: if a > b return -1. So Max is at 0. */
        /* So Descending: [Max, ..., Min] */
        /* Moving Down (increasing offset): Start from Max (index 0). */
         ref_cur = &g_array_index(self->cursors, EditorCursor, 0); 
    } else {
        /* Up: Start from Min (last index) */
        ref_cur = &g_array_index(self->cursors, EditorCursor, self->cursors->len - 1);
    }
    
    /* Logic to find target offset - duplicated from move_cursor for single step */
    size_t line_idx = document_get_line_of_offset(self->doc, ref_cur->cursor_offset);
    size_t line_start = document_get_offset_of_line(self->doc, line_idx);
    size_t char_idx = ref_cur->cursor_offset - line_start;
    
    char *text = NULL; size_t len;
    PangoLayout *layout = create_pango_layout_for_line(self, line_idx, &text, &len);
    if (!layout) return;
    
    double target_x = ref_cur->target_x;
    if (target_x < 0) {
         PangoRectangle strong_pos;
         pango_layout_get_cursor_pos(layout, (int)MIN(char_idx, len), &strong_pos, NULL);
         target_x = pango_units_to_double(strong_pos.x);
         /* Update reference cursor too so it remembers x for subsequent moves */
         ref_cur->target_x = target_x;
    }
    
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
    
    if (!found_v_line) current_v_line_idx = MAX(0, pango_layout_get_line_count(layout) - 1);
    
    size_t new_offset = ref_cur->cursor_offset; /* Fallback */
    
    int local_delta = visual_lines_delta;
    size_t current_line_idx = line_idx;
    
    /* Loop to find target line */
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
                 size_t next_line = current_line_idx + 1;
                 if (next_line >= document_get_line_count(self->doc)) {
                     /* EOF reached, clamp to end of last line */
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
                 if (current_line_idx == 0) {
                     /* BOF reached */
                     iter = pango_layout_get_iter(layout);
                     PangoLayoutLine *v_line = pango_layout_iter_get_line_readonly(iter);
                     int index, trailing;
                     pango_layout_line_x_to_index(v_line, (int)(target_x * PANGO_SCALE), &index, &trailing);
                     size_t ls = document_get_offset_of_line(self->doc, current_line_idx);
                     new_offset = ls + index + trailing;
                     pango_layout_iter_free(iter);
                     break;
                 }
                 current_line_idx--;
                 g_object_unref(layout); g_free(text);
                 layout = create_pango_layout_for_line(self, current_line_idx, &text, &len);
                 current_v_line_idx = pango_layout_get_line_count(layout) - 1; 
            }
        }
    }
    g_object_unref(layout);
    g_free(text);
    
    /* Add the new cursor */
    /* Check if duplicate before adding? editor_widget_add_cursor has toggle logic. 
       If we land on existing, it deletes it. 
       Check if new_offset matches any existing. */
    gboolean exists = FALSE;
    for (guint i=0; i<self->cursors->len; i++) {
        EditorCursor *c = &g_array_index(self->cursors, EditorCursor, i);
        if (c->cursor_offset == new_offset) { exists = TRUE; break; }
    }
    
    if (!exists) {
        editor_widget_add_cursor(self, new_offset);
        /* Set target_x for the new cursor */
        /* It is now at the end of array */
        EditorCursor *new_cur = &g_array_index(self->cursors, EditorCursor, self->cursors->len - 1);
        new_cur->target_x = target_x;
    }
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
    /* Collapse to single cursor for complex move operations */
    if (self->cursors && self->cursors->len > 1) editor_widget_clear_cursors(self);
    
    size_t total = document_get_length(self->doc);
    size_t s = MIN(self->cursor_offset, self->selection_anchor);
    size_t e = MAX(self->cursor_offset, self->selection_anchor);
    
    /* If no selection, auto-select word at caret precisely as in swap_words */
    if (s == e) {
        self->alt_word_mode = TRUE;
        size_t off = self->cursor_offset;
        if (off > 0 && !is_alt_word_char_at(self->doc, off) && is_alt_word_char_at(self->doc, utf8_prev_grapheme(self, off))) {
            off = utf8_prev_grapheme(self, off);
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
                
                document_begin_undo_group(self->doc);
                /* Save actual cursor state (e.g. collapsed) */
                document_set_undo_group_selection(self->doc, self->selection_anchor, self->cursor_offset);
                document_delete(self->doc, s, (e - s) + gap_len);
                document_insert(self->doc, s, gap_text, gap_len);
                document_insert(self->doc, s + gap_len, sel_text, e - s);
                
                size_t new_anchor = s + gap_len;
                size_t new_cursor = new_anchor + (e - s);
                document_set_redo_group_selection(self->doc, new_anchor, new_cursor);
                document_end_undo_group(self->doc);
                
                self->selection_anchor = new_anchor;
                self->cursor_offset = new_cursor;
                g_free(gap_text);
            }
        } else {
            if (s > 0) {
                size_t prev_gap = utf8_prev_grapheme(self, s);
                size_t gap_len = s - prev_gap;
                char *gap_text = document_get_text_range(self->doc, prev_gap, gap_len);
                
                document_begin_undo_group(self->doc);
                /* Save actual cursor state */
                document_set_undo_group_selection(self->doc, self->selection_anchor, self->cursor_offset);
                document_delete(self->doc, prev_gap, gap_len + (e - s));
                document_insert(self->doc, prev_gap, sel_text, e - s);
                document_insert(self->doc, prev_gap + (e - s), gap_text, gap_len);
                
                size_t new_anchor = prev_gap;
                size_t new_cursor = new_anchor + (e - s);
                document_set_redo_group_selection(self->doc, new_anchor, new_cursor);
                document_end_undo_group(self->doc);
                
                self->selection_anchor = new_anchor;
                self->cursor_offset = new_cursor;
                g_free(gap_text);
            }
        }
        g_free(sel_text);
        update_target_x(self);
        
        /* Sync primary cursor */
        EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);
        primary->cursor_offset = self->cursor_offset;
        primary->selection_anchor = self->selection_anchor;
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
        
        document_begin_undo_group(self->doc);
        /* Save actual cursor state */
        document_set_undo_group_selection(self->doc, self->selection_anchor, self->cursor_offset);
        document_delete(self->doc, s, sel_len + sep_len + w2_len);
        document_insert(self->doc, s, w2_text, w2_len);
        document_insert(self->doc, s + w2_len, sep_text, sep_len);
        document_insert(self->doc, s + w2_len + sep_len, sel_text, sel_len);
        
        size_t new_anchor = s + w2_len + sep_len;
        size_t new_cursor = new_anchor + sel_len;
        document_set_redo_group_selection(self->doc, new_anchor, new_cursor);
        document_end_undo_group(self->doc);
        
        self->selection_anchor = new_anchor;
        self->cursor_offset = new_cursor;
        
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
        
        document_begin_undo_group(self->doc);
        /* Save actual cursor state */
        document_set_undo_group_selection(self->doc, self->selection_anchor, self->cursor_offset);
        document_delete(self->doc, w2_start, w2_len + sep_len + sel_len);
        document_insert(self->doc, w2_start, sel_text, sel_len);
        document_insert(self->doc, w2_start + sel_len, sep_text, sep_len);
        document_insert(self->doc, w2_start + sel_len + sep_len, w2_text, w2_len);
        
        size_t new_anchor = w2_start;
        size_t new_cursor = w2_start + sel_len;
        document_set_redo_group_selection(self->doc, new_anchor, new_cursor);
        document_end_undo_group(self->doc);
        
        self->selection_anchor = new_anchor;
        self->cursor_offset = new_cursor;
        
        g_free(sel_text); g_free(sep_text); g_free(w2_text);
    }
    update_target_x(self);
    /* Sync primary cursor */
    EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);
    primary->cursor_offset = self->cursor_offset;
    primary->selection_anchor = self->selection_anchor;
}

static void
editor_widget_move_lines_vertically(EditorWidget *self, int delta)
{
    /* Collapse to single cursor for complex move operations */
    if (self->cursors && self->cursors->len > 1) editor_widget_clear_cursors(self);

    size_t start_off = MIN(self->cursor_offset, self->selection_anchor);
    size_t end_off = MAX(self->cursor_offset, self->selection_anchor);
    
    size_t start_line = document_get_line_of_offset(self->doc, start_off);
    size_t end_line = document_get_line_of_offset(self->doc, end_off);
    
    /* If selection ends exactly at start of next line, don't include that line */
    if (end_off > start_off && end_off == document_get_offset_of_line(self->doc, end_line) && end_line > start_line) {
        end_line--;
    }
    
    size_t total_lines = document_get_line_count(self->doc);
    document_begin_undo_group(self->doc);
    /* Save selection state (moved lines) */
    document_set_undo_group_selection(self->doc, self->selection_anchor, self->cursor_offset);
    
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
        
        self->selection_anchor = prev_start;
        self->cursor_offset = prev_start + range_len + (range_needs_newline ? 1 : 0);
        
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
        
        self->selection_anchor = range_start + next_len + (next_needs_newline ? 1 : 0);
        self->cursor_offset = self->selection_anchor + range_len;
        
        g_free(range_text);
        g_free(next_text);
    }
    
    document_end_undo_group(self->doc);
    
    update_target_x(self);
    /* Sync primary cursor */
    EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);
    primary->cursor_offset = self->cursor_offset;
    primary->selection_anchor = self->selection_anchor;
}



static gboolean
get_cursor_screen_coordinates(EditorWidget *self, double *out_x, double *out_y)
{
    if (!self->doc || !self->vadjustment) return FALSE;
    
    double scroll_y = gtk_adjustment_get_value(self->vadjustment);
    size_t start_line = (size_t)(scroll_y / self->line_height);
    size_t cursor_line = document_get_line_of_offset(self->doc, self->cursor_offset);
    
    /* If cursor is far before start_line, return FALSE (off screen) */
    if (cursor_line < start_line) return FALSE;
    
    /* Iterate from start_line to find cursor line Y */
    double current_y = 0;
    
    /* Initial offset from smooth scroll */
    if (start_line < document_get_line_count(self->doc)) {
         double fraction = fmod(scroll_y, self->line_height) / self->line_height;
         char *text; size_t len;
         PangoLayout *layout = create_pango_layout_for_line(self, start_line, &text, &len);
         if (layout) {
             int h; pango_layout_get_pixel_size(layout, NULL, &h);
             double layout_h = (double)h;
             if (layout_h < self->line_height) layout_h = self->line_height;
             current_y = -(fraction * layout_h);
             g_object_unref(layout);
             g_free(text);
         }
    }
    
    size_t line = start_line;
    double page_height = gtk_adjustment_get_page_size(self->vadjustment);
    
    while (line <= cursor_line) {
        if (current_y > page_height) return FALSE; /* Cursor below viewport */
        
        char *text; size_t len;
        PangoLayout *layout = create_pango_layout_for_line(self, line, &text, &len);
        if (!layout) break;
        
        int h;
        pango_layout_get_pixel_size(layout, NULL, &h);
        double layout_h = (double)h;
        if (layout_h < self->line_height) layout_h = self->line_height;
        
        if (line == cursor_line) {
            /* Found cursor line. Calculate X and Y within line */
            size_t line_start = document_get_offset_of_line(self->doc, line);
            size_t idx = self->cursor_offset - line_start;
            PangoRectangle pos;
            pango_layout_get_cursor_pos(layout, MIN((int)idx, (int)len), &pos, NULL);
            
            if (out_x) *out_x = pango_units_to_double(pos.x) + self->padding_left;
            if (out_y) *out_y = current_y + pango_units_to_double(pos.y) + self->padding_top;
            
            g_object_unref(layout);
            g_free(text);
            return TRUE;
        }
        
        current_y += layout_h;
        g_object_unref(layout);
        g_free(text);
        line++;
    }
    
    return FALSE; /* Not found visible */
}

/* Indentation helpers */
static int compare_size_t(gconstpointer a, gconstpointer b) {
    size_t sa = *(const size_t*)a;
    size_t sb = *(const size_t*)b;
    if (sa < sb) return -1;
    if (sa > sb) return 1;
    return 0;
}

static void
editor_widget_indent_selection(EditorWidget *self)
{
    if (!self->doc || !self->cursors) return;

    /* Collect all unique lines to indent */
    GArray *lines = g_array_new(FALSE, FALSE, sizeof(size_t));
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        
        size_t start_line = document_get_line_of_offset(self->doc, start);
        size_t end_line = document_get_line_of_offset(self->doc, end);
        
        if (end > start && end == document_get_offset_of_line(self->doc, end_line)) {
            if (end_line > start_line) end_line--;
        }
        
        for (size_t l = start_line; l <= end_line; l++) {
            g_array_append_val(lines, l);
        }
    }
    
    if (lines->len == 0) {
        g_array_free(lines, TRUE);
        return;
    }
    
    g_array_sort(lines, compare_size_t);
    
    /* Remove duplicates */
    for (guint i = 0; i < lines->len - 1; ) {
        size_t l1 = g_array_index(lines, size_t, i);
        size_t l2 = g_array_index(lines, size_t, i+1);
        if (l1 == l2) {
            g_array_remove_index(lines, i+1);
        } else {
            i++;
        }
    }
    
    /* Prepare indentation string */
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
    
    document_begin_undo_group(self->doc);
    
    /* Process descending to keep line indices valid during insertion? 
       Actually line indices stay valid if we insert characters, as newline count doesn't change.
       Wait, document_get_offset_of_line uses line structure. 
       Inserting non-newlines doesn't change line mapping usually.
       But sorting descending is safer for offsets if we tracked offsets.
       Since we use get_offset_of_line REPEATEDLY, checking line index is safe assuming line count constant.
    */
    
    for (int i = lines->len - 1; i >= 0; i--) {
        size_t line_idx = g_array_index(lines, size_t, i);
        size_t line_start = document_get_offset_of_line(self->doc, line_idx);
        
        document_insert(self->doc, line_start, indent_str, indent_len);
        
        /* Update all cursors after this point */
        for (guint c = 0; c < self->cursors->len; c++) {
            EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
            /* If cursor is at or after insertion, shift it */
            if (cur->cursor_offset >= line_start) cur->cursor_offset += indent_len;
            if (cur->selection_anchor >= line_start) cur->selection_anchor += indent_len;
        }
    }
    
    document_end_undo_group(self->doc);
    g_array_free(lines, TRUE);
    g_free(indent_str);
}

static void
editor_widget_unindent_selection(EditorWidget *self)
{
    if (!self->doc || !self->cursors) return;

    /* Collect all unique lines to unindent */
    GArray *lines = g_array_new(FALSE, FALSE, sizeof(size_t));
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        
        size_t start_line = document_get_line_of_offset(self->doc, start);
        size_t end_line = document_get_line_of_offset(self->doc, end);
        
        if (end > start && end == document_get_offset_of_line(self->doc, end_line)) {
            if (end_line > start_line) end_line--;
        }
        
        for (size_t l = start_line; l <= end_line; l++) {
            g_array_append_val(lines, l);
        }
    }
    
    if (lines->len == 0) {
        g_array_free(lines, TRUE);
        return;
    }
    
    g_array_sort(lines, compare_size_t);
    /* Remove duplicates */
    for (guint i = 0; i < lines->len - 1; ) {
        size_t l1 = g_array_index(lines, size_t, i);
        size_t l2 = g_array_index(lines, size_t, i+1);
        if (l1 == l2) {
            g_array_remove_index(lines, i+1);
        } else {
            i++;
        }
    }
    
    document_begin_undo_group(self->doc);
    
    for (int i = lines->len - 1; i >= 0; i--) {
        size_t line_idx = g_array_index(lines, size_t, i);
        size_t line_off = document_get_offset_of_line(self->doc, line_idx);
        
        size_t len;
        char *line_text = document_get_line(self->doc, line_idx, &len);
        
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
            
            /* Update cursors */
            for (guint c = 0; c < self->cursors->len; c++) {
                EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                if (cur->cursor_offset > line_off) {
                    cur->cursor_offset -= MIN(cur->cursor_offset - line_off, delete_len);
                }
                if (cur->selection_anchor > line_off) {
                    cur->selection_anchor -= MIN(cur->selection_anchor - line_off, delete_len);
                }
            }
        }
    }
    
    document_end_undo_group(self->doc);
    g_array_free(lines, TRUE);
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

    if (self->is_dragging_selection && (keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R)) {
        self->drag_copy_mode = TRUE;
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return TRUE;
    }
    
    switch (keyval) {
        case GDK_KEY_Escape:
            /* standard Reset: remove all extra cursors */
            if (self->cursors->len > 1) {
                g_array_set_size(self->cursors, 1);
                /* Cache is already primary (index 0), so no change needed unless primary changed? */
                /* Primary is index 0. We removed 1..N. Primary valid. Cache valid. */
                gtk_widget_queue_draw(GTK_WIDGET(self));
            }
            break;
        case GDK_KEY_Up:
            if (state & GDK_ALT_MASK) {
                if (state & GDK_SHIFT_MASK) {
                    editor_widget_add_cursor_vertically(self, -1);
                } else {
                    editor_widget_move_lines_vertically(self, -1);
                }
            } else {
                self->alt_word_mode = FALSE;
                move_cursor(self, -1);
                if (!(state & GDK_SHIFT_MASK)) {
                    for (guint c = 0; c < self->cursors->len; c++) {
                        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                        cur->selection_anchor = cur->cursor_offset;
                    }
                }
            }
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Down:
            if (state & GDK_ALT_MASK) {
                if (state & GDK_SHIFT_MASK) {
                    editor_widget_add_cursor_vertically(self, 1);
                } else {
                    editor_widget_move_lines_vertically(self, 1);
                }
            } else {
                self->alt_word_mode = FALSE;
                move_cursor(self, 1);
                if (!(state & GDK_SHIFT_MASK)) {
                     for (guint c = 0; c < self->cursors->len; c++) {
                         EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                         cur->selection_anchor = cur->cursor_offset;
                     }
                }
            }
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Left:
            if (state & GDK_ALT_MASK) {
                editor_widget_move_selection_horizontally(self, -1);
            } else {
                 self->alt_word_mode = FALSE;
                 for (guint c = 0; c < self->cursors->len; c++) {
                     EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                     if (state & GDK_CONTROL_MASK) {
                         cur->cursor_offset = word_prev(self, cur->cursor_offset);
                     } else {
                         cur->cursor_offset = utf8_prev_grapheme(self, cur->cursor_offset);
                     }
                     if (!(state & GDK_SHIFT_MASK)) cur->selection_anchor = cur->cursor_offset;
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
                 for (guint c = 0; c < self->cursors->len; c++) {
                     EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                     if (state & GDK_CONTROL_MASK) {
                         cur->cursor_offset = word_next(self, cur->cursor_offset);
                     } else {
                         cur->cursor_offset = utf8_next_grapheme(self, cur->cursor_offset);
                     }
                     if (!(state & GDK_SHIFT_MASK)) cur->selection_anchor = cur->cursor_offset;
                 }
                 update_target_x(self);
            }
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Home:
        {
            for (guint c = 0; c < self->cursors->len; c++) {
                 EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                 if (state & GDK_CONTROL_MASK) {
                     cur->cursor_offset = 0;
                 } else {
                     size_t line = document_get_line_of_offset(self->doc, cur->cursor_offset);
                     cur->cursor_offset = document_get_offset_of_line(self->doc, line);
                 }
                 if (!(state & GDK_SHIFT_MASK)) cur->selection_anchor = cur->cursor_offset;
            }
            update_target_x(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        }
        case GDK_KEY_End:
        {
            for (guint c = 0; c < self->cursors->len; c++) {
                 EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                 if (state & GDK_CONTROL_MASK) {
                     cur->cursor_offset = document_get_length(self->doc);
                 } else {
                     size_t line = document_get_line_of_offset(self->doc, cur->cursor_offset);
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
                     cur->cursor_offset = start + real_len;
                 }
                 if (!(state & GDK_SHIFT_MASK)) cur->selection_anchor = cur->cursor_offset;
            }
            
            update_target_x(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        }
        case GDK_KEY_Page_Up:
        {
             /* Simplified multi-cursor page up: move all by page size */
             double page_px = (self->vadjustment) ? gtk_adjustment_get_page_size(self->vadjustment) : 200;
             if (self->vadjustment) {
                 double current = gtk_adjustment_get_value(self->vadjustment);
                 gtk_adjustment_set_value(self->vadjustment, current - page_px);
             }
             int page_lines = (int)(page_px / self->line_height);
             if (page_lines < 1) page_lines = 1;
             move_cursor(self, -page_lines);
             
             if (!(state & GDK_SHIFT_MASK)) {
                 for (guint c = 0; c < self->cursors->len; c++) {
                     EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                     cur->selection_anchor = cur->cursor_offset;
                 }
             }
             scroll_to_cursor(self);
             gtk_widget_queue_draw(GTK_WIDGET(self));
             break;
        }
        case GDK_KEY_Page_Down:
        {
             double page_px = (self->vadjustment) ? gtk_adjustment_get_page_size(self->vadjustment) : 200;
             if (self->vadjustment) {
                 double current = gtk_adjustment_get_value(self->vadjustment);
                 gtk_adjustment_set_value(self->vadjustment, current + page_px);
             }
             int page_lines = (int)(page_px / self->line_height);
             if (page_lines < 1) page_lines = 1;
             move_cursor(self, page_lines);
             
             if (!(state & GDK_SHIFT_MASK)) {
                 for (guint c = 0; c < self->cursors->len; c++) {
                     EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                     cur->selection_anchor = cur->cursor_offset;
                 }
             }
             scroll_to_cursor(self);
             gtk_widget_queue_draw(GTK_WIDGET(self));
             break;   
        }
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
             /* Enter key */
             /* Insert newline at all cursors */
             document_begin_undo_group(self->doc);
             g_array_sort(self->cursors, compare_cursors_desc);
             for (guint c = 0; c < self->cursors->len; c++) {
                 EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                 size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
                 size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
                 
                 /* Auto-indentation logic (per cursor) */
                 char *indent = NULL;
                 if (self->auto_indent) {
                     size_t line = document_get_line_of_offset(self->doc, start);
                     size_t line_len;
                     char *text = document_get_line(self->doc, line, &line_len);
                     if (text) {
                         size_t i = 0;
                         while (i < line_len && (text[i] == ' ' || text[i] == '\t')) i++;
                         if (i > 0) indent = g_strndup(text, i);
                         g_free(text);
                     }
                 }

                 if (start != end) {
                     document_delete(self->doc, start, end-start);
                     cur->cursor_offset = start;
                 }
                 document_insert(self->doc, cur->cursor_offset, "\n", 1);
                 cur->cursor_offset++;
                 
                 if (indent) {
                     size_t len = strlen(indent);
                     document_insert(self->doc, cur->cursor_offset, indent, len);
                     cur->cursor_offset += len;
                     g_free(indent);
                 }
                 
                 cur->selection_anchor = cur->cursor_offset;
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
                for (guint c = 0; c < self->cursors->len; c++) {
                    EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                    if (cur->cursor_offset != cur->selection_anchor) {
                        any_selection = TRUE;
                        break;
                    }
                }
                
                if (any_selection) {
                    editor_widget_indent_selection(self);
                } else {
                    document_begin_undo_group(self->doc);
                    g_array_sort(self->cursors, compare_cursors_desc);
                    
                    char *text_to_insert;
                    char *spaces = NULL;
                    size_t insert_len;
                    
                    if (self->indent_style == 0) {
                        spaces = g_malloc(self->indent_width + 1);
                        memset(spaces, ' ', self->indent_width);
                        spaces[self->indent_width] = '\0';
                        text_to_insert = spaces;
                        insert_len = self->indent_width;
                    } else {
                        text_to_insert = "\t";
                        insert_len = 1;
                    }
                    
                    for (guint c = 0; c < self->cursors->len; c++) {
                        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                        document_insert(self->doc, cur->cursor_offset, text_to_insert, insert_len);
                        cur->cursor_offset += insert_len;
                        cur->selection_anchor = cur->cursor_offset;
                    }
                    
                    if (spaces) g_free(spaces);
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
             if (state & GDK_CONTROL_MASK) {
                 /* Ctrl+Backspace: Delete Word */
                 document_begin_undo_group(self->doc);
                 g_array_sort(self->cursors, compare_cursors_desc);
                 for (guint c = 0; c < self->cursors->len; c++) {
                     EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                     size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
                     size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
                     if (start != end) {
                         document_delete(self->doc, start, end-start);
                         cur->cursor_offset = start; cur->selection_anchor = start;
                     } else {
                         size_t prev = word_prev(self, cur->cursor_offset);
                         if (prev < cur->cursor_offset) {
                             document_delete(self->doc, prev, cur->cursor_offset - prev);
                             cur->cursor_offset = prev; cur->selection_anchor = prev;
                         }
                     }
                 }
                 document_end_undo_group(self->doc);
                 editor_widget_update_adjustments(self, -1, -1);
                 scroll_to_cursor(self);
             } else {
                 editor_widget_backspace(self);
             }
             gtk_widget_queue_draw(GTK_WIDGET(self));
             break;
        case GDK_KEY_Delete:
             if (state & GDK_CONTROL_MASK) {
                 /* Ctrl+Delete: Delete Word Next */
                 document_begin_undo_group(self->doc);
                 g_array_sort(self->cursors, compare_cursors_desc);
                 for (guint c = 0; c < self->cursors->len; c++) {
                     EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                     size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
                     size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
                     if (start != end) {
                         document_delete(self->doc, start, end-start);
                         cur->cursor_offset = start; cur->selection_anchor = start;
                     } else {
                         size_t next = word_next(self, cur->cursor_offset);
                         if (next > cur->cursor_offset) {
                             document_delete(self->doc, cur->cursor_offset, next - cur->cursor_offset);
                         }
                     }
                 }
                 document_end_undo_group(self->doc);
                 editor_widget_update_adjustments(self, -1, -1);
                 scroll_to_cursor(self);
             } else {
                 editor_widget_delete(self);
             }
             gtk_widget_queue_draw(GTK_WIDGET(self));
             break;
        case GDK_KEY_z:
            if (state & GDK_CONTROL_MASK) {
                 UndoInfo info = document_undo(self->doc);
                 if (info.success) {
                     /* Collapse to single cursor at undo point to avoid multi-cursor desync */
                     editor_widget_clear_cursors(self);
                     EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);
                     
                     if (info.has_selection) {
                         /* Restore saved selection state faithfully */
                         primary->selection_anchor = info.selection_start;
                         primary->cursor_offset = info.selection_end;
                     } else if (info.is_insert) {
                         /* Text restored/inserted: Select it */
                         primary->cursor_offset = info.start + info.length;
                         primary->selection_anchor = info.start;
                     } else {
                         /* Text deleted: Place cursor at start */
                         primary->cursor_offset = info.start;
                         primary->selection_anchor = info.start;
                     }
                     /* Sync cache */
                     self->cursor_offset = primary->cursor_offset;
                     self->selection_anchor = primary->selection_anchor;
                     
                     editor_widget_reset_cursor_blink(self);
                     editor_widget_update_adjustments(self, -1, -1);
                     scroll_to_cursor(self);
                 }
                 gtk_widget_queue_draw(GTK_WIDGET(self));
            }
            break;
        case GDK_KEY_y:
            if (state & GDK_CONTROL_MASK) {
                 UndoInfo info = document_redo(self->doc);
                 if (info.success) {
                     /* Collapse to single cursor */
                     editor_widget_clear_cursors(self);
                     EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);

                     if (info.has_selection) {
                         /* Restore saved selection state (e.g. from Alt+Arrow Redo) */
                         /* User requested to KEEP selection on Redo */
                         primary->selection_anchor = info.selection_start;
                         primary->cursor_offset = info.selection_end;
                     } else if (info.is_insert) {
                         primary->cursor_offset = info.start + info.length;
                         primary->selection_anchor = info.start;
                     } else {
                         primary->cursor_offset = info.start;
                         primary->selection_anchor = info.start;
                     }
                     /* Sync cache */
                     self->cursor_offset = primary->cursor_offset;
                     self->selection_anchor = primary->selection_anchor;
                     
                     editor_widget_reset_cursor_blink(self);
                     editor_widget_update_adjustments(self, -1, -1);
                     scroll_to_cursor(self);
                 }
                 gtk_widget_queue_draw(GTK_WIDGET(self));
            }
            break;
        case GDK_KEY_c:
            if (state & GDK_CONTROL_MASK) {
                editor_widget_copy(self);
            } else {
                handled = FALSE;
            }
            break;
        case GDK_KEY_x:
            if (state & GDK_CONTROL_MASK) {
                editor_widget_cut(self);
            } else {
                handled = FALSE;
            }
            break;
        case GDK_KEY_v:
            if (state & GDK_CONTROL_MASK) {
                editor_widget_paste(self);
            } else {
                handled = FALSE;
            }
            break;
        case GDK_KEY_a:
            if (state & GDK_CONTROL_MASK) {
                /* Select all - anchor at end so Shift+Click reduces from start */
                /* Select all: Anchor at 0 (start), Cursor at length (end) */
                editor_widget_clear_cursors(self);
                EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);
                
                size_t total = document_get_length(self->doc);
                primary->selection_anchor = 0;
                primary->cursor_offset = total;
                
                /* Sync cache */
                self->cursor_offset = primary->cursor_offset;
                self->selection_anchor = primary->selection_anchor;
                
                self->alt_word_mode = TRUE; /* Treat as auto-selection */
                scroll_to_cursor(self);
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
    if (!self->doc || !self->cursors) return;
    
    size_t len = strlen(str);
    if (len == 0) return;

    document_begin_undo_group(self->doc);
    g_array_sort(self->cursors, compare_cursors_desc);
    
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        
        long delta = 0;
        
        /* If replacing selection */
        if (start != end) {
            document_delete(self->doc, start, end - start);
            /* Current cursor collapses to start */
            cur->cursor_offset = start;
            /* Delta is negative length of deletion */
            delta -= (long)(end - start);
        }
        
        /* Insert new text */
        document_insert(self->doc, cur->cursor_offset, str, len);
        cur->cursor_offset += len;
        cur->selection_anchor = cur->cursor_offset;
        delta += (long)len;
        
        /* Propagate offset change to all previously processed cursors (indices 0..c-1)
           Since we sort descending, these cursors are at higher offsets than current 'start'.
        */
        if (delta != 0) {
            for (guint j = 0; j < c; j++) {
                EditorCursor *prev = &g_array_index(self->cursors, EditorCursor, j);
                /* Using signed math for delta */
                if ((long)prev->cursor_offset + delta < 0) prev->cursor_offset = 0;
                else prev->cursor_offset += delta;
                
                if ((long)prev->selection_anchor + delta < 0) prev->selection_anchor = 0;
                else prev->selection_anchor += delta;
            }
        }
    }
    document_end_undo_group(self->doc);
    
    self->alt_word_mode = FALSE;
    
    editor_widget_reset_cursor_blink(self);
    editor_widget_update_adjustments(self, -1, -1);
    scroll_to_cursor(self); /* Syncs cache and updates IM location */
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
editor_widget_update_im_cursor_location(EditorWidget *self)
{
    if (!self->im_context || !self->doc) return;

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
    
    /* Calculate elapsed time in milliseconds */
    gint64 elapsed_ms = (now - self->cursor_blink_start_time) / 1000;
    
    /* Simple on/off blink: 500ms on, 500ms off (no fade animation) */
    /* This is much more efficient as we only redraw when state changes */
    double new_alpha = ((elapsed_ms / 500) % 2 == 0) ? 1.0 : 0.0;
    
    /* Only redraw if alpha actually changed (state transition) */
    gboolean need_redraw = (new_alpha != self->cursor_alpha);
    self->cursor_alpha = new_alpha;
    
    if (need_redraw) {
        gtk_widget_queue_draw(widget);
    }
    
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
                
                size_t sel_start = MIN(self->cursor_offset, self->selection_anchor);
                size_t sel_end = MAX(self->cursor_offset, self->selection_anchor);

                if (drop_off >= sel_start && drop_off < sel_end) {
                    self->drag_drop_offset = (size_t)-1;
                } else {
                    self->drag_drop_offset = drop_off;
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
    if (self->cursors) g_array_free(self->cursors, TRUE);
    if (self->idle_resize_id) {
        g_source_remove(self->idle_resize_id);
        self->idle_resize_id = 0;
    }
    
    /* Disconnect GSettings signal and cleanup */
    if (self->interface_settings) {
        g_signal_handlers_disconnect_by_func(self->interface_settings, on_system_font_changed, self);
        g_clear_object(&self->interface_settings);
    }
    
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
    
    self->line_y_offsets = g_array_new(FALSE, FALSE, sizeof(double));
    self->cursors = g_array_new(FALSE, FALSE, sizeof(EditorCursor));
    editor_widget_add_cursor(self, 0);
    
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
    self->doc = doc;
    /* Force clear all cursors including the default one from init */
    if (self->cursors) g_array_set_size(self->cursors, 0);
    editor_widget_add_cursor(self, 0);
    
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

Document *
editor_widget_get_document(EditorWidget *self)
{
    g_return_val_if_fail(EDITOR_IS_WIDGET(self), NULL);
    return self->doc;
}
