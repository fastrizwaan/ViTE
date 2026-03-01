#include "theme-manager.h"
#include <adwaita.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <math.h>
#include "editor-widget.h"

/* --- Static State --- */
static GPtrArray *all_themes = NULL;  /* ViteTheme* */
static ViteTheme *current_theme = NULL;
static GtkCssProvider *current_css_provider = NULL;
static guint64 theme_revision = 0;

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
    /* One Dark Pro Night Flat colors */
    gdk_rgba_parse(&theme->editor_bg, "#16191d");
    gdk_rgba_parse(&theme->editor_fg, "#abb2bf");
    gdk_rgba_parse(&theme->gutter_bg, "#16191d");
    gdk_rgba_parse(&theme->gutter_fg, "#667187");
    gdk_rgba_parse(&theme->gutter_active_fg, "#abb2bf");
    gdk_rgba_parse(&theme->line_highlight, "#2c313c50"); /* Use some alpha for highlight */
    gdk_rgba_parse(&theme->selection, "#67769660");
    gdk_rgba_parse(&theme->cursor_color, "#528bff");
    gdk_rgba_parse(&theme->find_match, "#528bff4d");
    gdk_rgba_parse(&theme->find_match_highlight, "#528bff4d");

    gdk_rgba_parse(&theme->tab_active_bg, "#23272e");
    gdk_rgba_parse(&theme->tab_active_fg, "#abb2bf");
    gdk_rgba_parse(&theme->tab_inactive_bg, "#16191d");
    gdk_rgba_parse(&theme->tab_inactive_fg, "#667187");
    gdk_rgba_parse(&theme->tab_border, "#101216");

    gdk_rgba_parse(&theme->titlebar_bg, "#16191d");
    gdk_rgba_parse(&theme->titlebar_fg, "#abb2bf");
    gdk_rgba_parse(&theme->statusbar_bg, "#16191d");
    gdk_rgba_parse(&theme->statusbar_fg, "#abb2bf");

    gdk_rgba_parse(&theme->scrollbar_bg, "#4e566680");
    gdk_rgba_parse(&theme->scrollbar_hover, "#5a637580");
    gdk_rgba_parse(&theme->scrollbar_active, "#747d9180");

    /* Syntax Colors */
    pango_color_parse(&theme->syntax[COLOR_KEYWORD], "#c678dd");
    pango_color_parse(&theme->syntax[COLOR_BUILTIN], "#56b6c2");
    pango_color_parse(&theme->syntax[COLOR_STRING], "#98c379");
    pango_color_parse(&theme->syntax[COLOR_COMMENT], "#5c6370");
    pango_color_parse(&theme->syntax[COLOR_NUMBER], "#d19a66");
    pango_color_parse(&theme->syntax[COLOR_FUNCTION], "#61afef");
    pango_color_parse(&theme->syntax[COLOR_TYPE], "#e5c07b");
    pango_color_parse(&theme->syntax[COLOR_DECORATOR], "#56b6c2");
    pango_color_parse(&theme->syntax[COLOR_VARIABLE], "#e06c75");
    pango_color_parse(&theme->syntax[COLOR_MEMBER], "#abb2bf");
    pango_color_parse(&theme->syntax[COLOR_CONSTANT], "#d19a66");
    pango_color_parse(&theme->syntax[COLOR_TAG], "#e06c75");
    pango_color_parse(&theme->syntax[COLOR_OPERATOR], "#56b6c2");
    pango_color_parse(&theme->syntax[COLOR_PUNCTUATION], "#abb2bf");
    pango_color_parse(&theme->syntax[COLOR_ATTRIBUTE], "#d19a66");
    pango_color_parse(&theme->syntax[COLOR_PARAM], "#d19a66");
    pango_color_parse(&theme->syntax[COLOR_PROPERTY], "#e06c75");
    pango_color_parse(&theme->syntax[COLOR_PREPROC], "#c678dd");
    pango_color_parse(&theme->syntax[COLOR_LOGICAL], "#56b6c2");

    pango_color_parse(&theme->syntax[COLOR_BRACKET_1], "#ffd700");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_2], "#da70d6");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_3], "#87cefa");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_4], "#ffd700");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_5], "#da70d6");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_6], "#87cefa");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_UNMATCHED], "#ff1111");

    theme->is_dark = TRUE;
}

static void
apply_theme_inheritance_and_fallback(ViteTheme *theme, int *slot_scores)
{
    /* Fallback inheritance: unset slots inherit from parent scope colors and styles.
     * This matches how VSCode inherits styles from broader scopes. */
    #define INHERIT(child, parent) \
        if (!theme->has_style_set[child] && theme->has_style_set[parent]) { \
            theme->syntax_style[child] = theme->syntax_style[parent]; \
        } \
        if ((!slot_scores || slot_scores[child] == 0) && (!slot_scores || slot_scores[parent] > 0)) { \
            theme->syntax[child] = theme->syntax[parent]; \
        }

    INHERIT(COLOR_PREPROC,    COLOR_KEYWORD)    /* preprocessor ← keyword */
    INHERIT(COLOR_OPERATOR,   COLOR_KEYWORD)    /* operator ← keyword */
    INHERIT(COLOR_LOGICAL,    COLOR_OPERATOR)   /* logical ← operator ← keyword */
    INHERIT(COLOR_LOGICAL,    COLOR_KEYWORD)    /* logical ← keyword (if operator also unset) */
    INHERIT(COLOR_MEMBER, COLOR_VARIABLE)   /* C member ← variable */
    INHERIT(COLOR_PROPERTY,   COLOR_VARIABLE)   /* property ← variable */
    INHERIT(COLOR_KEYWORD_CONTROL, COLOR_KEYWORD)/* control keyword ← keyword */
    INHERIT(COLOR_KEYWORD,    COLOR_KEYWORD_CONTROL)/* keyword ← control keyword */
    INHERIT(COLOR_CONSTANT,   COLOR_NUMBER)     /* constant ← number */
    INHERIT(COLOR_DECORATOR,  COLOR_FUNCTION)   /* decorator ← function */
    INHERIT(COLOR_BUILTIN,    COLOR_FUNCTION)   /* builtin ← function */
    
    INHERIT(COLOR_STORAGE,    COLOR_KEYWORD)    /* storage (primitive types) ← keyword */
    INHERIT(COLOR_CONSTANT_LANG, COLOR_CONSTANT)/* true/false ← constant */
    #undef INHERIT

    /* For any completely unset slots (even after inheritance), fall back to editor foreground */
    PangoColor fg_pango;
    fg_pango.red   = (guint16)(theme->editor_fg.red   * 65535);
    fg_pango.green = (guint16)(theme->editor_fg.green * 65535);
    fg_pango.blue  = (guint16)(theme->editor_fg.blue  * 65535);

    for (int i = 0; i < COLOR_SLOT_COUNT; i++) {
        if (i >= COLOR_BRACKET_1 && i <= COLOR_BRACKET_UNMATCHED) continue;
        
        if (!slot_scores) {
            /* If no slot_scores provided (built-in theme), we check if the slot is empty (all 0s) */
            if (theme->syntax[i].red == 0 && theme->syntax[i].green == 0 && theme->syntax[i].blue == 0) {
                 theme->syntax[i] = fg_pango;
            }
        } else if (slot_scores[i] == 0) {
            theme->syntax[i] = fg_pango;
        }
    }
}


static void
set_default_light_theme(ViteTheme *theme)
{
    /* Atom One Light Modern colors */
    gdk_rgba_parse(&theme->editor_bg, "#F2F2F2");
    gdk_rgba_parse(&theme->editor_fg, "#24292e");
    gdk_rgba_parse(&theme->gutter_bg, "#F2F2F2");
    gdk_rgba_parse(&theme->gutter_fg, "#1b1f234d");
    gdk_rgba_parse(&theme->gutter_active_fg, "#24292e");
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

    pango_color_parse(&theme->syntax[COLOR_KEYWORD], "#A626A4");
    pango_color_parse(&theme->syntax[COLOR_BUILTIN], "#0184BC");
    pango_color_parse(&theme->syntax[COLOR_STRING], "#50A14F");
    pango_color_parse(&theme->syntax[COLOR_COMMENT], "#A0A1A7");
    pango_color_parse(&theme->syntax[COLOR_NUMBER], "#986801");
    pango_color_parse(&theme->syntax[COLOR_FUNCTION], "#4078F2");
    pango_color_parse(&theme->syntax[COLOR_TYPE], "#C18401");
    pango_color_parse(&theme->syntax[COLOR_DECORATOR], "#A626A4");
    pango_color_parse(&theme->syntax[COLOR_VARIABLE], "#E45649");
    pango_color_parse(&theme->syntax[COLOR_MEMBER], "#383A42");
    pango_color_parse(&theme->syntax[COLOR_CONSTANT], "#986801");
    pango_color_parse(&theme->syntax[COLOR_TAG], "#E45649");
    pango_color_parse(&theme->syntax[COLOR_OPERATOR], "#383A42");
    pango_color_parse(&theme->syntax[COLOR_PUNCTUATION], "#383A42");
    pango_color_parse(&theme->syntax[COLOR_ATTRIBUTE], "#986801");
    pango_color_parse(&theme->syntax[COLOR_PARAM], "#383A42");
    pango_color_parse(&theme->syntax[COLOR_PROPERTY], "#696C77");
    pango_color_parse(&theme->syntax[COLOR_PREPROC], "#A626A4");
    pango_color_parse(&theme->syntax[COLOR_LOGICAL], "#383A42");

    pango_color_parse(&theme->syntax[COLOR_BRACKET_1], "#005cc5");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_2], "#e36209");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_3], "#5a32a3");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_4], "#005cc5");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_5], "#e36209");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_6], "#5a32a3");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_UNMATCHED], "#FF1111");

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
    { "keyword.operator.comparison",   COLOR_LOGICAL },
    { "keyword.operator",              COLOR_OPERATOR },
    { "keyword.control.directive",     COLOR_PREPROC },
    { "keyword.control",               COLOR_KEYWORD_CONTROL },
    { "keyword.other.unit",            COLOR_NUMBER },
    { "keyword",                       COLOR_KEYWORD },
    { "storage.type",                  COLOR_STORAGE },
    { "storage.modifier",              COLOR_STORAGE },
    { "storage",                       COLOR_STORAGE },
    { "constant.numeric",              COLOR_NUMBER },
    { "constant.language",             COLOR_CONSTANT_LANG },
    { "constant.character.escape",     COLOR_BUILTIN },
    { "constant.other.color",          COLOR_BUILTIN },
    { "constant.other.symbol",         COLOR_BUILTIN },
    { "constant.variable",             COLOR_NUMBER },
    { "constant",                      COLOR_CONSTANT },
    { "variable.parameter",            COLOR_PARAM },
    { "variable.interpolation",        COLOR_VARIABLE },
    { "variable.other.property",       COLOR_PROPERTY },
    { "variable.other.member",         COLOR_MEMBER },
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
    { "markup.italic",                 COLOR_PROPERTY },
    { "punctuation.definition.comment",COLOR_PUNCTUATION },
    { "punctuation.definition.string", COLOR_PUNCTUATION },
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

static SyntaxLanguage
detect_language_from_scope(const char *scope)
{
    /* Simple substring checks for common VSCode scope language suffixes/prefixes */
    if (g_str_has_suffix(scope, ".c") || strstr(scope, ".c ") || strstr(scope, "source.c")) return LANG_C;
    if (g_str_has_suffix(scope, ".cpp") || strstr(scope, ".cpp ") || strstr(scope, "source.cpp")) return LANG_C;
    if (g_str_has_suffix(scope, ".h") || strstr(scope, ".h ") || strstr(scope, "source.h")) return LANG_C;
    
    if (g_str_has_suffix(scope, ".python") || strstr(scope, ".python ") || strstr(scope, "source.python")) return LANG_PYTHON;
    if (g_str_has_suffix(scope, ".py") || strstr(scope, ".py ") || strstr(scope, "source.py")) return LANG_PYTHON;
    
    if (g_str_has_suffix(scope, ".js") || strstr(scope, ".js ") || strstr(scope, "source.js")) return LANG_JAVASCRIPT;
    if (g_str_has_suffix(scope, ".ts") || strstr(scope, ".ts ") || strstr(scope, "source.ts")) return LANG_JAVASCRIPT;
    
    if (g_str_has_suffix(scope, ".sh") || strstr(scope, ".sh ") || strstr(scope, "source.sh") || strstr(scope, "source.shell")) return LANG_BASH;
    
    if (g_str_has_suffix(scope, ".json") || strstr(scope, ".json ") || strstr(scope, "source.json")) return LANG_JSON;
    if (g_str_has_suffix(scope, ".yaml") || strstr(scope, ".yaml ") || strstr(scope, "source.yaml")) return LANG_YAML;
    if (g_str_has_suffix(scope, ".yml") || strstr(scope, ".yml ") || strstr(scope, "source.yml")) return LANG_YAML;
    if (g_str_has_suffix(scope, ".xml") || strstr(scope, ".xml ") || strstr(scope, "text.xml")) return LANG_XML;
    if (g_str_has_suffix(scope, ".html") || strstr(scope, ".html ") || strstr(scope, "text.html")) return LANG_XML;
    
    if (g_str_has_suffix(scope, ".rust") || strstr(scope, ".rust ") || strstr(scope, "source.rust")) return LANG_RUST;
    if (g_str_has_suffix(scope, ".rs") || strstr(scope, ".rs ") || strstr(scope, "source.rs")) return LANG_RUST;
    
    if (g_str_has_suffix(scope, ".markdown") || strstr(scope, ".markdown ") || strstr(scope, "text.html.markdown")) return LANG_MARKDOWN;
    if (g_str_has_suffix(scope, ".md") || strstr(scope, ".md ") || strstr(scope, "source.md")) return LANG_MARKDOWN;

    return LANG_NONE;
}

static void strip_lang_suffix(char *rule) {
    const char *suffixes[] = {
        ".cpp", ".python", ".yaml", ".rust", ".html", 
        ".json", ".xml", ".bash", ".shell", ".markdown",
        ".javascript",
        ".py", ".js", ".ts", ".sh", ".rs", ".md", ".c", ".h", 
        NULL
    };
    for (int i = 0; suffixes[i]; i++) {
        if (g_str_has_suffix(rule, suffixes[i])) {
            rule[strlen(rule) - strlen(suffixes[i])] = '\0';
            return;
        }
    }
}

/* Apply a single JSON/YAML rule to all map slots that subclass it.
 * E.g. json rule "keyword" applies to map slot "keyword.control" because
 * "keyword.control" starts with "keyword". */
static void
apply_rule_to_slots(const char *json_rule, const PangoColor *color, guint8 style_mask, gboolean has_style, ViteTheme *theme, int *slot_scores, int slot_scores_lang[VITE_LANG_COUNT][COLOR_SLOT_COUNT])
{
    SyntaxLanguage lang = detect_language_from_scope(json_rule);

    char **parts = NULL;
    const char *actual_rule = json_rule;
    int penalty = 1;

    /* If scope contains spaces (compound scope like "source.c keyword.operator"),
     * try the last part. We penalize compound matches so generic definitions will win. */
    if (strchr(json_rule, ' ')) {
        parts = g_strsplit(json_rule, " ", -1);
        int last_part = -1;
        for (int p = 0; parts[p]; p++) {
            char *trimmed = g_strstrip(parts[p]);
            if (trimmed[0] != '\0' && trimmed[0] != '>') {
                last_part = p;
            }
        }
        if (last_part >= 0) {
            actual_rule = parts[last_part];
        }
        if (lang == LANG_NONE) penalty = 4;
    }

    char *trimmed_rule = g_strstrip(g_strdup(actual_rule));
    if (lang != LANG_NONE) {
        strip_lang_suffix(trimmed_rule);
    }

    for (int i = 0; scope_map[i].scope != NULL; i++) {
        /* Does our mapped scope start with the JSON rule? */
        if (scope_starts_with(scope_map[i].scope, trimmed_rule)) {
            int score = (strlen(trimmed_rule) * 1000) / strlen(scope_map[i].scope);
            score /= penalty;

            ViteColorSlot slot = scope_map[i].slot;
            if (lang != LANG_NONE) {
                if (score > 0 && score >= slot_scores_lang[lang][slot]) {
                    if (color) {
                        theme->syntax_lang[lang][slot] = *color;
                        theme->has_lang_syntax[lang][slot] = TRUE;
                    }
                    if (has_style) {
                        theme->syntax_lang_style[lang][slot] = style_mask;
                        theme->has_lang_style_set[lang][slot] = TRUE;
                    }
                    slot_scores_lang[lang][slot] = score;
                }
            } else {
                if (score > 0 && score >= slot_scores[slot]) {
                    if (color) {
                        theme->syntax[slot] = *color;
                    }
                    if (has_style) {
                        theme->syntax_style[slot] = style_mask;
                        theme->has_style_set[slot] = TRUE;
                    }
                    slot_scores[slot] = score;
                }
            }
        }
    }

    g_free(trimmed_rule);
    if (parts) g_strfreev(parts);
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

    /* Bracket Pair Defaults (VSCode standards) */
    pango_color_parse(&theme->syntax[COLOR_BRACKET_1], "#FFD700"); /* Yellow */
    pango_color_parse(&theme->syntax[COLOR_BRACKET_2], "#DA70D6"); /* Fuchsia */
    pango_color_parse(&theme->syntax[COLOR_BRACKET_3], "#87CEFA"); /* Blue */
    pango_color_parse(&theme->syntax[COLOR_BRACKET_4], "#FFD700");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_5], "#DA70D6");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_6], "#87CEFA");
    pango_color_parse(&theme->syntax[COLOR_BRACKET_UNMATCHED], "#FF1111");

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

    /* Bracket Pair Customizations */
    if ((val = json_object_get_string_safe(colors, "editorBracketHighlight.foreground1")))
        pango_color_parse(&theme->syntax[COLOR_BRACKET_1], val);
    if ((val = json_object_get_string_safe(colors, "editorBracketHighlight.foreground2")))
        pango_color_parse(&theme->syntax[COLOR_BRACKET_2], val);
    if ((val = json_object_get_string_safe(colors, "editorBracketHighlight.foreground3")))
        pango_color_parse(&theme->syntax[COLOR_BRACKET_3], val);
    if ((val = json_object_get_string_safe(colors, "editorBracketHighlight.foreground4")))
        pango_color_parse(&theme->syntax[COLOR_BRACKET_4], val);
    if ((val = json_object_get_string_safe(colors, "editorBracketHighlight.foreground5")))
        pango_color_parse(&theme->syntax[COLOR_BRACKET_5], val);
    if ((val = json_object_get_string_safe(colors, "editorBracketHighlight.foreground6")))
        pango_color_parse(&theme->syntax[COLOR_BRACKET_6], val);
    if ((val = json_object_get_string_safe(colors, "editorBracketHighlight.unexpectedBracket.foreground")))
        pango_color_parse(&theme->syntax[COLOR_BRACKET_UNMATCHED], val);
}

static void
parse_token_colors(JsonArray *token_colors, ViteTheme *theme, int *slot_scores_out)
{
    /* Track which slots have been set using their best score */
    int slot_scores[COLOR_SLOT_COUNT];
    memset(slot_scores, 0, sizeof(slot_scores));

    int slot_scores_lang[VITE_LANG_COUNT][COLOR_SLOT_COUNT];
    memset(slot_scores_lang, 0, sizeof(slot_scores_lang));

    guint n = json_array_get_length(token_colors);
    for (guint i = 0; i < n; i++) {
        JsonNode *elem = json_array_get_element(token_colors, i);
        if (!JSON_NODE_HOLDS_OBJECT(elem)) continue;
        JsonObject *rule = json_node_get_object(elem);

        /* Get settings */
        if (!json_object_has_member(rule, "settings")) continue;
        JsonObject *settings = json_object_get_object_member(rule, "settings");
        if (!settings) continue;
        
        const char *fg = json_object_get_string_safe(settings, "foreground");
        PangoColor color;
        PangoColor *color_ptr = NULL;
        if (fg && parse_hex_color_to_pango(fg, &color)) {
            color_ptr = &color;
        }

        const char *fs = json_object_get_string_safe(settings, "fontStyle");
        guint8 style_mask = 0;
        gboolean has_style = FALSE;
        if (fs) {
            has_style = TRUE;
            if (strstr(fs, "bold")) style_mask |= VITE_FONT_STYLE_BOLD;
            if (strstr(fs, "italic")) style_mask |= VITE_FONT_STYLE_ITALIC;
            if (strstr(fs, "underline")) style_mask |= VITE_FONT_STYLE_UNDERLINE;
        }

        if (!color_ptr && !has_style) continue;

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
            apply_rule_to_slots(scope_str, color_ptr, style_mask, has_style, theme, slot_scores, slot_scores_lang);
        }
        g_ptr_array_free(scopes, TRUE);
    }

    /* Copy out which slots were set */
    if (slot_scores_out) {
        memcpy(slot_scores_out, slot_scores, sizeof(slot_scores));
    }
}

static gboolean
detect_is_dark(const GdkRGBA *bg)
{
    double lum = bg->red * 0.299 + bg->green * 0.587 + bg->blue * 0.114;
    return lum < 0.5;
}

/* Removes JSON comments (single/multi) and trailing commas to allow json-glib to parse VSCode Themes */
static char *
clean_json_string(const char *input)
{
    size_t len = strlen(input);
    char *out = g_malloc(len + 1);
    size_t j = 0;
    gboolean in_string = FALSE;
    gboolean in_single_line_comment = FALSE;
    gboolean in_multi_line_comment = FALSE;
    
    for (size_t i = 0; i < len; i++) {
        if (in_single_line_comment) {
            if (input[i] == '\n') {
                in_single_line_comment = FALSE;
                out[j++] = input[i]; /* preserve line numbers */
            }
            continue;
        }
        if (in_multi_line_comment) {
            if (input[i] == '*' && i + 1 < len && input[i+1] == '/') {
                in_multi_line_comment = FALSE;
                i++;
            } else if (input[i] == '\n') {
                out[j++] = input[i]; /* preserve line numbers */
            }
            continue;
        }
        if (!in_string) {
            if (input[i] == '/' && i + 1 < len) {
                if (input[i+1] == '/') {
                    in_single_line_comment = TRUE;
                    i++;
                    continue;
                } else if (input[i+1] == '*') {
                    in_multi_line_comment = TRUE;
                    i++;
                    continue;
                }
            }
        }
        if (input[i] == '"' && (i == 0 || input[i-1] != '\\' || (i > 1 && input[i-2] == '\\'))) {
            in_string = !in_string;
        }
        out[j++] = input[i];
    }
    out[j] = '\0';
    
    /* Second pass: Remove trailing commas before } or ] */
    for (size_t i = 0; i < j; i++) {
        if (out[i] == ',') {
            /* Look ahead for } or ] */
            size_t k = i + 1;
            while (k < j && g_ascii_isspace(out[k])) k++;
            if (k < j && (out[k] == '}' || out[k] == ']')) {
                out[i] = ' '; /* Erase trailing comma */
            }
        }
    }
    return out;
}

static ViteTheme *
load_theme_from_json(const char *path)
{
    GError *error = NULL;
    char *contents = NULL;
    gsize length = 0;

    if (!g_file_get_contents(path, &contents, &length, &error)) {
        g_warning("Could not read theme file '%s': %s", path, error->message);
        g_error_free(error);
        return NULL;
    }

    char *cleaned_json = clean_json_string(contents);
    g_free(contents);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, cleaned_json, -1, &error)) {
        g_warning("Could not parse theme '%s': %s", path, error->message);
        g_error_free(error);
        g_free(cleaned_json);
        g_object_unref(parser);
        return NULL;
    }
    g_free(cleaned_json);

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
     * don't leak hardcoded One Dark/Light fallback colors.
     * Handled by apply_theme_inheritance_and_fallback. */

    /* Parse token colors for syntax */
    int slot_scores[COLOR_SLOT_COUNT];
    memset(slot_scores, 0, sizeof(slot_scores));
    if (json_object_has_member(obj, "tokenColors")) {
        JsonArray *token_colors = json_object_get_array_member(obj, "tokenColors");
        if (token_colors) parse_token_colors(token_colors, theme, slot_scores);
    }

    apply_theme_inheritance_and_fallback(theme, slot_scores);

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

static ViteTheme *
load_theme_from_yaml(const char *path)
{
    GError *error = NULL;
    char *contents = NULL;
    gsize length = 0;

    if (!g_file_get_contents(path, &contents, &length, &error)) {
        g_warning("Could not read yaml theme file '%s': %s", path, error->message);
        g_error_free(error);
        return NULL;
    }

    ViteTheme *theme = g_new0(ViteTheme, 1);
    theme->file_path = g_strdup(path);
    /* default dark for now */
    set_default_dark_theme(theme);

    /* Very simple line-by-line parser */
    char **lines = g_strsplit(contents, "\n", -1);
    g_free(contents);
    
    int state = 0; /* 0: root, 1: ui, 2: syntax, 3: syntax_lang */
    char current_lang_str[64] = {0};
    SyntaxLanguage current_lang = LANG_NONE;
    int slot_scores[COLOR_SLOT_COUNT] = {0};
    int slot_scores_lang[VITE_LANG_COUNT][COLOR_SLOT_COUNT] = {{0}};
    
    for (int i = 0; lines[i] != NULL; i++) {
        char *line = lines[i];
        /* strip comments: only if '#' is preceded by space, or is the first char */
        char *comment = strchr(line, '#');
        while (comment) {
            if (comment == line || g_ascii_isspace(*(comment - 1))) {
                *comment = '\0';
                break;
            }
            comment = strchr(comment + 1, '#');
        }
        
        char *trimmed = g_strstrip(g_strdup(line));
        if (trimmed[0] == '\0') {
            g_free(trimmed);
            continue;
        }
        
        /* Count indentation of original line */
        int indent = 0;
        while (line[indent] == ' ') indent++;
        
        /* state transitions based on indent */
        if (indent == 0) state = 0;
        else if (indent <= 2 && state == 3) state = 2; /* back from lang to syntax */
        
        if (g_str_has_suffix(trimmed, ":")) {
            trimmed[strlen(trimmed)-1] = '\0'; /* remove colon */
            if (indent == 0) {
                if (g_strcmp0(trimmed, "ui") == 0) state = 1;
                else if (g_strcmp0(trimmed, "syntax") == 0) state = 2;
                else state = 0;
            } else if (indent == 2 && state >= 2) {
                if (g_strcmp0(trimmed, "common") == 0) {
                    current_lang = LANG_NONE;
                    state = 2;
                } else {
                    state = 3;
                    strncpy(current_lang_str, trimmed, sizeof(current_lang_str)-1);
                    /* convert "C/C++" from yaml to "c" etc. if needed */
                    if (g_ascii_strcasecmp(trimmed, "python") == 0) strcpy(current_lang_str, "python");
                    else if (g_ascii_strcasecmp(trimmed, "c/c++") == 0 || g_ascii_strcasecmp(trimmed, "c") == 0) strcpy(current_lang_str, "c");
                    else strcpy(current_lang_str, trimmed);

                    current_lang = detect_language_from_scope(current_lang_str);
                }
            }
            g_free(trimmed);
            continue;
        }
        
        /* key-value pairs */
        char **kv = g_strsplit(trimmed, ":", 2);
        if (kv[0] && kv[1]) {
            char *key = g_strstrip(kv[0]);
            char *val = g_strstrip(kv[1]);
            
            /* remove quotes from val */
            if (val[0] == '"' || val[0] == '\'') {
                val++;
                if (val[strlen(val)-1] == '"' || val[strlen(val)-1] == '\'') {
                    val[strlen(val)-1] = '\0';
                }
            }
            
            if (state == 0) {
                if (g_strcmp0(key, "name") == 0) {
                    theme->name = g_strdup(val);
                } else if (g_strcmp0(key, "is_dark") == 0) {
                    if (g_strcmp0(val, "true") == 0) {
                        set_default_dark_theme(theme);
                        theme->is_dark = TRUE;
                    } else if (g_strcmp0(val, "false") == 0) {
                        set_default_light_theme(theme);
                        theme->is_dark = FALSE;
                    }
                }
            } else if (state == 1) { /* ui */
                if (g_strcmp0(key, "editor_bg") == 0 || g_strcmp0(key, "background") == 0) parse_hex_color_to_rgba(val, &theme->editor_bg);
                else if (g_strcmp0(key, "editor_fg") == 0 || g_strcmp0(key, "foreground") == 0) parse_hex_color_to_rgba(val, &theme->editor_fg);
                else if (g_strcmp0(key, "gutter_bg") == 0) parse_hex_color_to_rgba(val, &theme->gutter_bg);
                else if (g_strcmp0(key, "gutter_fg") == 0) parse_hex_color_to_rgba(val, &theme->gutter_fg);
                else if (g_strcmp0(key, "gutter_active_fg") == 0) parse_hex_color_to_rgba(val, &theme->gutter_active_fg);
                else if (g_strcmp0(key, "line_highlight") == 0) parse_hex_color_to_rgba(val, &theme->line_highlight);
                else if (g_strcmp0(key, "selection") == 0 || g_strcmp0(key, "accent") == 0) parse_hex_color_to_rgba(val, &theme->selection);
                else if (g_strcmp0(key, "cursor_color") == 0) parse_hex_color_to_rgba(val, &theme->cursor_color);
                else if (g_strcmp0(key, "find_match") == 0) parse_hex_color_to_rgba(val, &theme->find_match);
                else if (g_strcmp0(key, "find_match_highlight") == 0) parse_hex_color_to_rgba(val, &theme->find_match_highlight);
                
                else if (g_strcmp0(key, "tab_active_bg") == 0 || g_strcmp0(key, "tab") == 0) parse_hex_color_to_rgba(val, &theme->tab_active_bg);
                else if (g_strcmp0(key, "tab_active_fg") == 0) parse_hex_color_to_rgba(val, &theme->tab_active_fg);
                else if (g_strcmp0(key, "tab_inactive_bg") == 0) parse_hex_color_to_rgba(val, &theme->tab_inactive_bg);
                else if (g_strcmp0(key, "tab_inactive_fg") == 0) parse_hex_color_to_rgba(val, &theme->tab_inactive_fg);
                else if (g_strcmp0(key, "tab_border") == 0 || g_strcmp0(key, "tab close button") == 0) parse_hex_color_to_rgba(val, &theme->tab_border);
                
                else if (g_strcmp0(key, "window") == 0 || g_strcmp0(key, "titlebar_bg") == 0) parse_hex_color_to_rgba(val, &theme->titlebar_bg);
                else if (g_strcmp0(key, "titlebar_fg") == 0) parse_hex_color_to_rgba(val, &theme->titlebar_fg);
                else if (g_strcmp0(key, "statusbar_bg") == 0) parse_hex_color_to_rgba(val, &theme->statusbar_bg);
                else if (g_strcmp0(key, "statusbar_fg") == 0) parse_hex_color_to_rgba(val, &theme->statusbar_fg);
                else if (g_strcmp0(key, "scrollbar_bg") == 0) parse_hex_color_to_rgba(val, &theme->scrollbar_bg);
                else if (g_strcmp0(key, "scrollbar_hover") == 0 || g_strcmp0(key, "hover tab") == 0) parse_hex_color_to_rgba(val, &theme->scrollbar_hover);
                else if (g_strcmp0(key, "scrollbar_active") == 0) parse_hex_color_to_rgba(val, &theme->scrollbar_active);
            } else if (state >= 2) { /* syntax */
                PangoColor color;
                PangoColor *color_ptr = NULL;
                guint8 style_mask = 0;
                gboolean has_style = FALSE;
                
                char *val_copy = g_strdup(val);
                char *token = strtok(val_copy, " ");
                while (token) {
                    if (token[0] == '#' && parse_hex_color_to_pango(token, &color)) {
                        color_ptr = &color;
                    } else if (g_ascii_strcasecmp(token, "bold") == 0) {
                        style_mask |= VITE_FONT_STYLE_BOLD;
                        has_style = TRUE;
                    } else if (g_ascii_strcasecmp(token, "italic") == 0) {
                        style_mask |= VITE_FONT_STYLE_ITALIC;
                        has_style = TRUE;
                    } else if (g_ascii_strcasecmp(token, "underline") == 0) {
                        style_mask |= VITE_FONT_STYLE_UNDERLINE;
                        has_style = TRUE;
                    }
                    token = strtok(NULL, " ");
                }
                g_free(val_copy);

                if (color_ptr || has_style) {
                    if (state == 2 || current_lang == LANG_NONE) { /* common syntax */
                        apply_rule_to_slots(key, color_ptr, style_mask, has_style, theme, slot_scores, slot_scores_lang);
                    } else if (state == 3 && current_lang != LANG_NONE) {
                        /* Create a scoped rule equivalent like 'keyword.python' */
                        char *scoped_key = g_strdup_printf("%s.%s", key, current_lang_str);
                        apply_rule_to_slots(scoped_key, color_ptr, style_mask, has_style, theme, slot_scores, slot_scores_lang);
                        g_free(scoped_key);
                    }
                }
            }
        }
        g_strfreev(kv);
        g_free(trimmed);
    }
    
    g_strfreev(lines);
    
    if (!theme->name) {
        char *basename = g_path_get_basename(path);
        char *dotpos = strrchr(basename, '.');
        if (dotpos) *dotpos = '\0';
        theme->name = g_strdup(basename);
        g_free(basename);
    }
    
    theme->gutter_bg = theme->editor_bg; // map gutter bg
    apply_theme_inheritance_and_fallback(theme, slot_scores);
    theme->is_dark = detect_is_dark(&theme->editor_bg);
    
    return theme;
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
        if (g_strcmp0(filename, ".") == 0 || g_strcmp0(filename, "..") == 0) continue;

        char *full_path = g_build_filename(dir_path, filename, NULL);
        
        if (g_file_test(full_path, G_FILE_TEST_IS_DIR)) {
            scan_theme_directory(full_path);
        } else if (g_str_has_suffix(filename, ".json")) {
            ViteTheme *theme = load_theme_from_json(full_path);
            if (theme) {
                g_ptr_array_add(all_themes, theme);
            }
        } else if (g_str_has_suffix(filename, ".yaml") || g_str_has_suffix(filename, ".yml")) {
            ViteTheme *theme = load_theme_from_yaml(full_path);
            if (theme) {
                g_ptr_array_add(all_themes, theme);
            }
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
    double shift = 0.02;
    GdkRGBA chrome_bg = {
        shift_color(bg.red,   shift, !theme->is_dark),
        shift_color(bg.green, shift, !theme->is_dark),
        shift_color(bg.blue,  shift, !theme->is_dark),
        1.0
    };

    /* Surface bg: between chrome and editor (for popovers, dialogs) */
    double shift2 = 0.04;
    GdkRGBA surface_bg = {
        shift_color(bg.red,   shift2, !theme->is_dark),
        shift_color(bg.green, shift2, !theme->is_dark),
        shift_color(bg.blue,  shift2, !theme->is_dark),
        1.0
    };

    /* Border: subtle separator */
    GdkRGBA border = {
        shift_color(bg.red,   0.10, !theme->is_dark),
        shift_color(bg.green, 0.10, !theme->is_dark),
        shift_color(bg.blue,  0.10, !theme->is_dark),
        0.5
    };

    /* Hover: slight brightness change */
    GdkRGBA hover_bg = {
        shift_color(bg.red,   0.10, !theme->is_dark),
        shift_color(bg.green, 0.10, !theme->is_dark),
        shift_color(bg.blue,  0.10, !theme->is_dark),
        1.0
    };

    /* Dim foreground for secondary text */
    GdkRGBA dim_fg = fg;
    dim_fg.alpha = 0.9;

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
        "@define-color view_bg_color %s;\n"
        "@define-color dialog_bg_color %s;\n"
        "@define-color dialog_fg_color %s;\n"
        "@define-color popover_bg_color %s;\n"
        "@define-color popover_fg_color %s;\n",
        c_chrome, c_fg, c_bg, c_fg, c_bg,
        c_surface, c_fg, c_surface, c_fg);

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
        "  border-bottom: none;"
        "  box-shadow: none;"
        "}\n", c_chrome, c_fg);

    /* --- AdwToolbarView top/bottom bars --- */
    g_string_append_printf(css,
        ".toolbar-view .top-bar, .toolbar-view .bottom-bar {"
        "  background-color: %s; color: %s;"
        "  border: none;"
        "  box-shadow: none;"
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

static void
invalidate_editors_recursive(GtkWidget *widget)
{
    if (!widget) return;
    if (EDITOR_IS_WIDGET(widget)) {
        EditorWidget *ed = EDITOR_WIDGET(widget);
        SyntaxContext *ctx = editor_widget_get_syntax_context(ed);
        if (ctx) syntax_context_invalidate_cache(ctx);
        gtk_widget_queue_draw(widget);
        return;
    }

    GtkWidget *child = gtk_widget_get_first_child(widget);
    while (child) {
        invalidate_editors_recursive(child);
        child = gtk_widget_get_next_sibling(child);
    }
}

static void
on_system_theme_changed(AdwStyleManager *style_mgr G_GNUC_UNUSED, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    /* Only auto-switch if we are specifically using the Auto theme */
    if (current_theme && g_strcmp0(current_theme->name, "ViTE Built-In (Auto)") == 0) {
        /* Re-apply the Auto theme. theme_manager_apply_theme will check system
         * state and copy the correct Light/Dark values into the target Auto theme struct */
        theme_manager_apply_theme("ViTE Built-In (Auto)");

        /* Invalidate all open editors so they redraw with the updated target colors */
        GListModel *toplevels = gtk_window_get_toplevels();
        guint n_windows = g_list_model_get_n_items(toplevels);
        for (guint i = 0; i < n_windows; i++) {
            GObject *win = g_list_model_get_item(toplevels, i);
            if (GTK_IS_WIDGET(win)) {
                invalidate_editors_recursive(GTK_WIDGET(win));
                gtk_widget_queue_draw(GTK_WIDGET(win));
            }
            g_object_unref(win);
        }
    }
}

void
theme_manager_init(void)
{
    if (all_themes) return;
    all_themes = g_ptr_array_new_with_free_func((GDestroyNotify)theme_free);

    /* ALWAYS set up initial built-in themes first */
    ViteTheme *fallback_auto = g_new0(ViteTheme, 1);
    fallback_auto->name = g_strdup("ViTE Built-In (Auto)");
    fallback_auto->file_path = NULL;
    /* Start with dark as dummy, will be overridden upon apply */
    set_default_dark_theme(fallback_auto);
    apply_theme_inheritance_and_fallback(fallback_auto, NULL);
    g_ptr_array_add(all_themes, fallback_auto);

    ViteTheme *fallback_dark = g_new0(ViteTheme, 1);
    fallback_dark->name = g_strdup("One Dark (Built-in)");
    fallback_dark->file_path = NULL;
    set_default_dark_theme(fallback_dark);
    apply_theme_inheritance_and_fallback(fallback_dark, NULL);
    g_ptr_array_add(all_themes, fallback_dark);
    
    ViteTheme *fallback_light = g_new0(ViteTheme, 1);
    fallback_light->name = g_strdup("One Light (Built-in)");
    fallback_light->file_path = NULL;
    set_default_light_theme(fallback_light);
    apply_theme_inheritance_and_fallback(fallback_light, NULL);
    g_ptr_array_add(all_themes, fallback_light);

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

    /* Print a warning ONLY if no external themes were loaded (we check for 3 built-ins) */
    if (all_themes->len <= 3) {
        g_warning("No external theme files found; using only built-in themes");
    }

    AdwStyleManager *style_mgr = adw_style_manager_get_default();

    /* Apply saved theme or default */
    char *saved = theme_manager_load_selection();
    if (saved) {
        theme_manager_apply_theme(saved);
        g_free(saved);
    } else {
        /* Default to Auto (the first item we added) */
        ViteTheme *first = g_ptr_array_index(all_themes, 0);
        theme_manager_apply_theme(first->name);
    }
    
    /* Auto-sync with system theme if using built-ins */
    g_signal_connect(style_mgr, "notify::dark", G_CALLBACK(on_system_theme_changed), NULL);
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
        g_warning("Theme '%s' not found, falling back to 'ViTE Built-In (Auto)'", theme_name);
        if (g_strcmp0(theme_name, "ViTE Built-In (Auto)") != 0) {
            theme_manager_apply_theme("ViTE Built-In (Auto)");
        }
        return;
    }

    AdwStyleManager *style_mgr = adw_style_manager_get_default();

    /* Intercept "ViTE Built-In (Auto)" and dynamically update its payload
     * to mirror Either One Dark or One Light based on system dark mode. */
    if (g_strcmp0(target->name, "ViTE Built-In (Auto)") == 0) {
        if (adw_style_manager_get_color_scheme(style_mgr) != ADW_COLOR_SCHEME_DEFAULT) {
            adw_style_manager_set_color_scheme(style_mgr, ADW_COLOR_SCHEME_DEFAULT);
        }
        gboolean is_dark = adw_style_manager_get_dark(style_mgr);
        if (is_dark) {
            set_default_dark_theme(target);
        } else {
            set_default_light_theme(target);
        }
        apply_theme_inheritance_and_fallback(target, NULL);
    }

    theme_revision++;
    current_theme = target;

    /* Remove old CSS provider */
    if (current_css_provider) {
        gtk_style_context_remove_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(current_css_provider));
        g_object_unref(current_css_provider);
        current_css_provider = NULL;
    }

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
        if (g_strcmp0(target->name, "ViTE Built-In (Auto)") == 0) {
            adw_style_manager_set_color_scheme(style_mgr, ADW_COLOR_SCHEME_DEFAULT);
        } else if (target->is_dark) {
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

guint64
theme_manager_get_revision(void)
{
    return theme_revision;
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
