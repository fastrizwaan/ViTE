#ifndef SYNTAX_INTERNAL_H
#define SYNTAX_INTERNAL_H

#include "syntax.h"
#include "theme-manager.h"

/* --- Internal Structures --- */

struct _SyntaxContext {
    int ref_count;
    SyntaxLanguage lang;
    
    /* State tracking: index i = state AFTER line i */
    GByteArray *state_chain;

    /* Cache: maps line_index -> SyntaxCacheEntry* */
    GPtrArray *line_cache;
    
    /* Regexes (Shared or Specific) */
    /* Bash */
    GRegex *sh_keywords;
    GRegex *sh_builtins;
    GRegex *sh_comment;
    GRegex *sh_variable;
    GRegex *sh_string;
    GRegex *sh_string_sq;

    /* Javascript */
    GRegex *js_keywords; /* Just re-using for list check mostly */
    
    /* XML */
    GRegex *xml_tag_open;
    GRegex *xml_tag_close;
    GRegex *xml_attr;
    GRegex *xml_comment_start;
    GRegex *xml_comment_end;
    GRegex *xml_cdata_start;
    GRegex *xml_cdata_end;

    /* Desktop Entry */
    GRegex *desktop_comment;
    GRegex *desktop_section; /* [Section] */
    GRegex *desktop_key;     /* Key= */
    GRegex *desktop_arg;     /* %f %u etc */
    GRegex *desktop_string_dq;
    GRegex *desktop_string_sq;

    size_t valid_up_to;  /* Number of lines with valid state in state_chain */
};

/* Cache entry structure */
typedef struct {
    guint content_hash;
    SyntaxState start_state;
    PangoAttrList *attrs;
} SyntaxCacheEntry;

/* --- Color System (Theme-Driven) --- */

/* Add a syntax-colored attribute using a ViteColorSlot.
   Looks up the actual color from the current theme. */
void add_color_attr(PangoAttrList *attrs, int start, int end, ViteColorSlot slot);

/* --- Helper Functions --- */
void set_line_end_state(SyntaxContext *ctx, size_t line_index, SyntaxState state);
void set_line_end_state_with_depth(SyntaxContext *ctx, size_t line_index, SyntaxState state, int bracket_depth);
SyntaxState get_line_start_state(SyntaxContext *ctx, size_t line_index);
int get_line_start_bracket_depth(SyntaxContext *ctx, size_t line_index);

gboolean is_all_caps(const char *s, size_t len);
gboolean is_word_in_list(const char *word, size_t len, const char **list);

/* Language Specific Handlers */
void syntax_highlight_c(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index);
void syntax_highlight_python(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index);
void syntax_highlight_bash(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index);
void syntax_highlight_js(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index);
void syntax_highlight_json(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index);
void syntax_highlight_yaml(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index);
void syntax_highlight_xml(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index);
void syntax_highlight_desktop(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index);
void syntax_highlight_rust(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index);
void syntax_highlight_markdown(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index);

#endif
