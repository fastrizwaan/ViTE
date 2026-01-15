#include "syntax.h"

struct _SyntaxContext {
    SyntaxLanguage lang;
    
    /* State tracking: index i = state AFTER line i */
    GByteArray *state_chain;

    /* Cache: maps line_index -> {content_hash, PangoAttrList*} */
    GHashTable *line_cache;  /* size_t -> SyntaxCacheEntry* */
    
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

/* Cache entry structure */
typedef struct {
    guint content_hash;
    SyntaxState start_state;
    PangoAttrList *attrs;
} SyntaxCacheEntry;

/* --- Colors --- */
/* Atom One Dark theme colors - Pre-parsed for performance */
static PangoColor color_keyword;   /* Purple - keywords */
static PangoColor color_type;      /* Yellow/Gold - types, class names */
static PangoColor color_string;    /* Green - strings */
static PangoColor color_comment;   /* Gray - comments */
static PangoColor color_preproc;   /* Red - preprocessor, decorators */
static PangoColor color_number;    /* Orange - numbers */
static PangoColor color_function;  /* Blue - functions, builtins */
static PangoColor color_bool;      /* Orange - True/False/None */
static PangoColor color_self;      /* Red - self, helpers */
static PangoColor color_variable;  /* Red - variables */
static gboolean colors_initialized = FALSE;

static void
syntax_cache_entry_free(gpointer data)
{
    SyntaxCacheEntry *entry = data;
    if (entry->attrs) pango_attr_list_unref(entry->attrs);
    g_free(entry);
}

static void
init_syntax_colors(void)
{
    if (colors_initialized) return;
    pango_color_parse(&color_keyword, "#C678DD");
    pango_color_parse(&color_type, "#E5C07B");
    pango_color_parse(&color_string, "#98C379");
    pango_color_parse(&color_comment, "#5C6370");
    pango_color_parse(&color_preproc, "#E06C75");
    pango_color_parse(&color_number, "#D19A66");
    pango_color_parse(&color_function, "#61AFEF");
    pango_color_parse(&color_bool, "#D19A66");
    pango_color_parse(&color_self, "#E06C75");
    pango_color_parse(&color_variable, "#E06C75");
    colors_initialized = TRUE;
}

SyntaxContext *
syntax_context_new(void)
{
    /* Initialize color cache on first context creation */
    init_syntax_colors();
    
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
    
    ctx->sh_keywords = g_regex_new("\\b(if|then|else|elif|fi|case|esac|for|select|while|until|do|done|in|function|time|coproc|declare|typeset|local|readonly|export|unset|set|shopt|trap|source|alias|unalias|break|continue|return|exit|eval|exec)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->sh_comment = g_regex_new("#.*$", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->sh_variable = g_regex_new("\\$(\\w+|\\{[^}]+\\}|[0-9*@#?!$-])", G_REGEX_OPTIMIZE, 0, NULL);

    /* Initialize line cache */
    ctx->line_cache = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, syntax_cache_entry_free);

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
    
    /* Free cache - g_hash_table_destroy triggers the destroy callback (syntax_cache_entry_free)
     * for each entry, which handles cleanup - do NOT unref manually to avoid double-free. */
    if (ctx->line_cache) {
        g_hash_table_destroy(ctx->line_cache);
    }
    
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
    /* Remove cache entries from start_line onwards.
     * Note: g_hash_table_iter_remove will trigger the destroy callback
     * (syntax_cache_entry_free) which handles cleanup - do NOT unref manually. */
    if (ctx->line_cache) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, ctx->line_cache);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            size_t line = GPOINTER_TO_SIZE(key);
            if (line >= start_line) {
                g_hash_table_iter_remove(&iter);
            }
        }
    }
}

void
syntax_context_invalidate_all(SyntaxContext *ctx)
{
    g_byte_array_set_size(ctx->state_chain, 0);
    /* g_hash_table_remove_all triggers the destroy callback (syntax_cache_entry_free)
     * for each entry, which handles cleanup - do NOT unref manually to avoid double-free. */
    if (ctx->line_cache) {
        g_hash_table_remove_all(ctx->line_cache);
    }
}

/* Helper to add attribute - uses pre-parsed color for performance */
static void
add_attr(PangoAttrList *attrs, int start, int end, const PangoColor *color)
{
    if (start >= end) return;
    PangoAttribute *attr = pango_attr_foreground_new(color->red, color->green, color->blue);
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
    if (ctx->lang == LANG_NONE) return pango_attr_list_new();
    
    SyntaxState start_state = get_line_start_state(ctx, line_index);
    guint content_hash = g_str_hash(text);
    
    /* Cache lookup */
    if (ctx->line_cache) {
        SyntaxCacheEntry *cached = g_hash_table_lookup(ctx->line_cache, GSIZE_TO_POINTER(line_index));
        if (cached && cached->content_hash == content_hash && cached->start_state == start_state) {
            /* Cache hit - return a copy of the cached attrs */
            return pango_attr_list_ref(cached->attrs);
        }
    }
    
    PangoAttrList *attrs = pango_attr_list_new();
    SyntaxState state = start_state;
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
                    add_attr(attrs, s, e, &color_keyword);
                    g_match_info_next(mi, NULL);
                }
            }
            g_match_info_free(mi);
             
            if (g_regex_match(ctx->c_types, text, 0, &mi)) {
                while (g_match_info_matches(mi)) {
                    int s, e;
                    g_match_info_fetch_pos(mi, 0, &s, &e);
                    add_attr(attrs, s, e, &color_type);
                    g_match_info_next(mi, NULL);
                }
            }
            g_match_info_free(mi);
            
            if (g_regex_match(ctx->c_preproc, text, 0, &mi)) {
                 while (g_match_info_matches(mi)) {
                    int s, e;
                    g_match_info_fetch_pos(mi, 0, &s, &e);
                    add_attr(attrs, s, e, &color_preproc);
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
                    add_attr(attrs, start_pos, cur, &color_string);
                    continue; /* Loop again in ROOT */
                }
                /* // */
                else if (text[cur] == '/' && cur+1 < len && text[cur+1] == '/') {
                    /* Comment until EOL */
                    add_attr(attrs, cur, len, &color_comment);
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
                    
                    add_attr(attrs, start_pos, cur, &color_comment);
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
                add_attr(attrs, start_pos, cur, &color_comment);
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
                 add_attr(attrs, s, e, &color_preproc);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Function definitions (def funcname) */
         if (g_regex_match(ctx->py_function_def, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int kw_s, kw_e, name_s, name_e;
                 if (g_match_info_fetch_pos(mi, 1, &kw_s, &kw_e)) {
                     add_attr(attrs, kw_s, kw_e, &color_keyword);
                 }
                 if (g_match_info_fetch_pos(mi, 2, &name_s, &name_e)) {
                     add_attr(attrs, name_s, name_e, &color_function);
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
                     add_attr(attrs, kw_s, kw_e, &color_keyword);
                 }
                 if (g_match_info_fetch_pos(mi, 2, &name_s, &name_e)) {
                     add_attr(attrs, name_s, name_e, &color_type);
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
                 add_attr(attrs, s, e, &color_keyword);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Bool ops (True, False, None, and) */
         if (g_regex_match(ctx->py_bool_ops, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, e, &color_bool);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Builtins (print, len, etc.) */
         if (g_regex_match(ctx->py_builtins, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, e, &color_function);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Helpers (self, __xxx__) */
         if (g_regex_match(ctx->py_helpers, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, e, &color_self);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Numbers */
         if (g_regex_match(ctx->py_number, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, e, &color_number);
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
                     add_attr(attrs, start_pos, cur, &color_string);
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
                     add_attr(attrs, start_pos, cur, &color_string);
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
                     add_attr(attrs, start_pos, cur, &color_string);
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
                     add_attr(attrs, start_pos, cur, &color_string);
                 }
                 /* Comment */
                 else if (text[cur] == '#') {
                     add_attr(attrs, cur, len, &color_comment);
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
                 add_attr(attrs, start_pos, cur, &color_string);
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
                 add_attr(attrs, start_pos, cur, &color_string);
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
                 add_attr(attrs, s, e, &color_keyword);
                 g_match_info_next(mi, NULL);
             }
         }
         g_match_info_free(mi);
         
         /* Variables ($VAR, ${VAR}, $1, etc.) */
         if (g_regex_match(ctx->sh_variable, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, e, &color_variable);
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
                     add_attr(attrs, start_pos, cur, &color_string);
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
                     add_attr(attrs, start_pos, cur, &color_string);
                 }
                 /* Comment */
                 else if (text[cur] == '#') {
                     /* Careful: # inside ${} or "..." handled by state logic above? 
                        In ROOT, # is comment. 
                     */
                     add_attr(attrs, cur, len, &color_comment);
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
                 add_attr(attrs, start_pos, cur, &color_string);
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
                 add_attr(attrs, start_pos, cur, &color_string);
             }
             else {
                 state = STATE_ROOT;
                 cur++;
             }
         }
    }
    
    /* Save end state */
    set_line_end_state(ctx, line_index, state);
    
    /* Store in cache - hash table's destroy function handles cleanup */
    if (ctx->line_cache) {
        SyntaxCacheEntry *entry = g_new(SyntaxCacheEntry, 1);
        entry->content_hash = content_hash;
        entry->start_state = start_state;
        entry->attrs = pango_attr_list_ref(attrs);
        g_hash_table_insert(ctx->line_cache, GSIZE_TO_POINTER(line_index), entry);
    }
    
    return attrs;
}
