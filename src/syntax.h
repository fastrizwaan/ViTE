#ifndef SYNTAX_H
#define SYNTAX_H

#include <gtk/gtk.h>

typedef enum {
    LANG_NONE,
    LANG_C,
    LANG_PYTHON,
    LANG_BASH,
    LANG_JAVASCRIPT,
    LANG_JSON,
    LANG_YAML,
    LANG_XML,
    LANG_DESKTOP,
    LANG_RUST,
    LANG_MARKDOWN
} SyntaxLanguage;

void syntax_set_theme_mode(gboolean is_dark);
gboolean syntax_get_theme_mode(void);

/* States match Python logic roughly but simplified for C port first */
typedef enum {
    STATE_ROOT = 0,
    STATE_IN_SINGLE_QUOTE = 1,      /* '...' */
    STATE_IN_DOUBLE_QUOTE = 2,      /* "..." */
    STATE_IN_ML_COMMENT = 3,        /* / * ... * / */
    /* Python specifics */
    STATE_IN_TRIPLE_SQ_STRING = 4,  /* '''...''' */
    STATE_IN_TRIPLE_DQ_STRING = 5,  /* """...""" */
    STATE_C_PARAMS = 6,             /* Inside C function parameter list ( ... ) */
    STATE_C_ENUM_WAIT_LBRACE = 7,   /* Saw 'enum', waiting for '{' */
    STATE_C_ENUM = 8,               /* Inside enum { ... } */
    STATE_C_ENUM_ML_COMMENT = 9,    /* multi-line comment inside enum */
    STATE_C_PARAMS_ML_COMMENT = 10, /* multi-line comment inside params */
    STATE_SH_BACKTICK = 11,         /* `...` (command substitution) */
    STATE_BASH_CMD_SUBST = 12,      /* $(...) */
    STATE_BASH_CONTINUATION = 13,   /* Previous line ended with \ */
    STATE_BASH_CASE = 14,           /* Inside a case statement, looking for patterns */
    STATE_BASH_CASE_BODY = 15,      /* Inside a case statement, inside a command block */
    STATE_RUST_ML_COMMENT = 16,     /* Rust nested block comments */
    STATE_MD_CODE_BLOCK = 17,       /* Markdown ``` code block */
    STATE_MD_BLOCK_QUOTE = 18,      /* Markdown > block quote (multiline interaction) */
    STATE_XML_TAG = 19,             /* Inside XML tag < ... > */
    STATE_XML_CDATA = 20            /* Inside XML <![CDATA[ ... ]]> */
} SyntaxState;

typedef struct _SyntaxContext SyntaxContext;

SyntaxContext *syntax_context_new(void);
SyntaxContext *syntax_context_ref(SyntaxContext *ctx);
void syntax_context_unref(SyntaxContext *ctx);
void syntax_context_free(SyntaxContext *ctx); /* deprecated in favor of unref */

void syntax_context_set_language(SyntaxContext *ctx, const char *lang_name);
void syntax_context_set_language_for_byte_range(SyntaxContext *ctx, size_t start_off, size_t end_off, const char *lang_name);
void syntax_context_apply_edit(SyntaxContext *ctx, size_t start_line, int line_delta);
void syntax_context_apply_byte_edit(SyntaxContext *ctx, size_t offset, int64_t delta_len);
void syntax_context_invalidate_all(SyntaxContext *ctx);  /* Clear all cached highlights */
void syntax_context_invalidate_cache(SyntaxContext *ctx); /* Clear attributes but keep state */
const char *syntax_context_get_language_name(SyntaxContext *ctx);
SyntaxLanguage syntax_context_get_language(SyntaxContext *ctx);

const char *syntax_detect_language(const char *content);

/* Highlight a line given its index. Updates the internal state chain for the NEXT line. 
   'text' should be the content of the line.
   Returns a PangoAttrList to apply.
*/
/* Process a line to update state, optionally computing attributes. 
   If compute_attributes is FALSE, returns NULL. */
PangoAttrList *syntax_highlight_line(SyntaxContext *ctx, size_t line_index, size_t line_start_off, const char *text);
PangoAttrList *syntax_process_line(SyntaxContext *ctx, size_t line_index, size_t line_start_off, const char *text, gboolean compute_attributes);
PangoAttrList *syntax_process_line_len(SyntaxContext *ctx, size_t line_index, size_t line_start_off, const char *text, size_t len, gboolean compute_attributes);

/* Get the number of lines that have been processed for state so far */
size_t syntax_get_processed_line_count(SyntaxContext *ctx);

#endif
