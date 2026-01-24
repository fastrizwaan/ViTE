#include "editor-internal.h"
#include "syntax.h"
#include <math.h>

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
                                 GValue       *return_value,
                                 guint         n_param_values,
                                 const GValue *param_values,
                                 gpointer      invocation_hint,
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
  callback = (GMarshalFunc_VOID__UINT_UINT) (marshal_data ? marshal_data : cc->callback);

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










static void
on_system_font_changed(GSettings *settings, const char *key, gpointer user_data)
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
cursor_blink_tick_callback(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data)
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
    
    if (self->doc) {
        document_remove_content_callback(self->doc, on_doc_content_changed, self);
        self->doc = NULL;
    }
    
    if (self->syntax_ctx) {
        syntax_context_free(self->syntax_ctx);
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
    
    if (self->cursor_blink_tick_id) {
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->cursor_blink_tick_id);
        self->cursor_blink_tick_id = 0;
    }
    if (self->autoscroll_timer_id) {
        g_source_remove(self->autoscroll_timer_id);
        self->autoscroll_timer_id = 0;
    }

    G_OBJECT_CLASS(editor_widget_parent_class)->dispose(object);
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
    EditorWidget *self = EDITOR_WIDGET(widget);
    editor_widget_ensure_metrics(self);

    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        *minimum = 100;
        *natural = 400; 
    } else {
        *minimum = 100;
        *natural = 400;
    }
}

static void
editor_widget_size_allocate (GtkWidget *widget,
                             int        width,
                             int        height,
                             int        baseline)
{
    EditorWidget *self = EDITOR_WIDGET(widget);
    editor_widget_update_adjustments(self, width, height);
}

static void
editor_widget_init(EditorWidget *self)
{
    /* Initialize custom font name to default (used when custom font is enabled) */
    self->font_name = g_strdup("Monospace 11");
    self->use_custom_font = FALSE;
    self->insert_mode = TRUE;
    
    /* Monitor system font changes from GNOME settings */
    self->interface_settings = g_settings_new("org.gnome.desktop.interface");
    g_signal_connect(self->interface_settings, "changed::monospace-font-name", 
                     G_CALLBACK(on_system_font_changed), self);
    
    /* Initialize font_desc to NULL; ensure_metrics will set it based on use_custom_font */
    self->font_desc = NULL;
    
    self->line_y_offsets = g_array_new(FALSE, FALSE, sizeof(double));
    self->cursors = g_array_new(FALSE, FALSE, sizeof(EditorCursor));
    self->search_matches = NULL;
    self->current_match_idx = -1;
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
    
    gtk_widget_set_cursor_from_name(GTK_WIDGET(self), "text");

    /* Initialize input controllers */
    editor_input_init_controllers(self);
    
    self->syntax_ctx = syntax_context_new();
    
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);

    /* Filter initialization */
    self->filtered_lines = NULL;
    self->filter_pattern = NULL;
    self->filter_regex_pattern = NULL;
    self->filter_case_sensitive = FALSE;
    self->filter_is_regex = FALSE;
    self->avg_visual_lines = 1.0;
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

static void
on_doc_content_changed(Document *doc, void *user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    /* Content changed externally (e.g. from another view). Queue redraw. */
    /* Update adjustments (size might have changed) */
    editor_widget_update_adjustments(self, -1, -1);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void
editor_widget_set_document(EditorWidget *self, Document *doc)
{
    if (self->doc) {
        document_remove_content_callback(self->doc, on_doc_content_changed, self);
    }
    self->doc = doc;
    if (self->doc) {
        document_add_content_callback(self->doc, on_doc_content_changed, self);
    }
    /* Force clear all cursors including the default one from init */
    if (self->cursors) g_array_set_size(self->cursors, 0);
    editor_widget_add_cursor(self, 0);
    
    /* Removed large file auto-disable. Relying on efficient O(N) linear scan now. */
    
    /* Update adjustments (with current size) */
    editor_widget_update_adjustments(self, -1, -1);
    gtk_widget_queue_draw(GTK_WIDGET(self));
    
    /* Perform Initial Syntax Scan (Warm-up)
       Scan up to 20,000 lines to ensure multi-line constructs are valid immediately.
       State-only scan (compute_attributes=FALSE) is extremely fast (O(N)).
    */
    if (self->syntax_ctx && self->doc) {
        size_t total = document_get_line_count(self->doc);
        size_t limit = MIN(total, 20000);
        for (size_t i = 0; i < limit; i++) {
             size_t len;
             char *text = document_get_line_truncated(self->doc, i, &len, MAX_PANGO_LINE_LEN);
             if (text) {
                 /* Basic validation not strictly needed for state machine but good for safety */
                 size_t tlen = len;
                 while (tlen > 0 && (text[tlen-1] == '\n' || text[tlen-1] == '\r')) tlen--;
                 text[tlen] = '\0';
                 syntax_process_line(self->syntax_ctx, i, text, FALSE);
                 g_free(text);
             }
        }
    }
}

void
editor_widget_set_language(EditorWidget *self, const char *lang)
{
    if (self->syntax_ctx) {
        syntax_context_set_language(self->syntax_ctx, lang);
        
        /* Re-scan on language change too */
        if (self->doc) {
            syntax_context_invalidate_all(self->syntax_ctx);
            size_t total = document_get_line_count(self->doc);
            size_t limit = MIN(total, 20000);
            for (size_t i = 0; i < limit; i++) {
                 size_t len;
                 char *text = document_get_line_truncated(self->doc, i, &len, MAX_PANGO_LINE_LEN);
                 if (text) {
                     size_t tlen = len;
                     while (tlen > 0 && (text[tlen-1] == '\n' || text[tlen-1] == '\r')) tlen--;
                     text[tlen] = '\0';
                     syntax_process_line(self->syntax_ctx, i, text, FALSE);
                     g_free(text);
                 }
            }
        }
        
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
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
    
    FileEncoding enc = ENCODING_UTF8;
    if (g_strcmp0(encoding_id, "utf-16le") == 0) enc = ENCODING_UTF16LE;
    else if (g_strcmp0(encoding_id, "utf-16be") == 0) enc = ENCODING_UTF16BE;
    
    document_set_encoding(self->doc, enc);
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
        gtk_widget_queue_draw(GTK_WIDGET(self));
    }
}

gboolean
editor_widget_get_word_wrap(EditorWidget *self)
{
    g_return_val_if_fail(EDITOR_IS_WIDGET(self), FALSE);
    return self->wrap_lines;
}

