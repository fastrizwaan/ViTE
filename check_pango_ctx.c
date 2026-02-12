#include <pango/pangocairo.h>
#include <gtk/gtk.h>
#include <stdio.h>

int main(int argc, char **argv) {
    gtk_init();
    PangoFontMap *fontmap = pango_cairo_font_map_get_default();
    PangoContext *ctx = pango_font_map_create_context(fontmap);
    
    // Check default dir
    PangoDirection dir = pango_context_get_base_dir(ctx);
    printf("Default Base Dir: %s\n", dir == PANGO_DIRECTION_LTR ? "LTR" : (dir == PANGO_DIRECTION_RTL ? "RTL" : "WEAK"));

    // Set to RTL
    pango_context_set_base_dir(ctx, PANGO_DIRECTION_RTL);
    dir = pango_context_get_base_dir(ctx);
    printf("Set Base Dir to RTL: %s\n", dir == PANGO_DIRECTION_LTR ? "LTR" : (dir == PANGO_DIRECTION_RTL ? "RTL" : "WEAK"));

    // Create a layout with empty text and see its resolved dir
    PangoLayout *layout = pango_layout_new(ctx);
    pango_layout_set_text(layout, "", -1);
    // Auto dir on empty text?
    pango_layout_set_auto_dir(layout, TRUE);
    
    // Check line dir
    PangoLayoutLine *line = pango_layout_get_line_readonly(layout, 0);
    if (line) {
        printf("Empty Line Dir: %s\n", line->resolved_dir == PANGO_DIRECTION_LTR ? "LTR" : "RTL");
    }

    return 0;
}
