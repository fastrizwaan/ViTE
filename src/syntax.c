#include "syntax.h"

struct _SyntaxContext {
    SyntaxLanguage lang;
    
    /* State tracking: index i = state AFTER line i */
    GByteArray *state_chain;

    /* Regexes */
    /* C */
    GRegex *c_keywords;
    GRegex *c_types;
    GRegex *c_string_dq;  /* " */
    GRegex *c_comment_sl; /* // */
    GRegex *c_comment_ml_start; /* / * */
    GRegex *c_comment_ml_end;   /* * / */
    GRegex *c_preproc;    /* #... */
    
    /* Python */
    GRegex *py_keywords;
    GRegex *py_bool_ops;      /* True, False, None, and */
    GRegex *py_builtins;
    GRegex *py_helpers;       /* self, __xxx__ */
    GRegex *py_number;
    GRegex *py_decorator;
    GRegex *py_function_def;
    GRegex *py_class_def;
    GRegex *py_string_dq;
    GRegex *py_string_sq;
    GRegex *py_triple_dq;
    GRegex *py_triple_sq;
    GRegex *py_comment;
    
    /* Bash */
    GRegex *sh_keywords;
    GRegex *sh_comment;
    GRegex *sh_variable;
};

/* --- Colors --- */
/* Atom One Dark theme colors */
static const char *COLOR_KEYWORD  = "#C678DD";  /* Purple - keywords */
static const char *COLOR_TYPE     = "#E5C07B";  /* Yellow/Gold - types, class names */
static const char *COLOR_STRING   = "#98C379";  /* Green - strings */
static const char *COLOR_COMMENT  = "#5C6370";  /* Gray - comments */
static const char *COLOR_PREPROC  = "#E06C75";  /* Red - preprocessor, decorators */
static const char *COLOR_NUMBER   = "#D19A66";  /* Orange - numbers */
static const char *COLOR_FUNCTION = "#61AFEF";  /* Blue - functions, builtins */
static const char *COLOR_BOOL     = "#D19A66";  /* Orange - True/False/None */
static const char *COLOR_SELF     = "#E06C75";  /* Red - self, helpers */
static const char *COLOR_VARIABLE = "#E06C75";  /* Red - variables */

SyntaxContext *
syntax_context_new(void)
{
    SyntaxContext *ctx = malloc(sizeof(SyntaxContext));
    ctx->lang = LANG_NONE;
    ctx->state_chain = g_byte_array_new();
    
    /* Compile Regexes (Lazy or upfront? Upfront for simplicity) */
    
    /* C Patterns */
    ctx->c_keywords = g_regex_new("\\b(if|else|while|for|return|switch|case|break|continue|struct|typedef|enum|union|const|static|void|int|char|long|unsigned|signed|sizeof|double|float|virtual|class|public|private|protected)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->c_types = g_regex_new("\\b([A-Z][a-zA-Z0-9_]*_t|[A-Z][a-zA-Z0-9_]*)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->c_string_dq = g_regex_new("\"(\\\\.|[^\"])*\"", G_REGEX_OPTIMIZE, 0, NULL); 
    /* Note: Simple string regex doesn't handle multi-line string literal concatenation well in C but OK for single line. 
       However, we really care about STATE based matching now. 
       So we need PATTERNS for "start of string".
    */
    
    /* Redo regex strategy: We use pango_attr_list merging. 
       We can match tokens *independently* if we are careful.
       BUT state is critical.
    */
    
    if (ctx->c_string_dq) g_regex_unref(ctx->c_string_dq);
    /* Split specific patterns */
    ctx->c_string_dq = g_regex_new("\"", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->c_comment_sl = g_regex_new("//.*$", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->c_comment_ml_start = g_regex_new("/\\*", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->c_comment_ml_end = g_regex_new("\\*/", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->c_preproc = g_regex_new("^\\s*#\\w+", G_REGEX_OPTIMIZE, 0, NULL);

    /* Python Patterns - matching syntax_v2.py */
    ctx->py_keywords = g_regex_new("\\b(as|assert|async|await|break|class|continue|def|del|elif|else|except|finally|for|from|global|if|import|in|is|lambda|nonlocal|not|or|pass|raise|return|try|while|with|yield)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->py_bool_ops = g_regex_new("\\b(and|None|True|False)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->py_builtins = g_regex_new("\\b(abs|all|any|ascii|bin|bool|bytearray|bytes|callable|chr|classmethod|compile|complex|delattr|dict|dir|divmod|enumerate|eval|exec|filter|float|format|frozenset|getattr|globals|hasattr|hash|help|hex|id|input|int|isinstance|issubclass|iter|len|list|locals|map|max|memoryview|min|next|object|oct|open|ord|pow|print|property|range|repr|reversed|round|set|setattr|slice|sorted|staticmethod|str|sum|super|tuple|type|vars|zip|__import__|__init__)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->py_helpers = g_regex_new("\\b(self|__\\w+__)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->py_number = g_regex_new("\\b\\d+\\.?\\d*([eE][+-]?\\d+)?\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->py_decorator = g_regex_new("@\\w+", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->py_function_def = g_regex_new("\\b(def)\\s+(\\w+)", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->py_class_def = g_regex_new("\\b(class)\\s+(\\w+)", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->py_comment = g_regex_new("#.*$", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->py_triple_dq = g_regex_new("\"\"\"", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->py_triple_sq = g_regex_new("'''", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->py_string_dq = g_regex_new("\"", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->py_string_sq = g_regex_new("'", G_REGEX_OPTIMIZE, 0, NULL);
    
    /* Bash */
    ctx->sh_keywords = g_regex_new("\\b(if|then|else|elif|fi|case|esac|for|select|while|until|do|done|in|function|time|coproc|declare|typeset|local|readonly|export|unset|set|shopt|trap|source|alias|unalias|break|continue|return|exit|eval|exec)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->sh_comment = g_regex_new("#.*$", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->sh_variable = g_regex_new("\\$(\\w+|\\{[^}]+\\}|[0-9*@#?!$-])", G_REGEX_OPTIMIZE, 0, NULL);

    return ctx;
}

void
syntax_context_free(SyntaxContext *ctx)
{
    g_byte_array_unref(ctx->state_chain);
    
    if (ctx->c_keywords) g_regex_unref(ctx->c_keywords);
    if (ctx->c_types) g_regex_unref(ctx->c_types);
    if (ctx->c_string_dq) g_regex_unref(ctx->c_string_dq);
    if (ctx->c_comment_sl) g_regex_unref(ctx->c_comment_sl);
    if (ctx->c_comment_ml_start) g_regex_unref(ctx->c_comment_ml_start);
    if (ctx->c_comment_ml_end) g_regex_unref(ctx->c_comment_ml_end);
    if (ctx->c_preproc) g_regex_unref(ctx->c_preproc);

    if (ctx->py_keywords) g_regex_unref(ctx->py_keywords);
    if (ctx->py_bool_ops) g_regex_unref(ctx->py_bool_ops);
    if (ctx->py_builtins) g_regex_unref(ctx->py_builtins);
    if (ctx->py_helpers) g_regex_unref(ctx->py_helpers);
    if (ctx->py_number) g_regex_unref(ctx->py_number);
    if (ctx->py_decorator) g_regex_unref(ctx->py_decorator);
    if (ctx->py_function_def) g_regex_unref(ctx->py_function_def);
    if (ctx->py_class_def) g_regex_unref(ctx->py_class_def);
    if (ctx->py_comment) g_regex_unref(ctx->py_comment);
    if (ctx->py_triple_dq) g_regex_unref(ctx->py_triple_dq);
    if (ctx->py_triple_sq) g_regex_unref(ctx->py_triple_sq);
    if (ctx->py_string_dq) g_regex_unref(ctx->py_string_dq);
    if (ctx->py_string_sq) g_regex_unref(ctx->py_string_sq);

    if (ctx->sh_keywords) g_regex_unref(ctx->sh_keywords);
    if (ctx->sh_comment) g_regex_unref(ctx->sh_comment);
    if (ctx->sh_variable) g_regex_unref(ctx->sh_variable);
    
    free(ctx);
}

void
syntax_context_set_language(SyntaxContext *ctx, const char *lang_name)
{
    if (!lang_name) {
        ctx->lang = LANG_NONE;
    } else if (strcmp(lang_name, "c") == 0 || strcmp(lang_name, "cpp") == 0 || strcmp(lang_name, "h") == 0) {
        ctx->lang = LANG_C;
    } else if (strcmp(lang_name, "python") == 0 || strcmp(lang_name, "py") == 0) {
        ctx->lang = LANG_PYTHON;
    } else if (strcmp(lang_name, "bash") == 0 || strcmp(lang_name, "sh") == 0 || strcmp(lang_name, "zsh") == 0) {
        ctx->lang = LANG_BASH;
    } else {
        ctx->lang = LANG_NONE;
    }
    
    /* Clear states */
    g_byte_array_set_size(ctx->state_chain, 0);
}

void
syntax_context_invalidate(SyntaxContext *ctx, size_t start_line)
{
    if (start_line < ctx->state_chain->len) {
        g_byte_array_set_size(ctx->state_chain, start_line);
    }
}

/* Helper to add attribute */
static void
add_attr(PangoAttrList *attrs, int start, int end, const char *color)
{
    if (start >= end) return;
    PangoAttribute *attr = pango_attr_foreground_new(0, 0, 0);
    pango_color_parse(&((PangoAttrColor*)attr)->color, color);
    attr->start_index = start;
    attr->end_index = end;
    pango_attr_list_insert(attrs, attr);
}

/* Helper to get next state */
static SyntaxState
get_line_start_state(SyntaxContext *ctx, size_t line_index)
{
    if (line_index == 0) return STATE_ROOT;
    if (line_index - 1 < ctx->state_chain->len) {
        return (SyntaxState)ctx->state_chain->data[line_index - 1];
    }
    return STATE_ROOT; /* Default if unknown, though usually we process in order */
}

static void
set_line_end_state(SyntaxContext *ctx, size_t line_index, SyntaxState state)
{
    if (line_index >= ctx->state_chain->len) {
        /* fill gaps with ROOT if any (shouldn't happen with sequential access) */
        size_t old_len = ctx->state_chain->len;
        g_byte_array_set_size(ctx->state_chain, line_index + 1);
        for (size_t i = old_len; i < line_index; i++) {
            ctx->state_chain->data[i] = STATE_ROOT;
        }
    }
    ctx->state_chain->data[line_index] = (guint8)state;
}

PangoAttrList *
syntax_highlight_line(SyntaxContext *ctx, size_t line_index, const char *text)
{
    PangoAttrList *attrs = pango_attr_list_new();
    if (ctx->lang == LANG_NONE) return attrs;
    
    SyntaxState state = get_line_start_state(ctx, line_index);
    size_t len = strlen(text);
    size_t cur = 0;
    
    /* 
       We iterate through the text manually string searching or regex? 
       Manual iteration char-by-char is robust for state machines. 
       Regex is good for "next token".
       
       Let's try a hybrid: 
       If ROOT, regex for "Next interesting thing".
       Things: Quote, Comment Start.
       
       Actually, simple manual scanner might be faster and easier for C/Py state logic.
    */
    
    if (ctx->lang == LANG_C) {
        /* Regex helper for keywords (stateless) */
        {
            GMatchInfo *mi;
            if (g_regex_match(ctx->c_keywords, text, 0, &mi)) {
                while (g_match_info_matches(mi)) {
                    int s, e;
                    g_match_info_fetch_pos(mi, 0, &s, &e);
                    add_attr(attrs, s, e, COLOR_KEYWORD);
                    g_match_info_next(mi, NULL);
                }
            }
            g_match_info_free(mi);
             
            if (g_regex_match(ctx->c_types, text, 0, &mi)) {
                while (g_match_info_matches(mi)) {
                    int s, e;
                    g_match_info_fetch_pos(mi, 0, &s, &e);
                    add_attr(attrs, s, e, COLOR_TYPE);
                    g_match_info_next(mi, NULL);
                }
            }
            g_match_info_free(mi);
            
            if (g_regex_match(ctx->c_preproc, text, 0, &mi)) {
                 while (g_match_info_matches(mi)) {
                    int s, e;
                    g_match_info_fetch_pos(mi, 0, &s, &e);
                    add_attr(attrs, s, e, COLOR_PREPROC);
                    g_match_info_next(mi, NULL);
                }
            }
             g_match_info_free(mi);
        }
        
        /* State machine for Strings and Comments */
        while (cur < len) {
            if (state == STATE_ROOT) {
                /* Check for start of string/comment */
                /* " */
                if (text[cur] == '"') {
                    state = STATE_IN_DOUBLE_QUOTE;
                    /* Color opening quote? handled by range later? 
                       We can push attribute for range [cur, ...] when we find end.
                       OR we paint char by char? No attributes need ranges.
                       Let's mark start_pos.
                    */
                    size_t start_pos = cur;
                    cur++;
                    
                    /* Find end quote or newline */
                    while (cur < len) {
                        if (text[cur] == '"' && text[cur-1] != '\\') {
                            /* End found */
                            cur++;
                            state = STATE_ROOT;
                            break;
                        }
                        cur++;
                    }
                    /* Apply color */
                    add_attr(attrs, start_pos, cur, COLOR_STRING);
                    continue; /* Loop again in ROOT */
                }
                /* // */
                else if (text[cur] == '/' && cur+1 < len && text[cur+1] == '/') {
                    /* Comment until EOL */
                    add_attr(attrs, cur, len, COLOR_COMMENT);
                    cur = len;
                    break;
                }
                /* / * */
                else if (text[cur] == '/' && cur+1 < len && text[cur+1] == '*') {
                    state = STATE_IN_ML_COMMENT;
                    size_t start_pos = cur;
                    cur += 2;
                    /* Find end */
                    while (cur + 1 < len) {
                        if (text[cur] == '*' && text[cur+1] == '/') {
                            cur += 2;
                            state = STATE_ROOT;
                            break;
                        }
                        cur++;
                    }
                    if (state == STATE_IN_ML_COMMENT) cur = len; /* Until end of line */
                    
                    add_attr(attrs, start_pos, cur, COLOR_COMMENT);
                    continue;
                }
                else {
                    cur++;
                }
            }
            else if (state == STATE_IN_ML_COMMENT) {
                /* Looking for end * / */
                size_t start_pos = cur;
                while (cur + 1 < len) {
                    if (text[cur] == '*' && text[cur+1] == '/') {
                        cur += 2;
                        state = STATE_ROOT;
                        break;
                    }
                    cur++;
                }
                if (state == STATE_IN_ML_COMMENT) cur = len;
                add_attr(attrs, start_pos, cur, COLOR_COMMENT);
            }
            else {
                /* Should handle multi-line strings if supported, C strings usually single line but with backslash... 
                   Simple C string handling above handles single line. 
                */
                cur++;
            }
        }
    } else if (ctx->lang == LANG_PYTHON) {
         /* All stateless regex matches first */
         GMatchInfo *mi;
         
         /* Decorators (@decorator) */
         if (g_regex_match(ctx->py_decorator, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, e, COLOR_PREPROC);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Function definitions (def funcname) */
         if (g_regex_match(ctx->py_function_def, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int kw_s, kw_e, name_s, name_e;
                 if (g_match_info_fetch_pos(mi, 1, &kw_s, &kw_e)) {
                     add_attr(attrs, kw_s, kw_e, COLOR_KEYWORD);
                 }
                 if (g_match_info_fetch_pos(mi, 2, &name_s, &name_e)) {
                     add_attr(attrs, name_s, name_e, COLOR_FUNCTION);
                 }
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Class definitions (class ClassName) */
         if (g_regex_match(ctx->py_class_def, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int kw_s, kw_e, name_s, name_e;
                 if (g_match_info_fetch_pos(mi, 1, &kw_s, &kw_e)) {
                     add_attr(attrs, kw_s, kw_e, COLOR_KEYWORD);
                 }
                 if (g_match_info_fetch_pos(mi, 2, &name_s, &name_e)) {
                     add_attr(attrs, name_s, name_e, COLOR_TYPE);
                 }
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Keywords */
         if (g_regex_match(ctx->py_keywords, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, e, COLOR_KEYWORD);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Bool ops (True, False, None, and) */
         if (g_regex_match(ctx->py_bool_ops, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, e, COLOR_BOOL);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Builtins (print, len, etc.) */
         if (g_regex_match(ctx->py_builtins, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, e, COLOR_FUNCTION);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Helpers (self, __xxx__) */
         if (g_regex_match(ctx->py_helpers, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, e, COLOR_SELF);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Numbers */
         if (g_regex_match(ctx->py_number, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, e, COLOR_NUMBER);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         while (cur < len) {
             /* Check state */
             if (state == STATE_ROOT) {
                 /* Triple double quote */
                 if (g_str_has_prefix(text + cur, "\"\"\"")) {
                     state = STATE_IN_TRIPLE_DQ_STRING;
                     size_t start_pos = cur;
                     cur += 3;
                     while (cur + 2 < len) {
                         if (g_str_has_prefix(text + cur, "\"\"\"") && text[cur-1] != '\\') {
                             cur += 3;
                             state = STATE_ROOT;
                             break;
                         }
                         cur++;
                     }
                     if (state == STATE_IN_TRIPLE_DQ_STRING) cur = len;
                     add_attr(attrs, start_pos, cur, COLOR_STRING);
                 }
                 /* Triple single quote */
                 else if (g_str_has_prefix(text + cur, "'''")) {
                     state = STATE_IN_TRIPLE_SQ_STRING;
                     size_t start_pos = cur;
                     cur += 3;
                     while (cur + 2 < len) {
                         if (g_str_has_prefix(text + cur, "'''") && text[cur-1] != '\\') {
                             cur += 3;
                             state = STATE_ROOT;
                             break;
                         }
                         cur++;
                     }
                     if (state == STATE_IN_TRIPLE_SQ_STRING) cur = len;
                     add_attr(attrs, start_pos, cur, COLOR_STRING);
                 }
                 /* Double quote */
                 else if (text[cur] == '"') {
                     /* Single line string usually in Py, unless escaped newline, but simplified here */
                     size_t start_pos = cur;
                     cur++;
                     while (cur < len) {
                         if (text[cur] == '"' && text[cur-1] != '\\') {
                             cur++;
                             break;
                         }
                         cur++;
                     }
                     add_attr(attrs, start_pos, cur, COLOR_STRING);
                 }
                 /* Single quote */
                 else if (text[cur] == '\'') {
                     size_t start_pos = cur;
                     cur++;
                     while (cur < len) {
                         if (text[cur] == '\'' && text[cur-1] != '\\') {
                             cur++;
                             break;
                         }
                         cur++;
                     }
                     add_attr(attrs, start_pos, cur, COLOR_STRING);
                 }
                 /* Comment */
                 else if (text[cur] == '#') {
                     add_attr(attrs, cur, len, COLOR_COMMENT);
                     cur = len;
                 }
                 else {
                     cur++;
                 }
             }
             else if (state == STATE_IN_TRIPLE_DQ_STRING) {
                 size_t start_pos = cur;
                 while (cur + 2 < len) {
                     if (g_str_has_prefix(text + cur, "\"\"\"") && (cur == 0 || text[cur-1] != '\\')) {
                         cur += 3;
                         state = STATE_ROOT;
                         break;
                     }
                     cur++;
                 }
                 if (state == STATE_IN_TRIPLE_DQ_STRING) cur = len;
                 add_attr(attrs, start_pos, cur, COLOR_STRING);
             }
             else if (state == STATE_IN_TRIPLE_SQ_STRING) {
                 size_t start_pos = cur;
                 while (cur + 2 < len) {
                     if (g_str_has_prefix(text + cur, "'''") && (cur == 0 || text[cur-1] != '\\')) {
                         cur += 3;
                         state = STATE_ROOT;
                         break;
                     }
                     cur++;
                 }
                 if (state == STATE_IN_TRIPLE_SQ_STRING) cur = len;
                 add_attr(attrs, start_pos, cur, COLOR_STRING);
             }
             else {
                 /* Should not happen if other states are handled or mapped */
                 state = STATE_ROOT;
                 cur++;
             }
         }
    } else if (ctx->lang == LANG_BASH) {
         /* Keywords */
         GMatchInfo *mi;
         if (g_regex_match(ctx->sh_keywords, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, e, COLOR_KEYWORD);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Variables ($VAR, ${VAR}, $1, etc.) */
         if (g_regex_match(ctx->sh_variable, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, e, COLOR_VARIABLE);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         while (cur < len) {
             /* Check state */
             if (state == STATE_ROOT) {
                 /* Double quote */
                 if (text[cur] == '"') {
                     state = STATE_IN_DOUBLE_QUOTE;
                     size_t start_pos = cur;
                     cur++;
                     while (cur < len) {
                         /* Handle escaped quote \" */
                         if (text[cur] == '"' && text[cur-1] != '\\') {
                             cur++;
                             state = STATE_ROOT; /* Back to root */
                             break;
                         }
                         cur++;
                     }
                     add_attr(attrs, start_pos, cur, COLOR_STRING);
                 }
                 /* Single quote (strong) */
                 else if (text[cur] == '\'') {
                     state = STATE_IN_SINGLE_QUOTE;
                     size_t start_pos = cur;
                     cur++;
                     while (cur < len) {
                         if (text[cur] == '\'') {
                             cur++;
                             state = STATE_ROOT;
                             break;
                         }
                         cur++;
                     }
                     add_attr(attrs, start_pos, cur, COLOR_STRING);
                 }
                 /* Comment */
                 else if (text[cur] == '#') {
                     /* Careful: # inside ${} or "..." handled by state logic above? 
                        In ROOT, # is comment. 
                     */
                     add_attr(attrs, cur, len, COLOR_COMMENT);
                     cur = len;
                 }
                 else {
                     cur++;
                 }
             }
             else if (state == STATE_IN_DOUBLE_QUOTE) {
                 size_t start_pos = cur;
                 while (cur < len) {
                     if (text[cur] == '"' && (cur == 0 || text[cur-1] != '\\')) {
                         cur++;
                         state = STATE_ROOT;
                         break;
                     }
                     cur++;
                 }
                 add_attr(attrs, start_pos, cur, COLOR_STRING);
             }
             else if (state == STATE_IN_SINGLE_QUOTE) {
                 size_t start_pos = cur;
                 while (cur < len) {
                     if (text[cur] == '\'') {
                         cur++;
                         state = STATE_ROOT;
                         break;
                     }
                     cur++;
                 }
                 add_attr(attrs, start_pos, cur, COLOR_STRING);
             }
             else {
                 state = STATE_ROOT;
                 cur++;
             }
         }
    }
    
    /* Save end state */
    set_line_end_state(ctx, line_index, state);
    
    /* Combine attributes? Pango does it. */
    
    return attrs;
}
