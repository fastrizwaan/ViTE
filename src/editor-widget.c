#include "editor-widget.h"
#include "syntax.h"

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
    
    /* Input */
    GtkIMContext *im_context;
    
    /* Syntax */
    SyntaxContext *syntax_ctx;
};

static void editor_widget_scrollable_init (GtkScrollableInterface *iface);
static void editor_widget_ensure_metrics(EditorWidget *self);

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
        
        /* Draw Line Background if selected */
        /* Selection rendering across lines is complex. 
           Simplified: If line is fully selected or partially.
        */
        
        /* Draw Selection */
        /* Simplistic selection: if line is within [anchor, cursor] range */
        size_t start_sel = MIN(self->cursor_offset, self->selection_anchor);
        size_t end_sel = MAX(self->cursor_offset, self->selection_anchor);
        size_t line_start_off = document_get_offset_of_line(self->doc, line_idx);
        size_t line_end_off = line_start_off + len; /* Not including newline for display usually? */
        
        /* Check overlap */
        if (start_sel < line_end_off && end_sel > line_start_off) {
            /* Intersection */
            size_t sel_in_line_start = MAX(start_sel, line_start_off) - line_start_off;
            size_t sel_in_line_end = MIN(end_sel, line_end_off) - line_start_off;
            
            /* Get pixel ranges from Pango */
            PangoRectangle r1, r2;
            pango_layout_index_to_pos(layout, sel_in_line_start, &r1);
            pango_layout_index_to_pos(layout, sel_in_line_end, &r2);
            
            /* Draw rectangle */
            double x = pango_units_to_double(r1.x);
            double w = pango_units_to_double(r2.x - r1.x);
            /* Handle RTL? r2.x might be < r1.x. Abs needed for width. */
            if (w < 0) { x += w; w = -w; }
            
            gtk_snapshot_append_color(snapshot, 
                                      &(GdkRGBA){0.2, 0.2, 0.6, 0.4}, /* Selection color hardcoded for now or derived? */
                                      &GRAPHENE_RECT_INIT(x, 0, w, self->line_height));
        }

        /* Calculate height of this line */
        int pixel_h;
        pango_layout_get_pixel_size(layout, NULL, &pixel_h);
        double layout_h = (double)pixel_h;
        if (layout_h < self->line_height) layout_h = self->line_height; /* Min height */

        gtk_snapshot_append_layout(snapshot, layout, &self->color_text);
        
        /* Draw cursor */
        if (line_idx == cursor_line) {
             size_t line_start_off = document_get_offset_of_line(self->doc, line_idx);
             if (self->cursor_offset >= line_start_off) {
                 size_t index_in_line = self->cursor_offset - line_start_off;
                 if (index_in_line > len) index_in_line = len;
                 
                 PangoRectangle strong_pos;
                 pango_layout_get_cursor_pos(layout, (int)index_in_line, &strong_pos, NULL);
                 
                 /* Cursor might be tall if wrapped? */
                 /* pango_layout_get_cursor_pos gives pos inside the layout logic */
                 
                 gtk_snapshot_append_color(snapshot, 
                                           &self->color_cursor,
                                           &GRAPHENE_RECT_INIT(
                                                pango_units_to_double(strong_pos.x),
                                                pango_units_to_double(strong_pos.y),
                                                2,
                                                pango_units_to_double(strong_pos.height)
                                           ));
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
            
            size_t line_start_off = document_get_offset_of_line(self->doc, line_idx);
            *out_offset = line_start_off + index;
            
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
    
    size_t off;
    editor_widget_get_offset_at_point(self, x, y, &off);
    
    self->cursor_offset = off;
    
    if (n_press == 2) {
        /* Double click select word - TODO */
    } else if (n_press == 3) {
        /* Triple click select line - TODO */
    } else {
        /* Single click reset selection */
        GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
        if (state & GDK_SHIFT_MASK) {
            /* Extend selection */
        } else {
            self->selection_anchor = off;
        }
    }
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data)
{
    EditorWidget *self = EDITOR_WIDGET(user_data);
    double start_x, start_y;
    gtk_gesture_drag_get_start_point(gesture, &start_x, &start_y);
    
    size_t off;
    editor_widget_get_offset_at_point(self, start_x + offset_x, start_y + offset_y, &off);
    
    self->cursor_offset = off;
    scroll_to_cursor(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
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

static void
move_cursor(EditorWidget *self, int visual_lines)
{
    size_t line = document_get_line_of_offset(self->doc, self->cursor_offset);
    size_t col_offset = self->cursor_offset - document_get_offset_of_line(self->doc, line);
    
    /* Logic to move to next line preserving col? */
    /* Only simple logic: move to start of next line + col? */
    size_t param_line = line + visual_lines;
    size_t count = document_get_line_count(self->doc);
    
    if (param_line >= count) param_line = count - 1;
    if (param_line < 0) param_line = 0; /* wrapped unsigned check? logic.. visual_lines is int */
    if (visual_lines < 0 && line == 0) param_line = 0;
    
    size_t new_line_start = document_get_offset_of_line(self->doc, param_line);
    size_t len;
    char *text = document_get_line(self->doc, param_line, &len);
    g_free(text);
    
    size_t new_col = col_offset;
    if (new_col > len) new_col = len; /* If newline included, might need len-1? */
    if (new_col > 0 && new_col == len) {
        /* check if last char is newline */
        /* document_get_line returns string with newline if present. */
        /* If we are at end, it's fine. */
    }
    
    self->cursor_offset = new_line_start + new_col;
    self->selection_anchor = self->cursor_offset; /* Reset selection strictly */
    
    scroll_to_cursor(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
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
            break;
        case GDK_KEY_Down:
            move_cursor(self, 1);
            if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
            break;
        case GDK_KEY_Left:
            self->cursor_offset = utf8_prev_grapheme(self->doc, self->cursor_offset);
            if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Right:
            self->cursor_offset = utf8_next_grapheme(self->doc, self->cursor_offset);
            if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_Home:
        {
            size_t line = document_get_line_of_offset(self->doc, self->cursor_offset);
            self->cursor_offset = document_get_offset_of_line(self->doc, line);
            if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
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
            /* If line has newline, end is len-1? */
            size_t start = document_get_offset_of_line(self->doc, line);
            /* Check if last char is \n */
            /* We need to peek but document_get_line returns string with \n. */
            size_t real_len = len;
            if (len > 0) {
                 /* We can't easily check last char from here without getting text again.
                    Optimized: document_get_text_range for last byte. */
                 char *last = document_get_text_range(self->doc, start + len - 1, 1);
                 if (last && last[0] == '\n') real_len--;
                 g_free(last);
            }
            self->cursor_offset = start + real_len;
            if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        }
        case GDK_KEY_Page_Up:
        {
             double page = (self->vadjustment) ? gtk_adjustment_get_page_size(self->vadjustment) : 10;
             move_cursor(self, -(int)page);
             if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
             break;
        }
        case GDK_KEY_Page_Down:
        {
             double page = (self->vadjustment) ? gtk_adjustment_get_page_size(self->vadjustment) : 10;
             move_cursor(self, (int)page);
             if (!(state & GDK_SHIFT_MASK)) self->selection_anchor = self->cursor_offset;
             break;   
        }
        case GDK_KEY_Return:
            document_insert(self->doc, self->cursor_offset, "\n", 1);
            self->cursor_offset++;
            self->selection_anchor = self->cursor_offset;
            editor_widget_update_adjustments(self);
            scroll_to_cursor(self);
            gtk_widget_queue_draw(GTK_WIDGET(self));
            break;
        case GDK_KEY_BackSpace:
            if (self->cursor_offset > 0) {
                size_t prev = utf8_prev_grapheme(self->doc, self->cursor_offset);
                size_t bytes_to_delete = self->cursor_offset - prev;
                document_delete(self->doc, prev, bytes_to_delete);
                self->cursor_offset = prev;
                self->selection_anchor = self->cursor_offset;
                editor_widget_update_adjustments(self);
                gtk_widget_queue_draw(GTK_WIDGET(self));
            }
            break;
        case GDK_KEY_Delete:
            if (self->cursor_offset < document_get_length(self->doc)) {
                size_t next = utf8_next_grapheme(self->doc, self->cursor_offset);
                size_t bytes_to_delete = next - self->cursor_offset;
                document_delete(self->doc, self->cursor_offset, bytes_to_delete);
                self->selection_anchor = self->cursor_offset;
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
    
    size_t len = strlen(str);
    document_insert(self->doc, self->cursor_offset, str, len);
    self->cursor_offset += len;
    self->selection_anchor = self->cursor_offset;
    
    editor_widget_update_adjustments(self);
    scroll_to_cursor(self);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

static void
editor_widget_dispose(GObject *object)
{
    EditorWidget *self = EDITOR_WIDGET(object);
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
    
    GtkEventController *controller = gtk_event_controller_key_new();
    g_signal_connect(controller, "key-pressed", G_CALLBACK(on_key_pressed), self);
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
    g_signal_connect(click, "pressed", G_CALLBACK(on_click_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));

    GtkGesture *drag = gtk_gesture_drag_new();
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_drag_update), self);
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
