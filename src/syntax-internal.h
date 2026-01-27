#ifndef SYNTAX_INTERNAL_H
#define SYNTAX_INTERNAL_H

#include "syntax.h"

/* --- Internal Structures --- */

struct _SyntaxContext {
    int ref_count;
    SyntaxLanguage lang;
    
    /* State tracking: index i = state AFTER line i */
    GByteArray *state_chain;

    /* Cache: maps line_index -> {content_hash, PangoAttrList*} */
    GHashTable *line_cache;  /* size_t -> SyntaxCacheEntry* */
    
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
};

/* Cache entry structure */
typedef struct {
    guint content_hash;
    SyntaxState start_state;
    PangoAttrList *attrs;
} SyntaxCacheEntry;

/* --- Colors (Shared) --- */
extern gboolean is_dark_mode;

/* Dark Theme */
extern PangoColor d_keyword;
extern PangoColor d_builtin;
extern PangoColor d_string;
extern PangoColor d_comment;
extern PangoColor d_number;
extern PangoColor d_function;
extern PangoColor d_type;
extern PangoColor d_decorator;
extern PangoColor d_variable;
extern PangoColor d_variable_c;
extern PangoColor d_constant;
extern PangoColor d_tag;
extern PangoColor d_operator;
extern PangoColor d_punctuation;
extern PangoColor d_attribute;
extern PangoColor d_param;
extern PangoColor d_property;
extern PangoColor d_preproc;
extern PangoColor d_logical;

/* Light Theme */
extern PangoColor l_keyword;
extern PangoColor l_builtin;
extern PangoColor l_string;
extern PangoColor l_comment;
extern PangoColor l_number;
extern PangoColor l_operator;
extern PangoColor l_punctuation;
extern PangoColor l_function;
extern PangoColor l_type;
extern PangoColor l_decorator;
extern PangoColor l_variable;
extern PangoColor l_variable_c;
extern PangoColor l_constant;
extern PangoColor l_tag;
extern PangoColor l_attribute;
extern PangoColor l_param;
extern PangoColor l_property;
extern PangoColor l_preproc;
extern PangoColor l_logical;

/* --- Helper Functions --- */

void add_attr(PangoAttrList *attrs, int start, int end, const PangoColor *color_ref);
void set_line_end_state(SyntaxContext *ctx, size_t line_index, SyntaxState state);
SyntaxState get_line_start_state(SyntaxContext *ctx, size_t line_index);

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

#endif
