#include <gtk/gtk.h>
#include "editor-widget.h"
#include "editor-internal.h"
#include "editor-minimap.h"
#include "syntax.h"
#include "syntax-internal.h"
#include <math.h>
#include <adwaita.h>
#include "settings.h"

#define MAX_PANGO_LINE_LEN 10485760 /* 10MB limit for single line rendering to avoid int overflow/crash */

static void editor_widget_scrollable_init (GtkScrollableInterface *iface);

static void on_doc_content_changed(Document *doc, void *user_data);

/* Note: on_doc_content_changed defined later, used later */

/* Helper to get gutter width based on settings */


G_DEFINE_TYPE_WITH_CODE (EditorWidget, editor_widget, GTK_TYPE_WIDGET,
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE, editor_widget_scrollable_init))

/* Marshaler for (uint, uint) */
static void
_editor_marshal_VOID__UINT_UINT (GClosure     *closure,
                                 GValue       *return_value G_GNUC_UNUSED,
                                 guint         n_param_values,
                                 const GValue *param_values,
                                 gpointer      invocation_hint G_GNUC_UNUSED,
                                 gpointer      marshal_data)
{
  typedef void (*GMarshalFunc_VOID__UINT_UINT) (gpointer     data1,
                                                guint        arg_1,
                                                guint        arg_2,
                                                gpointer     data2);
  GMarshalFunc_VOID__UINT_UINT callback;
  GCClosure *cc = (GCClosure*) closure;
  gpointer data1, data2;

  g_return_if_fail (n_param_values == 3);

  if (G_CCLOSURE_SWAP_DATA (closure))
    {
      data1 = closure->data;
      data2 = g_value_peek_pointer (param_values + 0);
    }
  else
    {
      data1 = g_value_peek_pointer (param_values + 0);
      data2 = closure->data;
    }
  union {
    gpointer p;
    GMarshalFunc_VOID__UINT_UINT f;
  } u;
  u.p = (marshal_data ? marshal_data : cc->callback);
  callback = u.f;

  callback (data1,
            g_value_get_uint (param_values + 1),
            g_value_get_uint (param_values + 2),
            data2);
}

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
    PROP_ENABLE_FOLDING,
    PROP_MINIMAP_ENABLED,
    N_PROPS
};

guint editor_signals[LAST_SIGNAL] = { 0 };



void
editor_widget_set_show_line_numbers(EditorWidget *self, gboolean show)
{
    g_return_if_fail(EDITOR_IS_WIDGET(self));
    if (self->show_line_numbers != show) {
        self->show_line_numbers = show;
        gtk_widget_queue_resize(GTK_WIDGET(self));
        g_object_notify(G_OBJECT(self), "show-line-numbers");
    }
}

gboolean
editor_widget_get_show_line_numbers(EditorWidget *self)
{
    g_return_val_if_fail(EDITOR_IS_WIDGET(self), TRUE);
    return self->show_line_numbers;
}





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



/* UTF-8 grapheme cluster navigation helpers */

/* Move cursor right by one grapheme cluster */











static gboolean
syntax_scan_step(gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->doc || !self->syntax_ctx) {
        self->syntax_scan_idle_id = 0;
        return G_SOURCE_REMOVE;
    }
    
    size_t processed = syntax_get_processed_line_count(self->syntax_ctx);
    size_t total = document_get_line_count(self->doc);
    
    if (processed >= total) {
        /* Done */
        self->syntax_scan_idle_id = 0;
        gtk_widget_queue_draw(GTK_WIDGET(self));
        return G_SOURCE_REMOVE;
    }
    
    size_t batch = 50000;
    size_t limit = MIN(total, processed + batch);
    
    #define SCAN_BUF_SIZE 65536
    char *buf = g_malloc(SCAN_BUF_SIZE + 1);
    
    DocumentIter iter;
    document_iter_init(self->doc, &iter, processed);
    
    size_t current_offset = document_get_offset_of_line(self->doc, processed);
    
    for (size_t i = processed; i < limit; i++) {
        size_t len = document_iter_next_line(&iter, buf, SCAN_BUF_SIZE);
        size_t orig_len = len;
        
        buf[MIN(len, SCAN_BUF_SIZE)] = '\0';
        
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) len--;
        buf[len] = '\0';
        
        SyntaxState old_state = STATE_ROOT;
        if (i < self->syntax_ctx->state_chain->len) {
            old_state = self->syntax_ctx->state_chain->data[i];
        }
        
        syntax_process_line(self->syntax_ctx, i, current_offset, buf, FALSE);
        current_offset += orig_len;
             
             /* Mark this line as valid */
             self->syntax_ctx->valid_up_to = i + 1;

        SyntaxState new_state = STATE_ROOT;
        if (i < self->syntax_ctx->state_chain->len) {
            new_state = self->syntax_ctx->state_chain->data[i];
        }
             
             /* If state hasn't changed, and we have valid states ahead, we can stop early! */
        if (old_state == new_state && i < self->syntax_ctx->state_chain->len - 1) {
            self->syntax_ctx->valid_up_to = total;
            self->syntax_scan_idle_id = 0;
            gtk_widget_queue_draw(GTK_WIDGET(self));
            g_free(buf);
            return G_SOURCE_REMOVE;
        }
    }
    
    g_free(buf);
    
    /* Continue idle loop */
    gtk_widget_queue_draw(GTK_WIDGET(self));
    return G_SOURCE_CONTINUE;
}

static void
on_document_edit(Document *doc G_GNUC_UNUSED, size_t offset, int64_t delta_len, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    
    if (self->syntax_ctx) {
        syntax_context_apply_byte_edit(self->syntax_ctx, offset, delta_len);
    }
    
    if (self->active_search) {
        document_search_task_apply_edit(self->active_search, offset, delta_len);
    }
    
    if (!self->search_matches || self->search_matches->len == 0) return;
    
    if (delta_len > 0) {
        /* Insertion */
        for (guint i = 0; i < self->search_matches->len; i++) {
             SearchMatch *m = &g_array_index(self->search_matches, SearchMatch, i);
             
             /* If insertion happens strictly BEFORE the match, shift it. */
             /* If insertion happens AT the start of the match (offset == m->start), 
                we usually consider this "before" and shift the match right. */
             if (m->start >= offset) {
                 m->start += delta_len;
                 m->end += delta_len;
             } 
             /* If insertion happens strictly INSIDE the match (offset > m->start && offset < m->end),
                block is broken. Remove it. */
             else if (m->end > offset) {
                 g_array_remove_index(self->search_matches, i);
                 i--; 
             }
        }
    } else {
        /* Deletion (delta_len is negative) */
        size_t abs_delta = (size_t)(-delta_len);
        size_t del_end = offset + abs_delta;
        
        for (guint i = 0; i < self->search_matches->len; i++) {
             SearchMatch *m = &g_array_index(self->search_matches, SearchMatch, i);
             
             /* Check overlap */
             /* Match is [start, end). Deletion is [offset, del_end). */
             
             if (m->end <= offset) {
                 /* Match is totally before deletion. Unaffected. */
                 continue;
             }
             
             if (m->start >= del_end) {
                 /* Match is totally after deletion. Shift down. */
                 m->start -= abs_delta;
                 m->end -= abs_delta;
                 continue;
             }
             
             /* Overlap detected: Match is damaged. Remove. */
             g_array_remove_index(self->search_matches, i);
             i--;
        }
    }
    
    /* Queue redraw to remove old highlights / show shifted ones */
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
shift_collapsed_folds(EditorWidget *self, size_t start_line, int line_delta)
{
    if (!self->fold_collapsed || self->fold_collapsed->len == 0) return;
    if (line_delta == 0) return;

    if (line_delta < 0) {
        size_t removed = (size_t)(-line_delta);
        size_t del_end = start_line + removed;
        for (gint i = (gint)self->fold_collapsed->len - 1; i >= 0; i--) {
            size_t v = g_array_index(self->fold_collapsed, size_t, i);
            if (v >= start_line && v < del_end) {
                g_array_remove_index(self->fold_collapsed, i);
            } else if (v >= del_end) {
                v -= removed;
                g_array_index(self->fold_collapsed, size_t, i) = v;
            }
        }
    } else {
        for (guint i = 0; i < self->fold_collapsed->len; i++) {
            size_t v = g_array_index(self->fold_collapsed, size_t, i);
            if (v >= start_line) {
                v += (size_t)line_delta;
                g_array_index(self->fold_collapsed, size_t, i) = v;
            }
        }
    }
}

static gboolean
on_fold_rebuild_timeout(gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    self->fold_rebuild_idle_id = 0;
    editor_widget_rebuild_folding(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    return G_SOURCE_REMOVE;
}

static void
on_document_update(Document *doc G_GNUC_UNUSED, size_t start_line, int line_delta, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->syntax_ctx) return;

    /* Efficiently shift the syntax cache and invalidate states from start_line onwards.
       This resets valid_up_to to start_line. */
    syntax_context_apply_edit(self->syntax_ctx, start_line, line_delta);
    
    /* LAZY UPDATE: 
       We do NOT scan the whole file here. It's O(N) and causes freezing on large files.
       Instead, we rely on 'editor_widget_ensure_syntax_state_up_to' in the renderer 
       to catch up the state for the visible area. 
    */

    /* Remove idle scanner if it was running (optional, but cleaner to let renderer drive) */
    if (self->syntax_scan_idle_id) {
        g_source_remove(self->syntax_scan_idle_id);
        self->syntax_scan_idle_id = 0;
    }

    shift_collapsed_folds(self, start_line, line_delta);
    
    /* Synchronously update visible lines array length so renderer doesn't crash */
    editor_widget_rebuild_visible_lines(self);

    if (self->fold_rebuild_idle_id) {
        g_source_remove(self->fold_rebuild_idle_id);
    }
    /* Debounce folding rebuild to avoid freezing on rapid typing */
    self->fold_rebuild_idle_id = g_timeout_add(250, on_fold_rebuild_timeout, self);

    if (self->line_y_offsets) g_array_set_size(self->line_y_offsets, 0);
    editor_widget_update_adjustments(self, -1, -1);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
on_system_font_changed(GSettings *settings G_GNUC_UNUSED, const char *key G_GNUC_UNUSED, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    if (!self->use_custom_font) {
        self->line_height = 0; /* Force re-calculation */
        editor_widget_ensure_metrics(self);
        gtk_widget_queue_resize(GTK_WIDGET(self));
    }
}

void
editor_widget_reset_cursor_blink(EditorWidget *self)
{
    self->cursor_blink_start_time = g_get_monotonic_time();
    self->cursor_alpha = 1.0;
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static gboolean
cursor_blink_tick_callback(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data G_GNUC_UNUSED)
{
    EditorWidget *self = EDITOR_WIDGET(widget);
    if (!gtk_widget_has_focus(widget)) return G_SOURCE_CONTINUE;

    gint64 now = gdk_frame_clock_get_frame_time(frame_clock);
    if (self->cursor_blink_start_time == 0) self->cursor_blink_start_time = now;
    
    double elapsed = (now - self->cursor_blink_start_time) / 1000000.0;
    gboolean visible = ((int)(elapsed * 2) % 2) == 0;
    
    double target = visible ? 1.0 : 0.0;
    if (ABS(self->cursor_alpha - target) > 0.01) {
        self->cursor_alpha = target;
        gtk_widget_queue_draw(widget);
    }
    
    return G_SOURCE_CONTINUE;
}

static void
editor_widget_dispose(GObject *object)
{
    EditorWidget *self = EDITOR_WIDGET(object);

    if (self->hadjustment) { g_object_unref(self->hadjustment); self->hadjustment = NULL; }
    if (self->vadjustment) { g_object_unref(self->vadjustment); self->vadjustment = NULL; }
    
    /* Cancel all pending timer/idle sources FIRST to prevent UAF callbacks */
    if (self->autoscroll_timer_id) {
        g_source_remove(self->autoscroll_timer_id);
        self->autoscroll_timer_id = 0;
    }
    if (self->idle_resize_id) {
        g_source_remove(self->idle_resize_id);
        self->idle_resize_id = 0;
    }
    if (self->cursor_blink_tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->cursor_blink_tick_id);
        self->cursor_blink_tick_id = 0;
    }
    if (self->syntax_scan_idle_id) {
        g_source_remove(self->syntax_scan_idle_id);
        self->syntax_scan_idle_id = 0;
    }
    
    if (self->doc) {
        document_remove_content_callback(self->doc, on_doc_content_changed, self);
        document_remove_update_callback(self->doc, on_document_update, self);
        document_remove_edit_callback(self->doc, on_document_edit, self);
        document_free(self->doc);
        self->doc = NULL;
    }
    
    if (self->syntax_ctx) {
        syntax_context_unref(self->syntax_ctx);
        self->syntax_ctx = NULL;
    }

    if (self->cursors) {
        g_array_unref(self->cursors);
        self->cursors = NULL;
    }
    if (self->line_y_offsets) {
        g_array_unref(self->line_y_offsets);
        self->line_y_offsets = NULL;
    }
    if (self->search_matches) {
        g_array_unref(self->search_matches);
        self->search_matches = NULL;
    }
    if (self->filtered_lines) {
        compact_matches_free(self->filtered_lines);
        self->filtered_lines = NULL;
    }
    if (self->visible_lines) {
        compact_matches_free(self->visible_lines);
        self->visible_lines = NULL;
    }
    if (self->fold_ranges) {
        g_array_unref(self->fold_ranges);
        self->fold_ranges = NULL;
    }
    if (self->fold_collapsed) {
        g_array_unref(self->fold_collapsed);
        self->fold_collapsed = NULL;
    }
    if (self->collapsed_ranges) {
        g_array_unref(self->collapsed_ranges);
        self->collapsed_ranges = NULL;
    }
    
    g_free(self->filter_pattern);
    self->filter_pattern = NULL;
    if (self->filter_regex_pattern) {
        g_regex_unref(self->filter_regex_pattern);
        self->filter_regex_pattern = NULL;
    }
    
    if (self->interface_settings) { g_object_unref(self->interface_settings); self->interface_settings = NULL; }
    if (self->im_context) { g_object_unref(self->im_context); self->im_context = NULL; }
    
    if (self->font_desc) {
        pango_font_description_free(self->font_desc);
        self->font_desc = NULL;
    }
    g_free(self->font_name);
    self->font_name = NULL;

    G_OBJECT_CLASS(editor_widget_parent_class)->dispose(object);
}

static void
editor_widget_measure (GtkWidget      *widget,
                       GtkOrientation  orientation,
                       int             for_size G_GNUC_UNUSED,
                       int            *minimum,
                       int            *natural,
                       int            *minimum_baseline G_GNUC_UNUSED,
                       int            *natural_baseline G_GNUC_UNUSED)
{
    EditorWidget *self = EDITOR_WIDGET(widget);
    editor_widget_ensure_metrics(self);

    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        double gutter_width = get_effective_gutter_width(self);
        if (self->wrap_lines) {
            *minimum = (int)gutter_width;
            *natural = (int)gutter_width;
        } else {
            *minimum = (int)(gutter_width + self->cached_char_width * 10 + 20); /* 10 chars + padding */
            *natural = (int)(gutter_width + self->cached_char_width * 80 + 20); /* 80 chars + padding */
        }
    } else {
        /* Minimum height should be at least one line */
        *minimum = (int)(self->line_height + 20); /* One line + padding */
        
        /* Natural height should be multiple lines */
        *natural = (int)(self->line_height * 10 + 20); /* 10 lines + padding */
    }
}

static void
editor_widget_size_allocate (GtkWidget *widget,
                             int        width,
                             int        height,
                             int        baseline G_GNUC_UNUSED)
{
    EditorWidget *self = EDITOR_WIDGET(widget);
    editor_widget_update_adjustments(self, width, height);
    editor_widget_update_search_viewport(self);
    /* Recalibrate minimap on resize */
    gtk_widget_queue_draw(widget);
}

static void
editor_widget_map (GtkWidget *widget)
{
    /* Force LTR direction for code editing, regardless of system locale */
    gtk_widget_set_direction(widget, GTK_TEXT_DIR_LTR);
    GTK_WIDGET_CLASS(editor_widget_parent_class)->map(widget);
}

static void
editor_widget_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec G_GNUC_UNUSED)
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
            editor_widget_set_show_line_numbers(self, g_value_get_boolean(value));
            break;
        case PROP_HIGHLIGHT_CURRENT_LINE:
            self->highlight_current_line = g_value_get_boolean(value);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            g_object_notify(G_OBJECT(self), "highlight-current-line");
            break;
        case PROP_SHOW_RIGHT_MARGIN:
            self->show_right_margin = g_value_get_boolean(value);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            g_object_notify(G_OBJECT(self), "show-right-margin");
            break;
        case PROP_RIGHT_MARGIN_POSITION:
            self->right_margin_position = g_value_get_int(value);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            g_object_notify(G_OBJECT(self), "right-margin-position");
            break;
        case PROP_WRAP_LINES:
            editor_widget_set_word_wrap(self, g_value_get_boolean(value));
            break;
        case PROP_AUTO_INDENT:
            self->auto_indent = g_value_get_boolean(value);
            break;
        case PROP_INDENT_STYLE:
            if (self->indent_style != g_value_get_int(value)) {
                self->indent_style = g_value_get_int(value);
                g_object_notify(G_OBJECT(self), "indent-style");
            }
            break;
        case PROP_TAB_WIDTH:
            if (self->tab_width != g_value_get_int(value)) {
                self->tab_width = g_value_get_int(value);
                /* Assuming metrics or layout might depend on tab width, force redraw */
                gtk_widget_queue_draw(GTK_WIDGET(self));
                g_object_notify(G_OBJECT(self), "tab-width");
                /* If we controlled pango tabs, we'd update them here */
            }
            break;
        case PROP_INDENT_WIDTH:
            if (self->indent_width != g_value_get_int(value)) {
                self->indent_width = g_value_get_int(value);
                g_object_notify(G_OBJECT(self), "indent-width");
            }
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
            editor_widget_ensure_metrics(self);
            gtk_widget_queue_resize(GTK_WIDGET(self));
            break;
        case PROP_ENABLE_FOLDING:
            self->enable_folding = g_value_get_boolean(value);
            editor_widget_rebuild_folding(self); /* Rebuild or Clear */
            gtk_widget_queue_resize(GTK_WIDGET(self));
            break;
        case PROP_MINIMAP_ENABLED:
             self->minimap_enabled = g_value_get_boolean(value);
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
                            GParamSpec *pspec G_GNUC_UNUSED)
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
        case PROP_ENABLE_FOLDING:
            g_value_set_boolean(value, self->enable_folding);
            break;
        case PROP_MINIMAP_ENABLED:
            g_value_set_boolean(value, self->minimap_enabled);
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
    
    editor_signals[CARET_MOVED] = g_signal_new("caret-moved",
                                 G_TYPE_FROM_CLASS(klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL,
                                 NULL,
                                 G_TYPE_NONE, 0);

    /**
     * EditorWidget::cursor-moved:
     * @editor: the editor widget
     * @line: the new line index
     * @col: the new column index
     *
     * Emitted when the primary cursor moves.
     */
    editor_signals[CURSOR_MOVED] = g_signal_new("cursor-moved",
                                 G_TYPE_FROM_CLASS(klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL,
                                 _editor_marshal_VOID__UINT_UINT,
                                 G_TYPE_NONE,
                                 2,
                                 G_TYPE_UINT,
                                 G_TYPE_UINT);
    
    editor_signals[INSERT_MODE_CHANGED] = g_signal_new("insert-mode-changed",
                                 G_TYPE_FROM_CLASS(klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL,
                                 NULL,
                                 G_TYPE_NONE, 0);
    
    editor_signals[UNDO_REDO_PROGRESS] = g_signal_new("undo-redo-progress",
                                 G_TYPE_FROM_CLASS(klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL,
                                 NULL,
                                 G_TYPE_NONE,
                                 2,
                                 G_TYPE_DOUBLE,
                                 G_TYPE_BOOLEAN);

    widget_class->snapshot = editor_widget_snapshot;
    widget_class->map = editor_widget_map;
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
        
    g_object_class_install_property(object_class, PROP_ENABLE_FOLDING,
        g_param_spec_boolean("enable-folding", "Enable Code Folding", "Enable Code Folding", FALSE, G_PARAM_READWRITE));

    g_object_class_install_property(object_class, PROP_MINIMAP_ENABLED,
        g_param_spec_boolean("minimap-enabled", "Enable Minimap", "Enable Minimap", FALSE, G_PARAM_READWRITE));
}

static void
editor_widget_scrollable_init(GtkScrollableInterface *iface G_GNUC_UNUSED)
{
    /* properties handled */
}

/* Helper to ensure syntax state is valid up to a specific line (On-Demand Catch-up) */
void
editor_widget_ensure_syntax_state_up_to(EditorWidget *self, size_t target_line)
{
    if (!self->syntax_ctx || !self->doc || syntax_context_get_language(self->syntax_ctx) == LANG_NONE) return;
    
    size_t valid_up_to = syntax_get_processed_line_count(self->syntax_ctx);
    if (valid_up_to > target_line) return; /* Already valid */
    
    size_t total = document_get_line_count(self->doc);
    size_t limit = MIN(total, target_line + 1); /* Scan up to target_line inclusive */
    
    /* Use heap buffer to avoid stack overflow */
    #define SCAN_BUF_SIZE 65536
    char *buf = g_malloc(SCAN_BUF_SIZE + 1);
    
    DocumentIter iter;
    document_iter_init(self->doc, &iter, valid_up_to);
    
    size_t current_offset = document_get_offset_of_line(self->doc, valid_up_to);

    for (size_t i = valid_up_to; i < limit; i++) {
        /* Zero-Allocation fetch via Iterator (O(1) amortized) */
        size_t len = document_iter_next_line(&iter, buf, SCAN_BUF_SIZE);
        size_t orig_len = len;
        
        /* If line was longer than buffer, it's truncated. */
        buf[MIN(len, SCAN_BUF_SIZE)] = '\0';
        
        /* Strip newline chars for the scanner (it expects clean line) */
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
            buf[len-1] = '\0';
            len--;
        }

        /* FALSE = State Only (Fast Path). Pass len to avoid strlen. */
        syntax_process_line_len(self->syntax_ctx, i, current_offset, buf, len, FALSE);
        current_offset += orig_len;
    }
    g_free(buf);
    self->syntax_ctx->valid_up_to = limit;
}

static void
editor_widget_init(EditorWidget *self)
{
    ViteSettings *settings = settings_get();
    /* Initialize custom font name to default (used when custom font is enabled) */
    self->font_name = g_strdup(settings->font_name ? settings->font_name : "Monospace 11");
    self->use_custom_font = settings->use_custom_font;
    self->font_zoom_steps = 0;
    self->insert_mode = TRUE;
    self->enable_folding = settings->enable_folding;
    
    self->minimap_enabled = settings->minimap_enabled;
    self->minimap_width = 100.0;
    self->minimap_block_height = 2;
    self->minimap_active = FALSE;
    
    self->cursors = g_array_new(FALSE, FALSE, sizeof(EditorCursor));
    self->line_y_offsets = g_array_new(FALSE, FALSE, sizeof(double));
    
    self->has_bracket_match = FALSE;
    self->bracket_match_start = 0;
    self->bracket_match_end = 0;
    
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    
    /* Initialize Animation State */
    self->cursor_alpha = 1.0; /* Corrected member */
    self->cursor_blink_start_time = 0; /* Corrected member */
    self->cursor_blink_tick_id = 0;
    
    /* Create IM context */
    /* self->im_context handled in editor_input_init_controllers? No, typically created in widget. 
       Let's check input.c first. */
    

    /* Monitor system font changes from GNOME settings */
    self->interface_settings = g_settings_new("org.gnome.desktop.interface");
    g_signal_connect(self->interface_settings, "changed::monospace-font-name", 
                     G_CALLBACK(on_system_font_changed), self);
    
    /* Initialize font_desc to NULL; ensure_metrics will set it based on use_custom_font */
    self->font_desc = NULL;
    
    self->search_matches = NULL;
    self->current_match_idx = -1;
    editor_widget_add_cursor(self, 0);
    
    /* Initialize cursor blink animation */
    self->cursor_blink_tick_id = gtk_widget_add_tick_callback(
        GTK_WIDGET(self), cursor_blink_tick_callback, NULL, NULL);
    
    self->drag_drop_offset = (size_t)-1;
    self->drag_copy_mode = FALSE;
    self->drag_ghost_layout = NULL;
    
    self->autoscroll_timer_id = 0;
    self->autoscroll_direction = 0;
    self->autoscroll_speed = 0;
    
    self->syntax_scan_idle_id = 0;
    
    self->last_theme_dark_mode = -1; /* Force initial theme sync */
    self->last_theme_revision = 0;
    
    /* Default Theme Colors (will be updated by theme logic or CSS later) */
    /* Light theme defaults */
    self->color_background = (GdkRGBA){1.0, 1.0, 1.0, 1.0};
    self->color_text = (GdkRGBA){0.0, 0.0, 0.0, 1.0};
    self->color_cursor = (GdkRGBA){0.0, 0.0, 0.0, 1.0};
    self->color_line_highlight = (GdkRGBA){0.0, 0.0, 0.0, 0.05};
    self->color_line_number = (GdkRGBA){0.0, 0.0, 0.0, 0.5};
    self->color_gutter_bg = (GdkRGBA){0.95, 0.95, 0.95, 1.0};
    
    /* Viewport padding */
    self->padding_left = 4;
    self->padding_top = 8;
    
    /* Config defaults */
    self->show_line_numbers = settings->show_line_numbers;
    self->highlight_current_line = settings->highlight_current_line;
    self->show_right_margin = settings->show_right_margin;
    self->right_margin_position = settings->right_margin_position;
    self->wrap_lines = settings->wrap_lines;
    self->auto_indent = settings->auto_indent;
    self->indent_style = settings->indent_style; /* Space */
    self->tab_width = settings->tab_width;
    self->indent_width = settings->indent_width;
    
    gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "text");

    /* Initialize input controllers */
    editor_input_init_controllers(self);
    
    g_signal_connect(self, "cursor-moved", G_CALLBACK(editor_widget_update_bracket_match), NULL);
    
    self->syntax_ctx = syntax_context_new();
    
    /* Filter initialization */
    self->filtered_lines = NULL;
    self->filter_pattern = NULL;
    self->filter_regex_pattern = NULL;
    self->filter_case_sensitive = FALSE;
    self->filter_is_regex = FALSE;
    self->fold_ranges = NULL;
    self->fold_collapsed = NULL;
    self->collapsed_ranges = NULL;
    self->visible_lines = NULL;
    self->avg_visual_lines = 1.0;
}

GtkWidget *
editor_widget_new(void)
{
    return g_object_new(EDITOR_TYPE_WIDGET, NULL);
}

static void
on_doc_content_changed(Document *doc G_GNUC_UNUSED, void *user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    /* Content changed externally (e.g. from another view). Queue redraw. */
    /* Update adjustments (size might have changed) */
    editor_widget_update_adjustments(self, -1, -1);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    
    /* Syntax invalidation is handled by on_document_update now for efficiency */
}

void
editor_widget_set_document(EditorWidget *self, Document *doc)
{
    if (self->syntax_scan_idle_id) {
        g_source_remove(self->syntax_scan_idle_id);
        self->syntax_scan_idle_id = 0;
    }

    if (self->syntax_ctx) {
        syntax_context_invalidate_all(self->syntax_ctx);
    }

    editor_widget_clear_search(self);

    if (self->filtered_lines) {
        compact_matches_free(self->filtered_lines);
        self->filtered_lines = NULL;
    }
    g_free(self->filter_pattern);
    self->filter_pattern = NULL;
    if (self->filter_regex_pattern) {
        g_regex_unref(self->filter_regex_pattern);
        self->filter_regex_pattern = NULL;
    }
    self->filter_case_sensitive = FALSE;
    self->filter_is_regex = FALSE;

    if (self->line_y_offsets) g_array_set_size(self->line_y_offsets, 0);
    if (self->fold_ranges) g_array_set_size(self->fold_ranges, 0);
    if (self->fold_collapsed) g_array_set_size(self->fold_collapsed, 0);
    if (self->collapsed_ranges) g_array_set_size(self->collapsed_ranges, 0);

    if (self->doc) {
        document_remove_content_callback(self->doc, on_doc_content_changed, self);
        document_remove_update_callback(self->doc, on_document_update, self);
        document_remove_edit_callback(self->doc, on_document_edit, self);
        document_free(self->doc);
    }
    self->doc = doc ? document_ref(doc) : NULL;
    if (self->doc) {
        document_add_content_callback(self->doc, on_doc_content_changed, self);
        document_add_update_callback(self->doc, on_document_update, self);
        document_add_edit_callback(self->doc, on_document_edit, self);
    }
    /* Force clear all cursors including the default one from init */
    if (self->cursors) g_array_set_size(self->cursors, 0);
    editor_widget_add_cursor(self, 0);
    
    /* Removed large file auto-disable. Relying on efficient O(N) linear scan now. */
    
    /* Update adjustments (with current size) */
    editor_widget_rebuild_folding(self);
    editor_widget_update_adjustments(self, -1, -1);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    
    /* Perform Initial Syntax Scan (Warm-up)
       Scan up to 20,000 lines to ensure multi-line constructs are valid immediately.
       State-only scan (compute_attributes=FALSE) is extremely fast (O(N)).
    */
    /* Perform Initial Syntax Scan (Synchronous State-Only)
       User requirement: "Synchronized highlighting" for multi-line constructs.
       The fast-path scanner is benchmarked at ~6ms for 100k lines.
       We can safely scan the ENTIRE file state here to ensure jumping works immediately.
    */
    if (self->syntax_ctx && self->doc && syntax_context_get_language(self->syntax_ctx) != LANG_NONE) {
        size_t total = document_get_line_count(self->doc);
        
        /* Scan everything for state correctness (separate loop from rendering) */
        editor_widget_ensure_syntax_state_up_to(self, total - 1);
        
        /* No need for background scanner for state anymore, 
           unless we want to re-scan for very huge files (e.g. > 1M lines) strictly? 
           For now, let's rely on this sync scan as it's < 100ms for 1M lines. */
        if (self->syntax_scan_idle_id) {
             g_source_remove(self->syntax_scan_idle_id);
             self->syntax_scan_idle_id = 0;
        }
    }

}

void
editor_widget_set_language(EditorWidget *self, const char *lang)
{
    if (self->syntax_ctx) {
        /* DEBUG: Print language set */

        
        syntax_context_set_language(self->syntax_ctx, lang);
        
        /* FULL SYNCHRONOUS SCAN on language change. */
        if (self->doc && syntax_context_get_language(self->syntax_ctx) != LANG_NONE) {
            syntax_context_invalidate_all(self->syntax_ctx);
            
            size_t total = document_get_line_count(self->doc);
            
            /* Scan ENTIRE file for state */
            editor_widget_ensure_syntax_state_up_to(self, total - 1);

            /* Disable background scanner as state is fully valid now */
            if (self->syntax_scan_idle_id) {
                g_source_remove(self->syntax_scan_idle_id);
                self->syntax_scan_idle_id = 0;
            }
        } else {
             /* Language None: clear any scanner */
             if (self->syntax_scan_idle_id) {
                g_source_remove(self->syntax_scan_idle_id);
                self->syntax_scan_idle_id = 0;
            }
        }
        
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

SyntaxContext *
editor_widget_get_syntax_context(EditorWidget *self)
{
    return self->syntax_ctx;
}

void
editor_widget_set_syntax_context(EditorWidget *self, SyntaxContext *ctx)
{
    if (self->syntax_ctx == ctx) return;
    
    if (self->syntax_ctx) syntax_context_unref(self->syntax_ctx);
    self->syntax_ctx = ctx ? syntax_context_ref(ctx) : NULL;
    
    /* If we have a document, restart background scanning for this context in THIS view */
    if (self->syntax_ctx && self->doc) {
         if (self->syntax_scan_idle_id) g_source_remove(self->syntax_scan_idle_id);
         self->syntax_scan_idle_id = g_idle_add((GSourceFunc)syntax_scan_step, self);
    }
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
editor_widget_set_line_ending(EditorWidget *self, const char *line_ending_id)
{
    if (!self->doc) return;
    
    NewlineType type = NEWLINE_LF;
    if (g_strcmp0(line_ending_id, "crlf") == 0) type = NEWLINE_CRLF;
    else if (g_strcmp0(line_ending_id, "cr") == 0) type = NEWLINE_CR;
    
    document_set_newline_type(self->doc, type);
    /* No redraw needed usually, unless we visualize line endings, but we don't yet. */
}

void
editor_widget_set_encoding(EditorWidget *self, const char *encoding_id)
{
    if (!self->doc) return;

    document_set_encoding(self->doc, file_encoding_from_id(encoding_id));
}

const char *editor_widget_get_language_name(EditorWidget *self);

Document *
editor_widget_get_document(EditorWidget *self)
{
    g_return_val_if_fail(EDITOR_IS_WIDGET(self), NULL);
    return self->doc;
}

const char *
editor_widget_get_language_name(EditorWidget *self)
{
    g_return_val_if_fail(EDITOR_IS_WIDGET(self), "Plain Text");
    if (!self->syntax_ctx) return "Plain Text";
    return syntax_context_get_language_name(self->syntax_ctx);
}

/* Search Integration */




void
editor_widget_set_word_wrap(EditorWidget *self, gboolean wrap)
{
    g_return_if_fail(EDITOR_IS_WIDGET(self));
    if (self->wrap_lines != wrap) {
        self->wrap_lines = wrap;
        self->line_height = 0; /* Force metrics update if needed, or simply relayout */
        editor_widget_ensure_metrics(self); /* recalculate dependent metrics */
        
        /* Clear offsets to force full relayout */
        if (self->line_y_offsets) g_array_set_size(self->line_y_offsets, 0);
        
        editor_widget_update_adjustments(self, -1, -1);
        gtk_widget_queue_resize(GTK_WIDGET(self));
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

gboolean
editor_widget_get_word_wrap(EditorWidget *self)
{
    g_return_val_if_fail(EDITOR_IS_WIDGET(self), FALSE);
    return self->wrap_lines;
}

static void
editor_widget_apply_zoom(EditorWidget *self, int delta_steps, gboolean reset)
{
    g_return_if_fail(EDITOR_IS_WIDGET(self));

    if (reset) {
        self->font_zoom_steps = 0;
    } else {
        self->font_zoom_steps += delta_steps;
    }

    self->line_height = 0; /* Force metrics update */
    editor_widget_ensure_metrics(self);

    if (self->line_y_offsets) g_array_set_size(self->line_y_offsets, 0);
    editor_widget_update_adjustments(self, -1, -1);
    gtk_widget_queue_resize(GTK_WIDGET(self));
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
editor_widget_zoom_in(EditorWidget *self)
{
    editor_widget_apply_zoom(self, 1, FALSE);
}

void
editor_widget_zoom_out(EditorWidget *self)
{
    editor_widget_apply_zoom(self, -1, FALSE);
}

void
editor_widget_zoom_reset(EditorWidget *self)
{
    editor_widget_apply_zoom(self, 0, TRUE);
}

int
editor_widget_get_zoom_steps(EditorWidget *self)
{
    return self->font_zoom_steps;
}
