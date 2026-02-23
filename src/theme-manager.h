#ifndef THEME_MANAGER_H
#define THEME_MANAGER_H

#include <gtk/gtk.h>
#include <pango/pango.h>

/* Semantic color slots for syntax highlighting */
typedef enum {
    COLOR_KEYWORD,
    COLOR_KEYWORD_CONTROL,
    COLOR_BUILTIN,
    COLOR_STRING,
    COLOR_COMMENT,
    COLOR_NUMBER,
    COLOR_FUNCTION,
    COLOR_TYPE,
    COLOR_DECORATOR,
    COLOR_VARIABLE,
    COLOR_VARIABLE_C,
    COLOR_CONSTANT,
    COLOR_CONSTANT_LANG,
    COLOR_TAG,
    COLOR_STORAGE,
    COLOR_OPERATOR,
    COLOR_PUNCTUATION,
    COLOR_ATTRIBUTE,
    COLOR_PARAM,
    COLOR_PROPERTY,
    COLOR_PREPROC,
    COLOR_LOGICAL,
    COLOR_SLOT_COUNT
} ViteColorSlot;

/* Centralized theme palette */
typedef struct {
    /* Editor / Widget colors (GdkRGBA) */
    GdkRGBA editor_bg;
    GdkRGBA editor_fg;
    GdkRGBA gutter_bg;
    GdkRGBA gutter_fg;
    GdkRGBA gutter_active_fg;
    GdkRGBA line_highlight;
    GdkRGBA selection;
    GdkRGBA cursor_color;
    GdkRGBA find_match;
    GdkRGBA find_match_highlight;

    /* Tab bar colors */
    GdkRGBA tab_active_bg;
    GdkRGBA tab_active_fg;
    GdkRGBA tab_inactive_bg;
    GdkRGBA tab_inactive_fg;
    GdkRGBA tab_border;

    /* Header bar / Title bar */
    GdkRGBA titlebar_bg;
    GdkRGBA titlebar_fg;

    /* Status bar */
    GdkRGBA statusbar_bg;
    GdkRGBA statusbar_fg;

    /* Scrollbar */
    GdkRGBA scrollbar_bg;
    GdkRGBA scrollbar_hover;
    GdkRGBA scrollbar_active;

    /* Syntax colors (PangoColor — used directly by add_color_attr) */
    PangoColor syntax[COLOR_SLOT_COUNT];

    /* Metadata */
    gboolean is_dark;
    char *name;
    char *file_path;
} ViteTheme;

/* Initialize the theme manager, load all themes from disk */
void theme_manager_init(void);

/* Cleanup */
void theme_manager_cleanup(void);

/* Get number of available themes */
int theme_manager_get_count(void);

/* Get theme name by index */
const char *theme_manager_get_name(int index);

/* Apply a theme by name; updates the global palette */
void theme_manager_apply_theme(const char *theme_name);

/* Get current active theme (never NULL after init) */
const ViteTheme *theme_manager_get_current(void);

/* Monotonic revision incremented each time a theme is applied */
guint64 theme_manager_get_revision(void);

/* Persist / load selection */
void theme_manager_save_selection(const char *theme_name);
char *theme_manager_load_selection(void);

#endif
