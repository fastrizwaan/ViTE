#ifndef EDITOR_INTERNAL_H
#define EDITOR_INTERNAL_H

#include <gtk/gtk.h>
#include "editor-widget.h"
#include "document.h"
#include "syntax.h"
#include "compact-matches.h"

#define MAX_PANGO_LINE_LEN 10485760

/* Structure Definitions */

enum {
    CARET_MOVED,
    CURSOR_MOVED,
    INSERT_MODE_CHANGED,
    UNDO_REDO_PROGRESS,
    LAST_SIGNAL
};

extern guint editor_signals[LAST_SIGNAL];

typedef struct {
    size_t cursor_offset;
    size_t selection_anchor;
    double target_x; /* -1 if needs update */
} EditorCursor;

typedef struct {
    size_t start_line;
    size_t end_line;
} FoldRange;

struct _EditorWidget {
    GtkWidget parent_instance;

    /* Core */
    Document *doc;
    SyntaxContext *syntax_ctx;
    
    /* Adjustments & Scrolling */
    GtkAdjustment *hadjustment;
    GtkAdjustment *vadjustment;
    GtkScrollablePolicy hscroll_policy;
    GtkScrollablePolicy vscroll_policy;

    /* Graphics & Metrics */
    PangoFontDescription *font_desc;
    char *font_name;
    gboolean use_custom_font;
    int font_zoom_steps;
    GSettings *interface_settings;
    
    GdkRGBA color_text;
    GdkRGBA color_cursor;
    
    double line_height;
    double ascent;
    double cached_char_width;
    double avg_visual_lines;
    
    int padding_left;
    int padding_top;

    /* Caches */
    GArray *line_y_offsets; /* double */
    GArray *cursors; /* EditorCursor */
    
    /* Primary cursor cache */
    size_t cursor_offset;
    size_t selection_anchor;
    double target_x;

    /* Input State */
    GtkIMContext *im_context;
    gboolean insert_mode;
    
    /* Typing Grouping State */
    gboolean typing_undo_group_active;
    gboolean last_char_was_separator;

    /* Drag & Selection State */
    gboolean is_dragging_selection;
    size_t drag_start_offset;
    
    gboolean is_drag_gesture_active;
    gboolean is_dnd_active;
    gboolean drag_copy_mode;
    size_t drag_drop_offset;
    PangoLayout *drag_ghost_layout;
    
    double drag_x;
    double drag_y;

    /* Multi-click */
    gboolean multi_click_selection;
    int multi_click_mode; /* 2=word, 3=line */
    size_t multi_click_start;
    size_t multi_click_end;
    gboolean alt_word_mode;

    /* Search */
    GArray *search_matches;          /* Viewport matches for rendering */
    int current_match_idx;           /* Index in viewport matches (for rendering, deprecated) */
    SearchTask *active_search;       /* Global search reference for navigation */
    size_t global_match_idx;         /* Position in ALL matches, not just viewport */
    size_t current_match_offset;     /* Start offset of current match for highlight comparison */

    /* Filter */
    CompactMatches *filtered_lines;
    char *filter_pattern;
    GRegex *filter_regex_pattern;
    gboolean filter_case_sensitive;
    gboolean filter_is_regex;
    
    /* Folding */
    GArray *fold_ranges;       /* FoldRange */
    GArray *fold_collapsed;    /* size_t start_line */
    GArray *collapsed_ranges;  /* FoldRange merged, for visibility checks */
    CompactMatches *visible_lines; /* Visible physical lines after filter+fold */

    /* Config */
    gboolean enable_folding;
    gboolean show_line_numbers;
    gboolean highlight_current_line;
    gboolean show_right_margin;
    int right_margin_position;
    gboolean wrap_lines;
    gboolean auto_indent;
    int indent_style;
    int tab_width;
    int indent_width;

    /* Minimap */
    gboolean minimap_enabled;
    double minimap_width;
    int minimap_block_height;
    gboolean minimap_active; /* Dragging minimap? */
    double drag_start_scroll; /* For relative dragging */
    double drag_ratio;        /* Cached ratio for stable dragging */

    /* Animation / Timer */
    double cursor_alpha;
    gint64 cursor_blink_start_time;
    guint cursor_blink_tick_id;

    guint autoscroll_timer_id;
    int autoscroll_direction;
    double autoscroll_speed;
    
    
    guint autoscroll_tick_count;
    guint idle_resize_id;
    guint syntax_scan_idle_id; /* For background full-document scanning */
    
    gboolean last_theme_dark_mode; /* Tracking for syntax cache invalidation */
    
    /* Async Undo/Redo */
    UndoRedoTask *undo_redo_task; /* Active undo/redo operation */
    
    /* UI State */
    gboolean mouse_in_gutter;
};

/* Helper Functions */
void editor_widget_find_word_boundary(EditorWidget *self, size_t offset, size_t *word_start, size_t *word_end);
void editor_widget_find_segment_boundary(EditorWidget *self, size_t offset, size_t *start, size_t *end);
size_t word_next(EditorWidget *self, size_t offset);
size_t word_prev(EditorWidget *self, size_t offset);
size_t word_end_next(EditorWidget *self, size_t offset);
size_t word_start_or_prev_end_left(EditorWidget *self, size_t offset);
gboolean is_alt_word_char_at(Document *doc, size_t offset);

double get_effective_gutter_width(EditorWidget *self);
void find_line_at_offset(Document *doc, size_t offset, size_t *line_start, size_t *line_end);
PangoLayout *create_pango_layout_for_line(EditorWidget *self, size_t line_idx, char **out_text, size_t *out_len);
void update_target_x(EditorWidget *self);
void editor_widget_add_cursor_vertically(EditorWidget *self, int visual_lines_delta);

/* Input */
void editor_input_init_controllers(EditorWidget *self);
void editor_widget_update_im_cursor_location(EditorWidget *self);
void editor_widget_on_undo_redo_progress(double progress, gboolean finished, UndoInfo *info, gpointer user_data);
void editor_widget_on_change_case_progress(int processed, int total, gboolean finished, gpointer user_data);
void move_cursor(EditorWidget *self, int visual_lines);

/* Selection */
void editor_widget_get_offset_at_point(EditorWidget *self, double x, double y, size_t *out_offset);
void update_selection_extension(EditorWidget *self, size_t off);
EditorCursor *editor_widget_get_primary_cursor(EditorWidget *self);
void editor_widget_clear_cursors(EditorWidget *self);
void editor_widget_add_cursor(EditorWidget *self, size_t offset);

/* Scrolling */
void scroll_to_cursor(EditorWidget *self);
void start_autoscroll(EditorWidget *self, int direction, double speed);
void stop_autoscroll(EditorWidget *self);
gboolean editor_on_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data);

/* Actions */
void editor_widget_move_lines_vertically(EditorWidget *self, int direction);
void editor_widget_move_selection_horizontally(EditorWidget *self, int direction);
void editor_widget_delete(EditorWidget *self);
void editor_widget_backspace(EditorWidget *self);
void editor_widget_indent_selection(EditorWidget *self);
void editor_widget_unindent_selection(EditorWidget *self);
/* Clipboard */
gboolean editor_widget_copy(EditorWidget *self);
void editor_widget_cut(EditorWidget *self);
void editor_widget_paste_primary(EditorWidget *self);
void editor_widget_paste(EditorWidget *self);
size_t editor_widget_delete_selection(EditorWidget *self);
void editor_widget_undo(EditorWidget *self);
void editor_widget_redo(EditorWidget *self);
void editor_widget_select_all(EditorWidget *self);
void editor_widget_change_case(EditorWidget *self, int type);
void editor_widget_finish_typing_undo_group(EditorWidget *self);

void editor_widget_update_adjustments(EditorWidget *self, int width, int height);
void editor_widget_update_search_viewport(EditorWidget *self);

void editor_widget_reset_cursor_blink(EditorWidget *self);
int compare_cursors_desc(gconstpointer a, gconstpointer b);

/* Metrics & Layout */
void editor_widget_ensure_metrics(EditorWidget *self);
size_t get_visual_line_count(EditorWidget *self);
size_t get_physical_line_index(EditorWidget *self, size_t visual_line_idx);

/* Helpers */
size_t utf8_next_grapheme(EditorWidget *self, size_t offset);
size_t utf8_prev_grapheme(EditorWidget *self, size_t offset);
void editor_widget_ensure_syntax_state_up_to(EditorWidget *self, size_t target_line);
void editor_widget_rebuild_folding(EditorWidget *self);
void editor_widget_rebuild_visible_lines(EditorWidget *self);
gboolean editor_widget_toggle_fold(EditorWidget *self, size_t line_idx);
gboolean editor_widget_get_fold_range(EditorWidget *self, size_t line_idx, FoldRange *out_range);
gboolean editor_widget_is_fold_collapsed(EditorWidget *self, size_t line_idx);
gboolean editor_widget_get_visual_line_for_physical(EditorWidget *self, size_t phys_line, size_t *out_visual);
gboolean editor_widget_get_next_visible_line(EditorWidget *self, size_t phys_line, size_t *out_line);
gboolean editor_widget_get_prev_visible_line(EditorWidget *self, size_t phys_line, size_t *out_line);
double editor_widget_get_fold_gutter_width(EditorWidget *self);
gboolean editor_widget_ensure_line_visible(EditorWidget *self, size_t phys_line);
void editor_widget_get_visible_line_range(EditorWidget *self, size_t *start, size_t *end);

/* Rendering */
void editor_widget_snapshot(GtkWidget *widget, GtkSnapshot *snapshot);

#endif
