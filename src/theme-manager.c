#include "theme-manager.h"
#include <adwaita.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <math.h>

/* --- Static State --- */
static GPtrArray *all_themes = NULL;  /* ViteTheme* */
static ViteTheme *current_theme = NULL;
static GtkCssProvider *current_css_provider = NULL;

/* Check if a theme is a built-in default that should use native Adwaita colors */
static gboolean
is_default_theme(const char *name)
{
    return (g_strcmp0(name, "Default Dark") == 0 ||
            g_strcmp0(name, "Default Light") == 0);
}

/* --- Built-in Default Palettes --- */

static void
set_default_dark_theme(ViteTheme *theme)
{
    /* One Dark colors */
    gdk_rgba_parse(&theme->editor_bg, "#282C34");
    gdk_rgba_parse(&theme->editor_fg, "#ABB2BF");
    gdk_rgba_parse(&theme->gutter_bg, "#282C34");
    gdk_rgba_parse(&theme->gutter_fg, "#636D83");
    gdk_rgba_parse(&theme->gutter_active_fg, "#ABB2BF");
    gdk_rgba_parse(&theme->line_highlight, "#99BBFF0A");
    gdk_rgba_parse(&theme->selection, "#3E4451");
    gdk_rgba_parse(&theme->cursor_color, "#528BFF");
    gdk_rgba_parse(&theme->find_match, "#528BFF3D");
    gdk_rgba_parse(&theme->find_match_highlight, "#528BFF3D");

    gdk_rgba_parse(&theme->tab_active_bg, "#282C34");
    gdk_rgba_parse(&theme->tab_active_fg, "#D7DAE0");
    gdk_rgba_parse(&theme->tab_inactive_bg, "#21252B");
    gdk_rgba_parse(&theme->tab_inactive_fg, "#9DA5B4");
    gdk_rgba_parse(&theme->tab_border, "#181A1F");

    gdk_rgba_parse(&theme->titlebar_bg, "#21252B");
    gdk_rgba_parse(&theme->titlebar_fg, "#9DA5B4");
    gdk_rgba_parse(&theme->statusbar_bg, "#21252B");
    gdk_rgba_parse(&theme->statusbar_fg, "#9DA5B4");

    gdk_rgba_parse(&theme->scrollbar_bg, "#4E566680");
    gdk_rgba_parse(&theme->scrollbar_hover, "#5A637580");
    gdk_rgba_parse(&theme->scrollbar_active, "#747D9180");

    pango_color_parse(&theme->syntax[COLOR_KEYWORD], "#c678dd");
    pango_color_parse(&theme->syntax[COLOR_BUILTIN], "#56b6c2");
    pango_color_parse(&theme->syntax[COLOR_STRING], "#98c379");
    pango_color_parse(&theme->syntax[COLOR_COMMENT], "#7f848e");
    pango_color_parse(&theme->syntax[COLOR_NUMBER], "#d19a66");
    pango_color_parse(&theme->syntax[COLOR_FUNCTION], "#61afef");
    pango_color_parse(&theme->syntax[COLOR_TYPE], "#e5c07b");
    pango_color_parse(&theme->syntax[COLOR_DECORATOR], "#56b6c2");
    pango_color_parse(&theme->syntax[COLOR_VARIABLE], "#e06c75");
    pango_color_parse(&theme->syntax[COLOR_VARIABLE_C], "#d1d1d1");
    pango_color_parse(&theme->syntax[COLOR_CONSTANT], "#e06c75");
    pango_color_parse(&theme->syntax[COLOR_TAG], "#e06c75");
    pango_color_parse(&theme->syntax[COLOR_OPERATOR], "#d19a66");
    pango_color_parse(&theme->syntax[COLOR_PUNCTUATION], "#d19a66");
    pango_color_parse(&theme->syntax[COLOR_ATTRIBUTE], "#d19a66");
    pango_color_parse(&theme->syntax[COLOR_PARAM], "#e06c75");
    pango_color_parse(&theme->syntax[COLOR_PROPERTY], "#56b6c2");
    pango_color_parse(&theme->syntax[COLOR_PREPROC], "#c678dd");
    pango_color_parse(&theme->syntax[COLOR_LOGICAL], "#56b6c2");

    theme->is_dark = TRUE;
}

static void
set_default_light_theme(ViteTheme *theme)
{
    /* One Light colors */
    gdk_rgba_parse(&theme->editor_bg, "#F2F2F2");
    gdk_rgba_parse(&theme->editor_fg, "#383A42");
    gdk_rgba_parse(&theme->gutter_bg, "#F2F2F2");
    gdk_rgba_parse(&theme->gutter_fg, "#9DA5B4");
    gdk_rgba_parse(&theme->gutter_active_fg, "#383A42");
    gdk_rgba_parse(&theme->line_highlight, "#E8E8E8");
    gdk_rgba_parse(&theme->selection, "#6D6D6D25");
    gdk_rgba_parse(&theme->cursor_color, "#044289");
    gdk_rgba_parse(&theme->find_match, "#ffdf5d");
    gdk_rgba_parse(&theme->find_match_highlight, "#ffdf5d66");

    gdk_rgba_parse(&theme->tab_active_bg, "#F2F2F2");
    gdk_rgba_parse(&theme->tab_active_fg, "#2f363d");
    gdk_rgba_parse(&theme->tab_inactive_bg, "#E8E8E8");
    gdk_rgba_parse(&theme->tab_inactive_fg, "#6a737d");
    gdk_rgba_parse(&theme->tab_border, "#CFCFCF");

    gdk_rgba_parse(&theme->titlebar_bg, "#F2F2F2");
    gdk_rgba_parse(&theme->titlebar_fg, "#2f363d");
    gdk_rgba_parse(&theme->statusbar_bg, "#F2F2F2");
    gdk_rgba_parse(&theme->statusbar_fg, "#586069");

    gdk_rgba_parse(&theme->scrollbar_bg, "#959da533");
    gdk_rgba_parse(&theme->scrollbar_hover, "#959da544");
    gdk_rgba_parse(&theme->scrollbar_active, "#959da588");

    pango_color_parse(&theme->syntax[COLOR_KEYWORD], "#a626a4");
    pango_color_parse(&theme->syntax[COLOR_BUILTIN], "#0184bc");
    pango_color_parse(&theme->syntax[COLOR_STRING], "#50a14f");
    pango_color_parse(&theme->syntax[COLOR_COMMENT], "#a0a1a7");
    pango_color_parse(&theme->syntax[COLOR_NUMBER], "#986801");
    pango_color_parse(&theme->syntax[COLOR_FUNCTION], "#4078f2");
    pango_color_parse(&theme->syntax[COLOR_TYPE], "#c18401");
    pango_color_parse(&theme->syntax[COLOR_DECORATOR], "#a626a4");
    pango_color_parse(&theme->syntax[COLOR_VARIABLE], "#e45649");
    pango_color_parse(&theme->syntax[COLOR_VARIABLE_C], "#383a42");
    pango_color_parse(&theme->syntax[COLOR_CONSTANT], "#e45649");
    pango_color_parse(&theme->syntax[COLOR_TAG], "#e45649");
    pango_color_parse(&theme->syntax[COLOR_OPERATOR], "#986801");
    pango_color_parse(&theme->syntax[COLOR_PUNCTUATION], "#986801");
    pango_color_parse(&theme->syntax[COLOR_ATTRIBUTE], "#986801");
    pango_color_parse(&theme->syntax[COLOR_PARAM], "#e45649");
    pango_color_parse(&theme->syntax[COLOR_PROPERTY], "#0184bc");
    pango_color_parse(&theme->syntax[COLOR_PREPROC], "#a626a4");
    pango_color_parse(&theme->syntax[COLOR_LOGICAL], "#0184bc");

    theme->is_dark = FALSE;
}

/* --- Scope Matching Helpers --- */

typedef struct {
    const char *scope;
    ViteColorSlot slot;
} ScopeMapping;

/* Ordered from most specific to least specific for priority matching */
static const ScopeMapping scope_map[] = {
    { "keyword.operator.logical",         COLOR_LOGICAL },
    { "keyword.operator.comparison",       COLOR_LOGICAL },
    { "keyword.operator",              COLOR_OPERATOR },
    { "keyword.control.directive",         COLOR_PREPROC },
    { "keyword.control",               COLOR_KEYWORD },
    { "keyword.other.unit",            COLOR_NUMBER },
    { "keyword",                       COLOR_KEYWORD },
    { "storage.type",                  COLOR_KEYWORD },
    { "storage.modifier",              COLOR_KEYWORD },
    { "storage",                       COLOR_KEYWORD },
    { "constant.numeric",              COLOR_NUMBER },
    { "constant.character.escape",     COLOR_BUILTIN },
    { "constant.other.color",          COLOR_BUILTIN },
    { "constant.other.symbol",         COLOR_BUILTIN },
    { "constant.variable",             COLOR_NUMBER },
    { "constant",                      COLOR_CONSTANT },
    { "variable.parameter",            COLOR_PARAM },
    { "variable.interpolation",        COLOR_VARIABLE },
    { "variable.other.property",       COLOR_PROPERTY },
    { "variable",                      COLOR_VARIABLE },
    { "string.regexp",                 COLOR_BUILTIN },
    { "string",                        COLOR_STRING },
    { "comment",                       COLOR_COMMENT },
    { "entity.name.function",          COLOR_FUNCTION },
    { "entity.name.type",              COLOR_TYPE },
    { "entity.name.class",             COLOR_TYPE },
    { "entity.name.tag",               COLOR_TAG },
    { "entity.name.section",           COLOR_FUNCTION },
    { "entity.other.attribute-name",   COLOR_ATTRIBUTE },
    { "entity.other.inherited-class",  COLOR_TYPE },
    { "support.class",                 COLOR_TYPE },
    { "support.type",                  COLOR_BUILTIN },
    { "support.function.any-method",   COLOR_FUNCTION },
    { "support.function",              COLOR_BUILTIN },
    { "meta.preprocessor",             COLOR_PREPROC },
    { "meta.class",                    COLOR_TYPE },
    { "markup.heading",                COLOR_TAG },
    { "markup.bold",                   COLOR_ATTRIBUTE },
    { "markup.italic",                 COLOR_KEYWORD },
    { "markup.inserted",               COLOR_STRING },
    { "markup.deleted",                COLOR_VARIABLE },
    { "markup.changed",                COLOR_KEYWORD },
    { "markup.link",                   COLOR_BUILTIN },
    { "markup.raw",                    COLOR_STRING },
    { "markup.quote",                  COLOR_ATTRIBUTE },
    { "punctuation.definition.comment",COLOR_COMMENT },
    { "punctuation.definition.string", COLOR_STRING },
    { "punctuation.separator",         COLOR_PUNCTUATION },
    { "punctuation.definition",        COLOR_PUNCTUATION },
    { "punctuation.terminator",        COLOR_PUNCTUATION },
    { "punctuation.accessor",          COLOR_PUNCTUATION },
    { "punctuation.section",           COLOR_PUNCTUATION },
    { "punctuation",                   COLOR_PUNCTUATION },
    { NULL, 0 }
};

static gboolean
scope_starts_with(const char *scope, const char *prefix)
{
    if (!g_str_has_prefix(scope, prefix)) return FALSE;
    /* Must be exact match or followed by '.' (sub-scope) */
    size_t plen = strlen(prefix);
    char next = scope[plen];
    return (next == '\0' || next == '.');
}

/* Try to match a single scope string against our mapping table.
   Handles compound scopes like "source.c keyword.operator" by trying
   each space-separated part. Returns TRUE if matched, and sets *slot.
   If is_compound is non-NULL, sets it to TRUE if the original scope
   was a compound (language-qualified) scope. */
static gboolean
match_scope(const char *scope_str, ViteColorSlot *slot, gboolean *is_compound)
{
    if (is_compound) *is_compound = FALSE;

    /* If scope contains spaces (compound scope like "source.c keyword.operator"),
     * try each part individually, picking the last matching one */
    if (strchr(scope_str, ' ')) {
        char **parts = g_strsplit(scope_str, " ", -1);
        gboolean found = FALSE;
        for (int p = 0; parts[p]; p++) {
            char *trimmed = g_strstrip(parts[p]);
            if (trimmed[0] == '\0') continue;
            if (trimmed[0] == '>') continue; /* skip ">" separators */
            for (int i = 0; scope_map[i].scope != NULL; i++) {
                if (scope_starts_with(trimmed, scope_map[i].scope)) {
                    *slot = scope_map[i].slot;
                    found = TRUE;
                    if (is_compound) *is_compound = TRUE;
                    break;
                }
            }
        }
        g_strfreev(parts);
        return found;
    }

    /* Simple single-scope matching */
    for (int i = 0; scope_map[i].scope != NULL; i++) {
        if (scope_starts_with(scope_str, scope_map[i].scope)) {
            *slot = scope_map[i].slot;
            return TRUE;
        }
    }
    return FALSE;
}

/* --- JSON Parsing --- */

static gboolean
parse_hex_color_to_pango(const char *hex, PangoColor *color)
{
    if (!hex || hex[0] != '#') return FALSE;
    return pango_color_parse(color, hex);
}

static gboolean
parse_hex_color_to_rgba(const char *hex, GdkRGBA *color)
{
    if (!hex || hex[0] != '#') return FALSE;
    return gdk_rgba_parse(color, hex);
}

static const char *
json_object_get_string_safe(JsonObject *obj, const char *member)
{
    if (!obj || !json_object_has_member(obj, member)) return NULL;
    JsonNode *node = json_object_get_member(obj, member);
    if (!JSON_NODE_HOLDS_VALUE(node)) return NULL;
    return json_node_get_string(node);
}

static void
parse_editor_colors(JsonObject *colors, ViteTheme *theme)
{
    const char *val;

    /* Editor */
    if ((val = json_object_get_string_safe(colors, "editor.background")))
        parse_hex_color_to_rgba(val, &theme->editor_bg);
    if ((val = json_object_get_string_safe(colors, "editor.foreground")))
        parse_hex_color_to_rgba(val, &theme->editor_fg);
    if ((val = json_object_get_string_safe(colors, "editor.lineHighlightBackground")))
        parse_hex_color_to_rgba(val, &theme->line_highlight);
    if ((val = json_object_get_string_safe(colors, "editor.selectionBackground")))
        parse_hex_color_to_rgba(val, &theme->selection);
    if ((val = json_object_get_string_safe(colors, "editorCursor.foreground")))
        parse_hex_color_to_rgba(val, &theme->cursor_color);
    if ((val = json_object_get_string_safe(colors, "editor.findMatchHighlightBackground")))
        parse_hex_color_to_rgba(val, &theme->find_match_highlight);
    if ((val = json_object_get_string_safe(colors, "editor.findMatchBackground")))
        parse_hex_color_to_rgba(val, &theme->find_match);

    /* Line numbers */
    if ((val = json_object_get_string_safe(colors, "editorLineNumber.foreground")))
        parse_hex_color_to_rgba(val, &theme->gutter_fg);
    if ((val = json_object_get_string_safe(colors, "editorLineNumber.activeForeground")))
        parse_hex_color_to_rgba(val, &theme->gutter_active_fg);

    /* Tab bar */
    if ((val = json_object_get_string_safe(colors, "tab.activeBackground")))
        parse_hex_color_to_rgba(val, &theme->tab_active_bg);
    if ((val = json_object_get_string_safe(colors, "tab.activeForeground")))
        parse_hex_color_to_rgba(val, &theme->tab_active_fg);
    if ((val = json_object_get_string_safe(colors, "tab.inactiveBackground")))
        parse_hex_color_to_rgba(val, &theme->tab_inactive_bg);
    if ((val = json_object_get_string_safe(colors, "tab.inactiveForeground")))
        parse_hex_color_to_rgba(val, &theme->tab_inactive_fg);
    if ((val = json_object_get_string_safe(colors, "tab.border")))
        parse_hex_color_to_rgba(val, &theme->tab_border);

    /* Title bar */
    if ((val = json_object_get_string_safe(colors, "titleBar.activeBackground")))
        parse_hex_color_to_rgba(val, &theme->titlebar_bg);
    if ((val = json_object_get_string_safe(colors, "titleBar.activeForeground")))
        parse_hex_color_to_rgba(val, &theme->titlebar_fg);

    /* Status bar */
    if ((val = json_object_get_string_safe(colors, "statusBar.background")))
        parse_hex_color_to_rgba(val, &theme->statusbar_bg);
    if ((val = json_object_get_string_safe(colors, "statusBar.foreground")))
        parse_hex_color_to_rgba(val, &theme->statusbar_fg);

    /* Scrollbar */
    if ((val = json_object_get_string_safe(colors, "scrollbarSlider.background")))
        parse_hex_color_to_rgba(val, &theme->scrollbar_bg);
    if ((val = json_object_get_string_safe(colors, "scrollbarSlider.hoverBackground")))
        parse_hex_color_to_rgba(val, &theme->scrollbar_hover);
    if ((val = json_object_get_string_safe(colors, "scrollbarSlider.activeBackground")))
        parse_hex_color_to_rgba(val, &theme->scrollbar_active);

    /* Gutter BG same as editor bg by default */
    theme->gutter_bg = theme->editor_bg;
}

static void
parse_token_colors(JsonArray *token_colors, ViteTheme *theme, gboolean *slot_set_out)
{
    /* Track which slots have been set — first match wins per slot */
    gboolean slot_set[COLOR_SLOT_COUNT];
    memset(slot_set, 0, sizeof(slot_set));

    guint n = json_array_get_length(token_colors);
    for (guint i = 0; i < n; i++) {
        JsonNode *elem = json_array_get_element(token_colors, i);
        if (!JSON_NODE_HOLDS_OBJECT(elem)) continue;
        JsonObject *rule = json_node_get_object(elem);

        /* Get settings.foreground */
        if (!json_object_has_member(rule, "settings")) continue;
        JsonObject *settings = json_object_get_object_member(rule, "settings");
        if (!settings) continue;
        const char *fg = json_object_get_string_safe(settings, "foreground");
        if (!fg) continue;

        PangoColor color;
        if (!parse_hex_color_to_pango(fg, &color)) continue;

        /* Get scope — can be string or array */
        JsonNode *scope_node = json_object_has_member(rule, "scope") ?
            json_object_get_member(rule, "scope") : NULL;
        if (!scope_node) continue;

        GPtrArray *scopes = g_ptr_array_new();
        if (JSON_NODE_HOLDS_ARRAY(scope_node)) {
            JsonArray *arr = json_node_get_array(scope_node);
            guint sn = json_array_get_length(arr);
            for (guint si = 0; si < sn; si++) {
                const char *s = json_array_get_string_element(arr, si);
                if (s) g_ptr_array_add(scopes, (gpointer)s);
            }
        } else if (JSON_NODE_HOLDS_VALUE(scope_node)) {
            const char *s = json_node_get_string(scope_node);
            if (s) {
                /* Could be comma-separated */
                char **parts = g_strsplit(s, ",", -1);
                for (int pi = 0; parts[pi]; pi++) {
                    char *trimmed = g_strstrip(g_strdup(parts[pi]));
                    g_ptr_array_add(scopes, trimmed);
                }
                g_strfreev(parts);
            }
        }

        for (guint si = 0; si < scopes->len; si++) {
            const char *scope_str = g_ptr_array_index(scopes, si);
            ViteColorSlot slot;
            gboolean is_compound = FALSE;
            if (match_scope(scope_str, &slot, &is_compound)) {
                /* First match wins per slot */
                if (!slot_set[slot]) {
                    theme->syntax[slot] = color;
                    slot_set[slot] = TRUE;
                }
            }
        }
        g_ptr_array_free(scopes, TRUE);
    }

    /* Copy out which slots were set */
    if (slot_set_out) {
        memcpy(slot_set_out, slot_set, sizeof(slot_set));
    }
}

static gboolean
detect_is_dark(const GdkRGBA *bg)
{
    double lum = bg->red * 0.299 + bg->green * 0.587 + bg->blue * 0.114;
    return lum < 0.5;
}

static ViteTheme *
load_theme_from_json(const char *path)
{
    GError *error = NULL;
    JsonParser *parser = json_parser_new();

    if (!json_parser_load_from_file(parser, path, &error)) {
        g_warning("Could not parse theme '%s': %s", path, error->message);
        g_error_free(error);
        g_object_unref(parser);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return NULL;
    }

    JsonObject *obj = json_node_get_object(root);
    const char *name = json_object_get_string_safe(obj, "name");
    if (!name) {
        /* Derive name from filename */
        char *basename = g_path_get_basename(path);
        char *dotpos = strrchr(basename, '.');
        if (dotpos) *dotpos = '\0';
        name = basename; /* will be duped below */
    }

    ViteTheme *theme = g_new0(ViteTheme, 1);
    theme->name = g_strdup(name);
    theme->file_path = g_strdup(path);

    /* Start with defaults based on whether we detect dark from name */
    if (g_strstr_len(name, -1, "Light") || g_strstr_len(name, -1, "light")) {
        set_default_light_theme(theme);
    } else {
        set_default_dark_theme(theme);
    }

    /* Parse editor colors */
    if (json_object_has_member(obj, "colors")) {
        JsonObject *colors = json_object_get_object_member(obj, "colors");
        if (colors) parse_editor_colors(colors, theme);
    }

    /* Reset all syntax slots to foreground color so uncovered scopes
     * don't leak hardcoded One Dark/Light fallback colors */
    {
        PangoColor fg_pango;
        fg_pango.red   = (guint16)(theme->editor_fg.red   * 65535);
        fg_pango.green = (guint16)(theme->editor_fg.green * 65535);
        fg_pango.blue  = (guint16)(theme->editor_fg.blue  * 65535);
        for (int i = 0; i < COLOR_SLOT_COUNT; i++) {
            theme->syntax[i] = fg_pango;
        }
    }

    /* Parse token colors for syntax */
    gboolean slot_set[COLOR_SLOT_COUNT];
    memset(slot_set, 0, sizeof(slot_set));
    if (json_object_has_member(obj, "tokenColors")) {
        JsonArray *token_colors = json_object_get_array_member(obj, "tokenColors");
        if (token_colors) parse_token_colors(token_colors, theme, slot_set);
    }

    /* Fallback inheritance: unset slots inherit from parent scope colors.
     * This matches how VSCode inherits styles from broader scopes. */
    #define INHERIT(child, parent) \
        if (!slot_set[child] && slot_set[parent]) { \
            theme->syntax[child] = theme->syntax[parent]; \
        }
    INHERIT(COLOR_PREPROC,    COLOR_KEYWORD)    /* preprocessor ← keyword */
    INHERIT(COLOR_OPERATOR,   COLOR_KEYWORD)    /* operator ← keyword */
    INHERIT(COLOR_LOGICAL,    COLOR_OPERATOR)   /* logical ← operator ← keyword */
    INHERIT(COLOR_LOGICAL,    COLOR_KEYWORD)    /* logical ← keyword (if operator also unset) */
    INHERIT(COLOR_VARIABLE_C, COLOR_VARIABLE)   /* C variable ← variable */
    INHERIT(COLOR_PROPERTY,   COLOR_VARIABLE)   /* property ← variable */
    INHERIT(COLOR_CONSTANT,   COLOR_NUMBER)     /* constant ← number */
    INHERIT(COLOR_DECORATOR,  COLOR_FUNCTION)   /* decorator ← function */
    INHERIT(COLOR_BUILTIN,    COLOR_FUNCTION)   /* builtin ← function */
    #undef INHERIT

    /* Detect dark/light from actual background color */
    theme->is_dark = detect_is_dark(&theme->editor_bg);

    g_object_unref(parser);
    return theme;
}

static void
theme_free(ViteTheme *theme)
{
    if (!theme) return;
    g_free(theme->name);
    g_free(theme->file_path);
    g_free(theme);
}

/* --- Theme Directory Discovery --- */

static void
scan_theme_directory(const char *dir_path)
{
    if (!dir_path || !g_file_test(dir_path, G_FILE_TEST_IS_DIR)) return;

    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) return;

    const char *filename;
    while ((filename = g_dir_read_name(dir)) != NULL) {
        if (!g_str_has_suffix(filename, ".json")) continue;

        char *full_path = g_build_filename(dir_path, filename, NULL);
        ViteTheme *theme = load_theme_from_json(full_path);
        if (theme) {
            g_ptr_array_add(all_themes, theme);
        }
        g_free(full_path);
    }
    g_dir_close(dir);
}

/* Helper: shift a color channel towards dark or light for chrome surfaces */
static inline double
shift_color(double val, double amount, gboolean darken)
{
    if (darken)
        return CLAMP(val - amount, 0.0, 1.0);
    else
        return CLAMP(val + amount, 0.0, 1.0);
}

static void
rgba_to_css(const GdkRGBA *c, char *buf, size_t size)
{
    snprintf(buf, size, "rgba(%d,%d,%d,%.2f)",
             (int)(c->red * 255), (int)(c->green * 255),
             (int)(c->blue * 255), c->alpha);
}

static char *
generate_css(const ViteTheme *theme)
{
    GString *css = g_string_new("");

    /* Base colors: derive chrome from editor background */
    GdkRGBA bg = theme->editor_bg;
    GdkRGBA fg = theme->editor_fg;

    /* Chrome bg: slightly darker for dark themes, slightly lighter for light */
    double shift = 0.04;
    GdkRGBA chrome_bg = {
        shift_color(bg.red,   shift, !theme->is_dark),
        shift_color(bg.green, shift, !theme->is_dark),
        shift_color(bg.blue,  shift, !theme->is_dark),
        1.0
    };

    /* Surface bg: between chrome and editor (for popovers, dialogs) */
    double shift2 = 0.02;
    GdkRGBA surface_bg = {
        shift_color(bg.red,   shift2, !theme->is_dark),
        shift_color(bg.green, shift2, !theme->is_dark),
        shift_color(bg.blue,  shift2, !theme->is_dark),
        1.0
    };

    /* Border: subtle separator */
    GdkRGBA border = {
        shift_color(bg.red,   0.08, !theme->is_dark),
        shift_color(bg.green, 0.08, !theme->is_dark),
        shift_color(bg.blue,  0.08, !theme->is_dark),
        0.5
    };

    /* Hover: slight brightness change */
    GdkRGBA hover_bg = {
        shift_color(bg.red,   0.06, !theme->is_dark),
        shift_color(bg.green, 0.06, !theme->is_dark),
        shift_color(bg.blue,  0.06, !theme->is_dark),
        1.0
    };

    /* Dim foreground for secondary text */
    GdkRGBA dim_fg = fg;
    dim_fg.alpha = 0.6;

    char c_chrome[64], c_surface[64], c_bg[64], c_fg[64], c_border[64], c_hover[64], c_dim[64];
    rgba_to_css(&chrome_bg, c_chrome, sizeof(c_chrome));
    rgba_to_css(&surface_bg, c_surface, sizeof(c_surface));
    rgba_to_css(&bg, c_bg, sizeof(c_bg));
    rgba_to_css(&fg, c_fg, sizeof(c_fg));
    rgba_to_css(&border, c_border, sizeof(c_border));
    rgba_to_css(&hover_bg, c_hover, sizeof(c_hover));
    rgba_to_css(&dim_fg, c_dim, sizeof(c_dim));

    /* --- Override GTK named colors so tab/tabbar CSS picks them up --- */
    g_string_append_printf(css,
        "@define-color headerbar_bg_color %s;\n"
        "@define-color headerbar_fg_color %s;\n"
        "@define-color window_bg_color %s;\n"
        "@define-color window_fg_color %s;\n"
        "@define-color view_bg_color %s;\n",
        c_chrome, c_fg, c_bg, c_fg, c_bg);

    /* --- Window / Root --- */
    g_string_append_printf(css,
        "window, window.background { background-color: %s; color: %s; }\n",
        c_bg, c_fg);

    /* --- Tab bar container: use chrome bg for consistency --- */
    g_string_append_printf(css,
        ".vite-tab-bar-container { background-color: %s; }\n",
        c_chrome);

    /* --- Headerbar --- */
    g_string_append_printf(css,
        "headerbar, headerbar.titlebar {"
        "  background-color: %s; color: %s;"
        "  border-bottom: 1px solid %s;"
        "}\n", c_chrome, c_fg, c_border);

    /* --- AdwToolbarView top/bottom bars --- */
    g_string_append_printf(css,
        ".toolbar-view .top-bar, .toolbar-view .bottom-bar {"
        "  background-color: %s; color: %s;"
        "}\n", c_chrome, c_fg);

    /* --- Status bar --- */
    g_string_append_printf(css,
        ".status-bar {"
        "  background-color: %s; color: %s;"
        "  border-top: 1px solid %s;"
        "}\n", c_chrome, c_fg, c_border);

    /* --- Find / Replace bar --- */
    g_string_append_printf(css,
        ".find-bar {"
        "  background-color: %s; color: %s;"
        "  border-top: 1px solid %s;"
        "}\n", c_chrome, c_fg, c_border);

    /* --- Popovers & Menus --- */
    g_string_append_printf(css,
        "popover > contents, popover.menu > contents {"
        "  background-color: %s; color: %s;"
        "}\n", c_surface, c_fg);

    g_string_append_printf(css,
        "popover > contents > modelbutton:hover, "
        "popover > contents > .item:hover, "
        "popover > contents modelbutton:hover, "
        "popover modelbutton:hover {"
        "  background-color: %s;"
        "}\n", c_hover);

    /* List row hover (file list, language selector, etc.) */
    g_string_append_printf(css,
        "list row:hover, "
        "listview row:hover, "
        ".navigation-sidebar row:hover {"
        "  background-color: alpha(%s, 0.08);"
        "}\n", c_fg);

    /* --- Tab bar area --- */
    g_string_append_printf(css,
        ".tab-bar { background-color: %s; color: %s; }\n",
        c_chrome, c_fg);

    /* --- Buttons: flat buttons in header, etc. --- */
    g_string_append_printf(css,
        "headerbar button.flat { color: %s; }\n"
        "headerbar button.flat:hover { background-color: %s; }\n"
        "headerbar button:not(.flat) { color: %s; }\n",
        c_fg, c_hover, c_fg);

    /* --- Text entries (search, find bar) --- */
    g_string_append_printf(css,
        "entry, searchentry, .find-bar entry {"
        "  background-color: %s; color: %s;"
        "  border-color: %s;"
        "}\n", c_bg, c_fg, c_border);

    /* --- Dim labels --- */
    g_string_append_printf(css,
        ".dim-label { color: %s; }\n", c_dim);

    /* --- Scrollbar --- */
    g_string_append_printf(css,
        "scrollbar slider {"
        "  background-color: %s;"
        "}\n"
        "scrollbar slider:hover {"
        "  background-color: %s;"
        "}\n",
        c_dim, c_fg);

    /* --- Preferences dialog --- */
    g_string_append_printf(css,
        "preferenceswindow, preferencespage, preferencesgroup, row {"
        "  background-color: transparent;"
        "}\n"
        "list { background-color: %s; }\n"
        ".navigation-sidebar { background-color: %s; }\n",
        c_surface, c_chrome);

    return g_string_free(css, FALSE);
}

/* --- Public API --- */

void
theme_manager_init(void)
{
    if (all_themes) return;
    all_themes = g_ptr_array_new_with_free_func((GDestroyNotify)theme_free);

    /* Scan multiple directories for themes */
    /* 1. Find themes relative to executable */
    char *exe_path = g_file_read_link("/proc/self/exe", NULL);
    if (exe_path) {
        char *bin_dir = g_path_get_dirname(exe_path);

        /* Installed location: $prefix/share/vite/themes/ */
        char *installed = g_build_filename(bin_dir, "..", "share", "vite", "themes", NULL);
        scan_theme_directory(installed);
        g_free(installed);

        /* Development: ../vscode-themes/ relative to binary */
        char *dev_themes = g_build_filename(bin_dir, "..", "vscode-themes", NULL);
        scan_theme_directory(dev_themes);
        g_free(dev_themes);

        g_free(bin_dir);
        g_free(exe_path);
    }

    /* 2. Current directory (for development) */
    {
        char *cwd = g_get_current_dir();
        char *cwd_themes = g_build_filename(cwd, "vscode-themes", NULL);
        /* Always scan CWD for development/overrides */
        scan_theme_directory(cwd_themes);
        g_free(cwd_themes);
        g_free(cwd);
    }

    /* 3. User themes: ~/.local/share/vite/themes/ */
    {
        char *user_themes = g_build_filename(g_get_user_data_dir(), "vite", "themes", NULL);
        scan_theme_directory(user_themes);
        g_free(user_themes);
    }

    /* Set up an initial default theme if none loaded */
    if (all_themes->len == 0) {
        g_warning("No theme files found; using built-in One Dark");
        ViteTheme *fallback = g_new0(ViteTheme, 1);
        fallback->name = g_strdup("One Dark (Built-in)");
        fallback->file_path = NULL;
        set_default_dark_theme(fallback);
        g_ptr_array_add(all_themes, fallback);
    }

    /* Apply saved theme or default */
    char *saved = theme_manager_load_selection();
    if (saved) {
        theme_manager_apply_theme(saved);
        g_free(saved);
    } else {
        /* Default to the first theme (usually OneDark) */
        ViteTheme *first = g_ptr_array_index(all_themes, 0);
        theme_manager_apply_theme(first->name);
    }
}

void
theme_manager_cleanup(void)
{
    if (current_css_provider) {
        gtk_style_context_remove_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(current_css_provider));
        g_object_unref(current_css_provider);
        current_css_provider = NULL;
    }

    if (all_themes) {
        g_ptr_array_unref(all_themes);
        all_themes = NULL;
    }
    current_theme = NULL;
}

int
theme_manager_get_count(void)
{
    return all_themes ? (int)all_themes->len : 0;
}

const char *
theme_manager_get_name(int index)
{
    if (!all_themes || index < 0 || (guint)index >= all_themes->len) return NULL;
    ViteTheme *t = g_ptr_array_index(all_themes, index);
    return t->name;
}

void
theme_manager_apply_theme(const char *theme_name)
{
    if (!all_themes || !theme_name) return;

    ViteTheme *target = NULL;
    for (guint i = 0; i < all_themes->len; i++) {
        ViteTheme *t = g_ptr_array_index(all_themes, i);
        if (g_strcmp0(t->name, theme_name) == 0) {
            target = t;
            break;
        }
    }

    if (!target) {
        g_warning("Theme '%s' not found", theme_name);
        return;
    }

    current_theme = target;

    /* Remove old CSS provider */
    if (current_css_provider) {
        gtk_style_context_remove_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(current_css_provider));
        g_object_unref(current_css_provider);
        current_css_provider = NULL;
    }

    AdwStyleManager *style_mgr = adw_style_manager_get_default();

    if (is_default_theme(target->name)) {
        /* Default themes: use native GTK4/Adwaita colors.
         * Skip generate_css() so @headerbar_bg_color, @window_fg_color, etc.
         * remain the standard Adwaita values. Only set the color scheme. */
        if (target->is_dark) {
            adw_style_manager_set_color_scheme(style_mgr, ADW_COLOR_SCHEME_FORCE_DARK);
        } else {
            adw_style_manager_set_color_scheme(style_mgr, ADW_COLOR_SCHEME_FORCE_LIGHT);
        }
    } else {
        /* Custom themes: generate and apply CSS for widget theming */
        char *css = generate_css(target);
        if (css && strlen(css) > 0) {
            current_css_provider = gtk_css_provider_new();
            gtk_css_provider_load_from_string(current_css_provider, css);
            gtk_style_context_add_provider_for_display(
                gdk_display_get_default(),
                GTK_STYLE_PROVIDER(current_css_provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
        g_free(css);

        /* Set Adw color scheme based on theme darkness */
        if (target->is_dark) {
            adw_style_manager_set_color_scheme(style_mgr, ADW_COLOR_SCHEME_FORCE_DARK);
        } else {
            adw_style_manager_set_color_scheme(style_mgr, ADW_COLOR_SCHEME_FORCE_LIGHT);
        }
    }
}

const ViteTheme *
theme_manager_get_current(void)
{
    return current_theme;
}

/* --- Persistence --- */

static char *
get_config_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "vite", "theme.conf", NULL);
}

void
theme_manager_save_selection(const char *theme_name)
{
    if (!theme_name) return;

    char *config_dir = g_build_filename(g_get_user_config_dir(), "vite", NULL);
    g_mkdir_with_parents(config_dir, 0755);
    g_free(config_dir);

    char *path = get_config_path();
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "Theme", "name", theme_name);

    GError *error = NULL;
    if (!g_key_file_save_to_file(kf, path, &error)) {
        g_warning("Could not save theme selection: %s", error->message);
        g_error_free(error);
    }

    g_key_file_free(kf);
    g_free(path);
}

char *
theme_manager_load_selection(void)
{
    char *path = get_config_path();
    GKeyFile *kf = g_key_file_new();
    char *result = NULL;

    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        result = g_key_file_get_string(kf, "Theme", "name", NULL);
    }

    g_key_file_free(kf);
    g_free(path);
    return result;
}