#include "editor-print.h"
#include "editor-internal.h"
#include <math.h>

typedef struct {
    EditorWidget *editor;
    GtkPrintSettings *settings;
    PangoFontDescription *font_desc;
    
    double char_width;
    double line_height;
    int lines_per_page;
    int n_pages;
    
    size_t total_lines;
    
    /* Layout */
    double margin_left;
    double margin_right;
    double margin_top;
    double margin_bottom;
    double content_w;
    double content_h;
    
    double header_height;
    double footer_height;
    
    gboolean original_theme_dark;
} PrintData;

static void
print_data_free(PrintData *data)
{
    /* Restore theme */
    if (data->editor && data->editor->syntax_ctx) {
        /* Only restore if we actually changed it? Or unconditionally?
           We saved original state, so just restore that. */
        if (syntax_get_theme_mode() != data->original_theme_dark) {
             syntax_set_theme_mode(data->original_theme_dark);
             syntax_context_invalidate_cache(data->editor->syntax_ctx);
             gtk_widget_queue_draw(GTK_WIDGET(data->editor));
        }
    }

    if (data->settings) g_object_unref(data->settings);
    if (data->font_desc) pango_font_description_free(data->font_desc);
    g_free(data);
}

static void
begin_print(GtkPrintOperation *operation, GtkPrintContext *context, gpointer user_data)
{
    PrintData *data = user_data;
    EditorWidget *self = data->editor;
    
    if (!self->doc) return;
    
    /* Force Light Theme for Printing */
    data->original_theme_dark = syntax_get_theme_mode();
    if (data->original_theme_dark) {
        syntax_set_theme_mode(FALSE); /* Light */
        if (self->syntax_ctx) syntax_context_invalidate_cache(self->syntax_ctx);
    }
    
    data->total_lines = document_get_line_count(self->doc);
    
    double width = gtk_print_context_get_width(context);
    double height = gtk_print_context_get_height(context);
    
    /* Margins? Usually handled by page setup, but checks */
    data->content_w = width;
    data->content_h = height;
    
    /* Create Layout for metrics */
    PangoLayout *layout = gtk_print_context_create_pango_layout(context);
    pango_layout_set_font_description(layout, data->font_desc);
    pango_layout_set_text(layout, "M", 1);
    
    PangoRectangle ink, logical;
    pango_layout_get_extents(layout, &ink, &logical);
    data->line_height = (double)logical.height / PANGO_SCALE;
    data->char_width = (double)logical.width / PANGO_SCALE;
    
    g_object_unref(layout);
    
    /* Header/Footer height estimate */
    data->header_height = data->line_height * 2.0; /* 2 lines space */
    data->footer_height = data->line_height * 2.0;
    
    double available_h = data->content_h - data->header_height - data->footer_height;
    if (available_h < data->line_height) available_h = data->line_height;
    
    data->lines_per_page = (int)(available_h / data->line_height);
    if (data->lines_per_page < 1) data->lines_per_page = 1;

    data->n_pages = (data->total_lines + data->lines_per_page - 1) / data->lines_per_page;
    if (data->n_pages == 0) data->n_pages = 1;
    
    gtk_print_operation_set_n_pages(operation, data->n_pages);
}

static void
draw_page(GtkPrintOperation *operation, GtkPrintContext *context, gint page_nr, gpointer user_data)
{
    PrintData *data = user_data;
    EditorWidget *self = data->editor;
    cairo_t *cr = gtk_print_context_get_cairo_context(context);
    
    double width = data->content_w;
    double height = data->content_h;
    
    /* 1. Header: Filename */
    const char *filename = NULL;
    if (self->doc) filename = document_get_file_path(self->doc);
    if (!filename) filename = "Untitled";
    else filename = g_path_get_basename(filename); /* Just basename */

    PangoLayout *layout = gtk_print_context_create_pango_layout(context);
    pango_layout_set_font_description(layout, data->font_desc);
    
    char header_buf[1024];
    snprintf(header_buf, sizeof(header_buf), "File: %s", filename);
    pango_layout_set_text(layout, header_buf, -1);
    
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_move_to(cr, 0, 0);
    pango_cairo_show_layout(cr, layout);
    
    /* Header Line */
    cairo_move_to(cr, 0, data->line_height * 1.2);
    cairo_line_to(cr, width, data->line_height * 1.2);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);
    
    /* 2. Footer: Page Numbers */
    char footer_buf[64];
    snprintf(footer_buf, sizeof(footer_buf), "Page %d of %d", page_nr + 1, data->n_pages);
    pango_layout_set_text(layout, footer_buf, -1);
    pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    pango_layout_set_width(layout, width * PANGO_SCALE);
    
    double footer_y = height - data->line_height;
    cairo_move_to(cr, 0, footer_y);
    pango_cairo_show_layout(cr, layout);
    
    /* Footer Line */
    cairo_move_to(cr, 0, footer_y - data->line_height * 0.2);
    cairo_line_to(cr, width, footer_y - data->line_height * 0.2);
    cairo_stroke(cr);
    
    /* 3. Content */
    size_t start_line = page_nr * data->lines_per_page;
    size_t end_line = start_line + data->lines_per_page;
    if (end_line > data->total_lines) end_line = data->total_lines;
    
    /* Check if line numbers should be shown */
    gboolean show_line_numbers = editor_widget_get_show_line_numbers(self);
    
    /* Calculate Line Number width */
    double lnum_w = 0.0;
    if (show_line_numbers) {
        int max_digits = 1;
        size_t tmp = data->total_lines;
        while (tmp >= 10) { tmp /= 10; max_digits++; }
        lnum_w = (max_digits + 1) * data->char_width;
    }
    
    double content_start_y = data->header_height;
    double current_y = content_start_y;
    
    /* Ensure syntax logic is ready (if any) */
    if (self->syntax_ctx) {
        editor_widget_ensure_syntax_state_up_to(self, end_line);
    }
    
    /* Use Monospace font for content */
    pango_layout_set_width(layout, -1); /* Unwrapped for now, or wrap? Let's check wrap settings */
    /* Printing usually wraps to avoid cut off */
    pango_layout_set_width(layout, (width - lnum_w) * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    
    for (size_t i = start_line; i < end_line; i++) {
        /* Line Number */
        if (show_line_numbers) {
            char lnum[32];
            snprintf(lnum, sizeof(lnum), "%zu", i + 1);
            pango_layout_set_text(layout, lnum, -1);
            pango_layout_set_width(layout, -1); /* Auto width for number */
            pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT); /* Line numbers right aligned */
            pango_layout_set_attributes(layout, NULL); /* Clear potential attributes from previous line */
            
            cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
            
            /* Right align number */
            int w;
            pango_layout_get_pixel_size(layout, &w, NULL);
            cairo_move_to(cr, lnum_w - w - 4, current_y);
            pango_cairo_show_layout(cr, layout);
        }
        
        /* Text */
        size_t len;
        char *text = document_get_line_truncated(self->doc, i, &len, 4096, NULL);
        if (!text) {
             text = g_strdup("");
             len = 0;
        }
        
        /* Strip newlines */
        while (len > 0 && (text[len-1] == '\r' || text[len-1] == '\n')) {
            text[len-1] = '\0';
            len--;
        }
        
        pango_layout_set_text(layout, text, len);
        pango_layout_set_text(layout, text, len);
        pango_layout_set_width(layout, (width - lnum_w) * PANGO_SCALE);
        pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT); /* Default left align for text */
        
        /* Syntax Highlighting */
        if (self->syntax_ctx) {
            PangoAttrList *attrs = syntax_highlight_line(self->syntax_ctx, i, text);
            if (attrs) {
                pango_layout_set_attributes(layout, attrs);
                pango_attr_list_unref(attrs);
            } else {
                pango_layout_set_attributes(layout, NULL);
            }
        }
        
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_move_to(cr, lnum_w, current_y);
        pango_cairo_show_layout(cr, layout);
        
        int line_pixel_h;
        pango_layout_get_pixel_size(layout, NULL, &line_pixel_h);
        /* Ensure min height */
        double advance = (double)line_pixel_h;
        if (advance < data->line_height) advance = data->line_height;
        
        current_y += advance;
        g_free(text);
        
        /* If page overflow (due to wrapping), stop? 
           Basic pagination assumes fixed height lines or we need to pre-calculate wrapping. 
           For now, let's just let it overflow a bit or clip. 
           Ideally begin_print should measure everything but that's expensive.
        */
        if (current_y > height - data->footer_height) break;
    }
    
    g_object_unref(layout);
    if (filename && g_strcmp0(filename, "Untitled") != 0) g_free((char*)filename);
}

void
editor_print_start(EditorWidget *self)
{
    GtkPrintOperation *op = gtk_print_operation_new();
    PrintData *data = g_new0(PrintData, 1);
    
    data->editor = self;
    data->font_desc = pango_font_description_copy(self->font_desc);
    
    /* Slightly smaller font for print? Or use same? */
    /* Let's keep same for WYSIWYG-ish feel */
    
    gtk_print_operation_set_job_name(op, "ViTE Document");
    
    g_signal_connect(op, "begin-print", G_CALLBACK(begin_print), data);
    g_signal_connect(op, "draw-page", G_CALLBACK(draw_page), data);
    
    /* Free data when done */
    g_signal_connect_swapped(op, "done", G_CALLBACK(print_data_free), data);
    
    GtkPrintOperationResult res = gtk_print_operation_run(op, 
        GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, 
        GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))), 
        NULL);
        
    /* Async handling or blocking? gtk_print_operation_run is blocking for doc?
       Wait, if we use PRINT_DIALOG it returns.
       But standard usage is async usually?
    */
    
    g_object_unref(op);
}
