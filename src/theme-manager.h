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
    COLOR_MEMBER,
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
    COLOR_BRACKET_1,
    COLOR_BRACKET_2,
    COLOR_BRACKET_3,
    COLOR_BRACKET_4,
    COLOR_BRACKET_5,
    COLOR_BRACKET_6,
    COLOR_BRACKET_UNMATCHED,
    COLOR_SLOT_COUNT
} ViteColorSlot;

#define VITE_LANG_COUNT 16

/* Font style bitflags */
#define VITE_FONT_STYLE_BOLD      (1 << 0)
#define VITE_FONT_STYLE_ITALIC    (1 << 1)
#define VITE_FONT_STYLE_UNDERLINE (1 << 2)

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
    guint8 syntax_style[COLOR_SLOT_COUNT];
    gboolean has_style_set[COLOR_SLOT_COUNT];

    /* Language-specific overrides */
    PangoColor syntax_lang[VITE_LANG_COUNT][COLOR_SLOT_COUNT];
    guint8 syntax_lang_style[VITE_LANG_COUNT][COLOR_SLOT_COUNT];
    gboolean has_lang_syntax[VITE_LANG_COUNT][COLOR_SLOT_COUNT];
    gboolean has_lang_style_set[VITE_LANG_COUNT][COLOR_SLOT_COUNT];

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
