#ifndef VITE_SETTINGS_H
#define VITE_SETTINGS_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
    gboolean show_line_numbers;
    gboolean enable_folding;
    gboolean minimap_enabled;
    gboolean highlight_current_line;
    gboolean show_save_button;
    
    char *font_name;
    gboolean use_custom_font;
    
    gboolean show_right_margin;
    int right_margin_position;
    gboolean wrap_lines;
    
    gboolean auto_indent;
    int indent_style; // 0 for Space, 1 for Tab
    int tab_width;
    int indent_width;
} ViteSettings;

void settings_init(void);
void settings_cleanup(void);

ViteSettings *settings_get(void);
void settings_save(void);

void settings_apply_to_all_editors(void);

G_END_DECLS

#endif /* VITE_SETTINGS_H */
