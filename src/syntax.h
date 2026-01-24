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
    LANG_DESKTOP
} SyntaxLanguage;

void syntax_set_theme_mode(gboolean is_dark);

/* States match Python logic roughly but simplified for C port first */
typedef enum {
    STATE_ROOT = 0,
    STATE_IN_SINGLE_QUOTE = 1,      /* '...' */
    STATE_IN_DOUBLE_QUOTE = 2,      /* "..." */
    STATE_IN_ML_COMMENT = 3,        /* / * ... * / */
    /* Python specifics */
    STATE_IN_TRIPLE_SQ_STRING = 4,  /* '''...''' */
    STATE_IN_TRIPLE_DQ_STRING = 5   /* """...""" */
} SyntaxState;

typedef struct _SyntaxContext SyntaxContext;

SyntaxContext *syntax_context_new(void);
void syntax_context_free(SyntaxContext *ctx);

void syntax_context_set_language(SyntaxContext *ctx, const char *lang_name);
void syntax_context_invalidate(SyntaxContext *ctx, size_t start_line);
void syntax_context_invalidate_all(SyntaxContext *ctx);  /* Clear all cached highlights */
const char *syntax_context_get_language_name(SyntaxContext *ctx);

/* Highlight a line given its index. Updates the internal state chain for the NEXT line. 
   'text' should be the content of the line.
   Returns a PangoAttrList to apply.
*/
/* Process a line to update state, optionally computing attributes. 
   If compute_attributes is FALSE, returns NULL. */
PangoAttrList *syntax_highlight_line(SyntaxContext *ctx, size_t line_index, const char *text);
PangoAttrList *syntax_process_line(SyntaxContext *ctx, size_t line_index, const char *text, gboolean compute_attributes);

/* Get the number of lines that have been processed for state so far */
size_t syntax_get_processed_line_count(SyntaxContext *ctx);

#endif
