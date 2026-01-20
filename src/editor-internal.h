#pragma once

#include "editor-widget.h"
#include <gtk/gtk.h>
#include "syntax.h"

#define MAX_PANGO_LINE_LEN 10485760

/* EditorCursor struct */
typedef struct {
    size_t cursor_offset;
    size_t selection_anchor;
    double target_x;
} EditorCursor;

enum {
    CARET_MOVED,
    CURSOR_MOVED,
    INSERT_MODE_CHANGED,
    LAST_SIGNAL
};

extern guint editor_signals[LAST_SIGNAL];

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

    gboolean alt_word_mode; /* TRUE if selection was auto-created for word swapping */

    /* Advanced Drag and Drop */
    double drag_x, drag_y;
    size_t drag_drop_offset;
    gboolean drag_copy_mode;
    PangoLayout *drag_ghost_layout;
    gboolean is_dnd_active; /* TRUE only after passing 8px threshold */
    gboolean is_drag_gesture_active; /* TRUE whenever mouse is dragging (selection or DnD) */
    
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
    
    gboolean insert_mode;
    
    /* Cached scroll upper bound (recalculated only when dimensions change) */
    double cached_scroll_upper;
    int cached_width;
    int cached_height;
    size_t cached_line_count;
    
    /* Cache for Y positions of all lines (accumulated height) */
    GArray *line_y_offsets;
    
    /* Idle resize handler to prevent blocking UI on every frame */
    guint idle_resize_id;
    
    /* Search State */
    GArray *search_matches; /* Array of SearchMatch */
    int current_match_idx;

#include "compact-matches.h"

    /* Filter State */
    CompactMatches *filtered_lines; /* MMap storage of physical line indices */
    char *filter_pattern;
    GRegex *filter_regex_pattern;
    gboolean filter_case_sensitive;
    gboolean filter_is_regex;

    
    /* Statistical Scroll for Large Files */
    double avg_visual_lines;
    
    /* System font monitoring */
    GSettings *interface_settings;
};

/* Internal function declarations */
void move_cursor(EditorWidget *self, int visual_lines_delta);
void scroll_to_cursor(EditorWidget *self);
void update_target_x(EditorWidget *self);
void editor_widget_clear_cursors(EditorWidget *self);
void editor_widget_add_cursor(EditorWidget *self, size_t offset);
void editor_widget_add_cursor_vertically(EditorWidget *self, int visual_lines_delta);
void editor_widget_move_lines_vertically(EditorWidget *self, int delta);
void editor_widget_move_selection_horizontally(EditorWidget *self, int delta);
void editor_widget_get_offset_at_point(EditorWidget *self, double x, double y, size_t *out_offset);
void update_selection_extension(EditorWidget *self, size_t off);
void find_line_at_offset(Document *doc, size_t offset, size_t *line_start, size_t *line_end);
void editor_widget_drag_drop_finish(EditorWidget *self, size_t drop_off);
void start_autoscroll(EditorWidget *self, int direction, double speed);
void stop_autoscroll(EditorWidget *self);
size_t editor_widget_delete_selection(EditorWidget *self);
int compare_cursors_desc(gconstpointer a, gconstpointer b);
size_t word_prev(EditorWidget *self, size_t offset);
size_t word_next(EditorWidget *self, size_t offset);
size_t utf8_prev_grapheme(EditorWidget *self, size_t offset);
size_t utf8_next_grapheme(EditorWidget *self, size_t offset);
void editor_widget_find_word_boundary(EditorWidget *self, size_t offset, size_t *word_start, size_t *word_end);
void editor_widget_copy(EditorWidget *self);
void editor_widget_cut(EditorWidget *self);
void editor_widget_paste(EditorWidget *self);
void editor_widget_paste_primary(EditorWidget *self);
void editor_widget_backspace(EditorWidget *self);
void editor_widget_delete(EditorWidget *self);
void editor_widget_indent_selection(EditorWidget *self);
void editor_widget_unindent_selection(EditorWidget *self);
void editor_widget_update_adjustments(EditorWidget *self, int widget_width, int widget_height);
void editor_widget_reset_cursor_blink(EditorWidget *self);
void editor_widget_update_im_cursor_location(EditorWidget *self);
size_t get_visual_line_count(EditorWidget *self);
size_t get_physical_line_index(EditorWidget *self, size_t visual_line_idx);
double get_effective_gutter_width(EditorWidget *self);
void editor_widget_ensure_metrics(EditorWidget *self);
void editor_widget_get_cursor_position(EditorWidget *self, size_t *line, size_t *col);
EditorCursor *editor_widget_get_primary_cursor(EditorWidget *self);
gboolean is_alt_word_char_at(Document *doc, size_t offset);

/* Pango layout helper from editor-scrolling.c needed by utils/others */
PangoLayout *create_pango_layout_for_line(EditorWidget *self, size_t line_idx, char **out_text, size_t *out_len);

void editor_widget_snapshot(GtkWidget *widget, GtkSnapshot *snapshot);


gboolean editor_on_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data);

void editor_input_init_controllers(EditorWidget *self);
