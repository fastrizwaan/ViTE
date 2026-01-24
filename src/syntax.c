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
    GRegex *sh_builtins;
    GRegex *sh_comment;
    GRegex *sh_variable;
    GRegex *sh_string;
    GRegex *sh_string_sq;

    /* JavaScript */
    GRegex *js_keywords;
    GRegex *js_builtins;
    GRegex *js_func_def;
    GRegex *js_comment_sl;
    GRegex *js_comment_ml_start;
    GRegex *js_comment_ml_end;
    GRegex *js_string_dq;
    GRegex *js_string_sq;
    GRegex *js_template; /* `...` */
    GRegex *js_number;

    /* JSON */
    GRegex *json_keywords; /* true/false/null */
    GRegex *json_string;
    GRegex *json_number;
    GRegex *json_key; /* "key": */

    /* YAML */
    GRegex *yaml_keys;
    GRegex *yaml_comment;
    GRegex *yaml_scalars; /* true/false/null */

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

/* --- Colors --- */
/* --- Colors --- */
static gboolean is_dark_mode = TRUE;

/* Dark Theme (One Dark) */
static PangoColor d_keyword;   /* #c678dd Purple */
static PangoColor d_builtin;   /* #56b6c2 Cyan */
static PangoColor d_string;    /* #98c379 Green */
static PangoColor d_comment;   /* #5c6370 Grey */
static PangoColor d_number;    /* #d19a66 Orange */
static PangoColor d_function;  /* #61afef Blue */
static PangoColor d_type;      /* #e5c07b Yellow/Gold (Class) */
static PangoColor d_decorator; /* #56b6c2 Cyan */
static PangoColor d_variable;  /* #e06c75 Red */
static PangoColor d_tag;       /* #e06c75 Red */
static PangoColor d_attribute; /* #d19a66 Orange */
static PangoColor d_param;     /* #d19a66 Orange (Argument) */
static PangoColor d_property;  /* #56b6c2 Cyan */
static PangoColor d_preproc;   /* #c678dd Purple */

/* Light Theme (One Light) */
static PangoColor l_keyword;   /* #a626a4 Purple */
static PangoColor l_builtin;   /* #0184bc Cyan/Blue */
static PangoColor l_string;    /* #50a14f Green */
static PangoColor l_comment;   /* #a0a1a7 Grey */
static PangoColor l_number;    /* #986801 Orange */
static PangoColor l_function;  /* #4078f2 Blue */
static PangoColor l_type;      /* #c18401 Orange/Gold */
static PangoColor l_decorator; /* #a626a4 Purple */
static PangoColor l_variable;  /* #e45649 Red */
static PangoColor l_tag;       /* #e45649 Red */
static PangoColor l_attribute; /* #986801 Orange */
static PangoColor l_param;     /* #986801 Orange */
static PangoColor l_property;  /* #0184bc Cyan */
static PangoColor l_preproc;   /* #a626a4 Purple */

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
    
    /* Dark */
    pango_color_parse(&d_keyword, "#c678dd");
    pango_color_parse(&d_builtin, "#56b6c2");
    pango_color_parse(&d_string, "#98c379");
    pango_color_parse(&d_comment, "#5c6370");
    pango_color_parse(&d_number, "#d19a66");
    pango_color_parse(&d_function, "#61afef");
    pango_color_parse(&d_type, "#e5c07b");
    pango_color_parse(&d_decorator, "#56b6c2");
    pango_color_parse(&d_variable, "#e06c75");
    pango_color_parse(&d_tag, "#e06c75");
    pango_color_parse(&d_attribute, "#d19a66");
    pango_color_parse(&d_param, "#d19a66");
    pango_color_parse(&d_property, "#56b6c2");
    pango_color_parse(&d_preproc, "#c678dd");

    /* Light */
    pango_color_parse(&l_keyword, "#a626a4");
    pango_color_parse(&l_builtin, "#0184bc");
    pango_color_parse(&l_string, "#50a14f");
    pango_color_parse(&l_comment, "#a0a1a7");
    pango_color_parse(&l_number, "#986801");
    pango_color_parse(&l_function, "#4078f2");
    pango_color_parse(&l_type, "#c18401");
    pango_color_parse(&l_decorator, "#a626a4");
    pango_color_parse(&l_variable, "#e45649");
    pango_color_parse(&l_tag, "#e45649");
    pango_color_parse(&l_attribute, "#986801");
    pango_color_parse(&l_param, "#986801");
    pango_color_parse(&l_property, "#0184bc");
    pango_color_parse(&l_preproc, "#a626a4");

    colors_initialized = TRUE;
}

void
syntax_set_theme_mode(gboolean is_dark)
{
    if (is_dark_mode != is_dark) {
        is_dark_mode = is_dark;
    }
}

SyntaxContext *
syntax_context_new(void)
{
    /* Initialize color cache on first context creation */
    init_syntax_colors();
    
    SyntaxContext *ctx = malloc(sizeof(SyntaxContext));
    memset(ctx, 0, sizeof(SyntaxContext)); /* Clear all regex pointers */
    ctx->lang = LANG_NONE;
    ctx->state_chain = g_byte_array_new();
    
    /* Compile Regexes */
    
    /* C Patterns (Migrated to Linear Tokenizer) */
    /* Python Patterns (Migrated to Linear Tokenizer) */

    
    /* Bash */
    ctx->sh_keywords = g_regex_new("\\b(if|then|else|elif|fi|case|esac|for|select|while|until|do|done|in|function|time|coproc|declare|typeset|local|readonly|export|unset|set|shopt|trap|source|alias|unalias|break|continue|return|exit|eval|exec)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->sh_builtins = g_regex_new("\\b(echo|printf|cd|pwd|ls|cp|mv|rm|mkdir|rmdir|touch|cat|grep|sed|awk|find|chmod|chown|kill|ps|jobs|bg|fg|history|read|wait|sleep|true|false|make|install|flatpak|git|node|npm|pip|python|python3|gcc|g\\+\\+|clang|docker|systemctl|journalctl)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->sh_comment = g_regex_new("#.*$", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->sh_variable = g_regex_new("(\\$[a-zA-Z_][a-zA-Z0-9_]*|\\$\\{[^}]+\\}|\\$[0-9*@#?!$-])", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->sh_string = g_regex_new("\"(\\\\.|[^\"])*\"", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->sh_string_sq = g_regex_new("'[^']*'", G_REGEX_OPTIMIZE, 0, NULL);

    /* JavaScript */
    ctx->js_keywords = g_regex_new("\\b(async|await|break|case|catch|class|const|continue|debugger|default|delete|do|else|export|extends|finally|for|from|function|get|if|import|in|instanceof|let|new|of|return|set|static|super|switch|this|throw|try|typeof|var|void|while|with|yield|true|false|null|undefined|NaN)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->js_builtins = g_regex_new("\\b(Array|Boolean|Date|Error|Function|JSON|Map|Math|Number|Object|Promise|Proxy|RegExp|Set|String|Symbol|WeakMap|WeakSet|console|document|window|global|module|exports|require|process)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->js_func_def = g_regex_new("\\b(function)\\s+([a-zA-Z_$][a-zA-Z0-9_$]*)", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->js_comment_sl = g_regex_new("//.*$", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->js_comment_ml_start = g_regex_new("/\\*", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->js_comment_ml_end = g_regex_new("\\*/", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->js_string_dq = g_regex_new("\"", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->js_string_sq = g_regex_new("'", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->js_template = g_regex_new("`", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->js_number = g_regex_new("\\b\\d+\\.?\\d*\\b", G_REGEX_OPTIMIZE, 0, NULL);

    /* JSON */
    ctx->json_keywords = g_regex_new("\\b(true|false|null)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->json_string = g_regex_new("\"(\\\\.|[^\"])*\"", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->json_key = g_regex_new("\"(\\\\.|[^\"])*\"\\s*:", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->json_number = g_regex_new("-?(0|[1-9]\\d*)(\\.\\d+)?([eE][+-]?\\d+)?", G_REGEX_OPTIMIZE, 0, NULL);

    /* YAML */
    /* Capture key part (group 1) and optional value part (group 2, if matches colon) */
    /* Keys can be: plain text, "quoted", 'quoted'. Preceded by spaces or '- '. */
    /* Regex: ^(\s*(- )?)?((?:"[^"]*"|'[^']*'|[\w-]+))\s*:(\s+.*)? */
    /* Simplification: Just match line structure in loop. Match Key. Match Value. */
    ctx->yaml_keys = g_regex_new("^(\\s*(?:-\\s+)?)([\"']?[\\w.-]+[\"']?|[\"'].*[\"'])\\s*:", G_REGEX_OPTIMIZE, 0, NULL); 
    ctx->yaml_comment = g_regex_new("#.*$", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->yaml_scalars = g_regex_new("\\b(true|false|null|yes|no)\\b", G_REGEX_OPTIMIZE, 0, NULL);

    /* XML */
    ctx->xml_tag_open = g_regex_new("</?([-\\w.:]+)", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->xml_tag_close = g_regex_new("/?>", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->xml_attr = g_regex_new("\\s([-\\w.:]+)=", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->xml_comment_start = g_regex_new("<!--", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->xml_comment_end = g_regex_new("-->", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->xml_cdata_start = g_regex_new("<!\\[CDATA\\[", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->xml_cdata_end = g_regex_new("\\]\\]>", G_REGEX_OPTIMIZE, 0, NULL);

    /* Desktop Entry */
    ctx->desktop_comment = g_regex_new("#.*$", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->desktop_section = g_regex_new("^\\[[^\\]]+\\]", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->desktop_key = g_regex_new("^([A-Za-z0-9-]+)\\s*(=)", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->desktop_arg = g_regex_new("%[a-zA-Z]", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->desktop_string_dq = g_regex_new("\"(\\\\.|[^\"])*\"", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->desktop_string_sq = g_regex_new("'(?:[^']|'')*'", G_REGEX_OPTIMIZE, 0, NULL);

    /* Initialize line cache */
    ctx->line_cache = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, syntax_cache_entry_free);

    return ctx;
}

void
syntax_context_free(SyntaxContext *ctx)
{
    g_byte_array_unref(ctx->state_chain);
    
    /* C Regexes freed - None used */
    /* Py Regexes freed - None used */


    if (ctx->sh_keywords) g_regex_unref(ctx->sh_keywords);
    if (ctx->sh_builtins) g_regex_unref(ctx->sh_builtins);
    if (ctx->sh_comment) g_regex_unref(ctx->sh_comment);
    if (ctx->sh_variable) g_regex_unref(ctx->sh_variable);
    if (ctx->sh_string) g_regex_unref(ctx->sh_string);
    if (ctx->sh_string_sq) g_regex_unref(ctx->sh_string_sq);

    if (ctx->js_keywords) g_regex_unref(ctx->js_keywords);
    if (ctx->js_builtins) g_regex_unref(ctx->js_builtins);
    if (ctx->js_func_def) g_regex_unref(ctx->js_func_def);
    if (ctx->js_comment_sl) g_regex_unref(ctx->js_comment_sl);
    if (ctx->js_comment_ml_start) g_regex_unref(ctx->js_comment_ml_start);
    if (ctx->js_comment_ml_end) g_regex_unref(ctx->js_comment_ml_end);
    if (ctx->js_string_dq) g_regex_unref(ctx->js_string_dq);
    if (ctx->js_string_sq) g_regex_unref(ctx->js_string_sq);
    if (ctx->js_template) g_regex_unref(ctx->js_template);
    if (ctx->js_number) g_regex_unref(ctx->js_number);

    if (ctx->json_keywords) g_regex_unref(ctx->json_keywords);
    if (ctx->json_string) g_regex_unref(ctx->json_string);
    if (ctx->json_number) g_regex_unref(ctx->json_number);
    if (ctx->json_key) g_regex_unref(ctx->json_key);

    if (ctx->yaml_keys) g_regex_unref(ctx->yaml_keys);
    if (ctx->yaml_comment) g_regex_unref(ctx->yaml_comment);
    if (ctx->yaml_scalars) g_regex_unref(ctx->yaml_scalars);

    if (ctx->xml_tag_open) g_regex_unref(ctx->xml_tag_open);
    if (ctx->xml_tag_close) g_regex_unref(ctx->xml_tag_close);
    if (ctx->xml_attr) g_regex_unref(ctx->xml_attr);
    if (ctx->xml_comment_start) g_regex_unref(ctx->xml_comment_start);
    if (ctx->xml_comment_end) g_regex_unref(ctx->xml_comment_end);
    if (ctx->xml_cdata_start) g_regex_unref(ctx->xml_cdata_start);
    if (ctx->xml_cdata_end) g_regex_unref(ctx->xml_cdata_end);

    if (ctx->desktop_comment) g_regex_unref(ctx->desktop_comment);
    if (ctx->desktop_section) g_regex_unref(ctx->desktop_section);
    if (ctx->desktop_key) g_regex_unref(ctx->desktop_key);
    if (ctx->desktop_arg) g_regex_unref(ctx->desktop_arg);
    if (ctx->desktop_string_dq) g_regex_unref(ctx->desktop_string_dq);
    if (ctx->desktop_string_sq) g_regex_unref(ctx->desktop_string_sq);
    
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
    } else if (strcmp(lang_name, "javascript") == 0 || strcmp(lang_name, "js") == 0 || strcmp(lang_name, "ts") == 0) {
        ctx->lang = LANG_JAVASCRIPT;
    } else if (strcmp(lang_name, "json") == 0) {
        ctx->lang = LANG_JSON;
    } else if (strcmp(lang_name, "yaml") == 0 || strcmp(lang_name, "yml") == 0) {
        ctx->lang = LANG_YAML;
    } else if (strcmp(lang_name, "xml") == 0 || strcmp(lang_name, "html") == 0 || strcmp(lang_name, "xsl") == 0 || strcmp(lang_name, "svg") == 0) {
        ctx->lang = LANG_XML;
    } else if (strcmp(lang_name, "desktop") == 0) {
        ctx->lang = LANG_DESKTOP;
    } else {
        ctx->lang = LANG_NONE;
    }
    
    /* Clear states */
    g_byte_array_set_size(ctx->state_chain, 0);
}

const char *
syntax_context_get_language_name(SyntaxContext *ctx)
{
    if (!ctx) return "Plain Text";
    switch (ctx->lang) {
        case LANG_C: return "C";
        case LANG_PYTHON: return "Python";
        case LANG_BASH: return "Shell Script";
        case LANG_JAVASCRIPT: return "JavaScript";
        case LANG_JSON: return "JSON";
        case LANG_YAML: return "YAML";
        case LANG_XML: return "XML/HTML";
        case LANG_DESKTOP: return "Desktop Entry";
        case LANG_NONE: 
        default: return "Plain Text";
    }
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

/* Helper to add attribute - selects color based on theme */
static void
add_attr(PangoAttrList *attrs, int start, int end, const PangoColor *color_ref)
{
    if (start >= end) return;
    
    /* Map reference pointer to actual color based on mode */
    const PangoColor *effective_color = color_ref;
    
    /* Determine if mapped to dark or light palette */
    if (is_dark_mode) {
        /* Already pointing to dark usually, but ensure mapping if we used generic pointers */
        if (color_ref == &l_keyword) effective_color = &d_keyword;
        else if (color_ref == &l_type) effective_color = &d_type;
        else if (color_ref == &l_string) effective_color = &d_string;
        else if (color_ref == &l_comment) effective_color = &d_comment;
        else if (color_ref == &l_preproc) effective_color = &d_preproc;
        else if (color_ref == &l_number) effective_color = &d_number;
        else if (color_ref == &l_function) effective_color = &d_function;
        else if (color_ref == &l_variable) effective_color = &d_variable;
    } else {
        /* If pointers are to dark (default statics), map to light */
        if (color_ref == &d_keyword || color_ref == &d_number || color_ref == &d_variable) effective_color = &l_keyword; /* Bool/Self map to Keyword/Preproc in light simplified */
        else if (color_ref == &d_type) effective_color = &l_type;
        else if (color_ref == &d_string) effective_color = &l_string;
        else if (color_ref == &d_comment) effective_color = &l_comment;
        else if (color_ref == &d_preproc) effective_color = &l_preproc;
        else if (color_ref == &d_number) effective_color = &l_number;
        else if (color_ref == &d_function) effective_color = &l_function;
        else if (color_ref == &d_variable) effective_color = &l_variable;
        
        /* Refine: color_bool (Orange) -> l_number (Orange) or l_keyword (Purple)? SVite uses numeric color? 
           One Light: Constants are Orange. 
        */
        if (color_ref == &d_number || color_ref == &d_type || color_ref == &d_number) effective_color = &l_number; 
        if (color_ref == &d_variable) effective_color = &l_variable;
        if (color_ref == &d_preproc) effective_color = &l_preproc;
    }
    
    PangoAttribute *attr = pango_attr_foreground_new(effective_color->red, effective_color->green, effective_color->blue);
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

static const char *c_keywords[] = {
    "auto", "bool", "break", "case", "catch", "char", "class", "const", "continue", 
    "default", "delete", "do", "double", "else", "enum", "extern", "false", "float", 
    "for", "friend", "goto", "if", "inline", "int", "long", "namespace", "new", 
    "operator", "private", "protected", "public", "register", "restrict", "return", 
    "short", "signed", "sizeof", "static", "struct", "switch", "template", "this", 
    "throw", "true", "try", "typedef", "typename", "union", "unsigned", "using", 
    "virtual", "void", "volatile", "while", "NULL", "_Bool", "_Complex", "_Imaginary", 
    NULL
};

/* Python Keyword Lists */
static const char *py_keywords[] = {
    "as", "assert", "async", "await", "break", "class", "continue", "def", "del",
    "elif", "else", "except", "finally", "for", "from", "global", "if", "import",
    "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try",
    "while", "with", "yield", NULL
};

static const char *py_bools[] = {
    "False", "None", "True", "and", NULL
};

static const char *py_builtins[] = {
    "abs", "all", "any", "ascii", "bin", "bool", "bytearray", "bytes", "callable",
    "chr", "classmethod", "compile", "complex", "delattr", "dict", "dir", "divmod",
    "enumerate", "eval", "exec", "filter", "float", "format", "frozenset", "getattr",
    "globals", "hasattr", "hash", "help", "hex", "id", "input", "int", "isinstance",
    "issubclass", "iter", "len", "list", "locals", "map", "max", "memoryview", "min",
    "next", "object", "oct", "open", "ord", "pow", "print", "property", "range",
    "repr", "reversed", "round", "set", "setattr", "slice", "sorted", "staticmethod",
    "str", "sum", "super", "tuple", "type", "vars", "zip", "__import__", "__init__", NULL
};

static gboolean 
is_word_in_list(const char *word, size_t len, const char **list) 
{
    /* Simple linear scan for now */
    for (int i = 0; list[i]; i++) {
        if (strncmp(word, list[i], len) == 0 && list[i][len] == '\0') return TRUE;
    }
    return FALSE;
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
        while (cur < len) {
            if (state == STATE_IN_ML_COMMENT) {
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
                add_attr(attrs, start_pos, cur, &d_comment);
                continue;
            }
            
            if (state == STATE_ROOT) {
                /* Comments */
                if (text[cur] == '/' && cur+1 < len && text[cur+1] == '/') {
                    add_attr(attrs, cur, len, &d_comment);
                    cur = len;
                    continue;
                }
                if (text[cur] == '/' && cur+1 < len && text[cur+1] == '*') {
                    state = STATE_IN_ML_COMMENT;
                    size_t start_pos = cur;
                    cur += 2;
                    while (cur + 1 < len) {
                        if (text[cur] == '*' && text[cur+1] == '/') {
                            cur += 2;
                            state = STATE_ROOT;
                            break;
                        }
                        cur++;
                    }
                    if (state == STATE_IN_ML_COMMENT) cur = len;
                    add_attr(attrs, start_pos, cur, &d_comment);
                    continue;
                }

                /* Strings */
                if (text[cur] == '"') {
                    size_t start_pos = cur;
                    cur++;
                    while (cur < len) {
                         if (text[cur] == '"' && text[cur-1] != '\\') {
                             cur++;
                             break;
                         }
                         cur++;
                    }
                    add_attr(attrs, start_pos, cur, &d_string);
                    continue;
                }
                if (text[cur] == '\'') {
                    size_t start_pos = cur;
                    cur++;
                    while (cur < len) {
                         if (text[cur] == '\'' && text[cur-1] != '\\') {
                             cur++;
                             break;
                         }
                         cur++;
                    }
                    add_attr(attrs, start_pos, cur, &d_string);
                    continue;
                }
                
                /* Preprocessor */
                if (text[cur] == '#') {
                    size_t start_pos = cur;
                    cur++;
                    while (cur < len && g_ascii_isalpha(text[cur])) {
                        cur++;
                    }
                    add_attr(attrs, start_pos, cur, &d_preproc);
                    continue;
                }

                /* Numbers */
                if (g_ascii_isdigit(text[cur])) {
                    size_t start_pos = cur;
                    if (text[cur] == '0' && cur+1 < len && (text[cur+1] == 'x' || text[cur+1] == 'X')) {
                        cur += 2;
                        while (cur < len && g_ascii_isxdigit(text[cur])) cur++;
                    } else {
                        while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '.')) cur++;
                    }
                    add_attr(attrs, start_pos, cur, &d_number);
                    continue;
                }

                /* Identifiers */
                if (g_ascii_isalpha(text[cur]) || text[cur] == '_') {
                    size_t start_pos = cur;
                    while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) {
                        cur++;
                    }
                    size_t word_len = cur - start_pos;
                    const char *word_start = text + start_pos;
                    
                    /* Check function call: word followed by ( */
                    gboolean is_func_call = FALSE;
                    size_t peek = cur;
                    while (peek < len && g_ascii_isspace(text[peek])) peek++;
                    if (peek < len && text[peek] == '(') is_func_call = TRUE;

                    if (is_word_in_list(word_start, word_len, c_keywords)) {
                        add_attr(attrs, start_pos, cur, &d_keyword);
                    } else if (is_func_call) {
                         add_attr(attrs, start_pos, cur, &d_function);
                    } else if (g_ascii_isupper(word_start[0]) || (word_len > 2 && word_start[word_len-1] == 't' && word_start[word_len-2] == '_')) {
                        add_attr(attrs, start_pos, cur, &d_type);
                    }
                    /* Else Plain */
                    
                    continue;
                }

                cur++;
            } else {
                /* Should not be here if STATE_IN_ML_COMMENT handled above */
                 state = STATE_ROOT;
                 cur++;
            }
        }
    }
 else if (ctx->lang == LANG_PYTHON) {
        gboolean expect_func = FALSE;
        gboolean expect_class = FALSE;

        /* If continuation of multiline string, handle immediately */
        if (state == STATE_IN_TRIPLE_DQ_STRING) {
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
            add_attr(attrs, start_pos, cur, &d_string);
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
            add_attr(attrs, start_pos, cur, &d_string);
        }

        while (cur < len) {
            /* String States (Inline checks if not already in state) */
            if (state == STATE_ROOT) {
                /* Triple Strings */
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
                    add_attr(attrs, start_pos, cur, &d_string);
                    continue;
                }
                if (g_str_has_prefix(text + cur, "'''")) {
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
                    add_attr(attrs, start_pos, cur, &d_string);
                    continue;
                }
                /* Single Strings */
                if (text[cur] == '"') {
                    size_t start_pos = cur;
                    cur++;
                    while (cur < len) {
                        if (text[cur] == '"' && text[cur-1] != '\\') {
                            cur++;
                            break;
                        }
                        cur++;
                    }
                    add_attr(attrs, start_pos, cur, &d_string);
                    continue;
                }
                if (text[cur] == '\'') {
                    size_t start_pos = cur;
                    cur++;
                    while (cur < len) {
                        if (text[cur] == '\'' && text[cur-1] != '\\') {
                            cur++;
                            break;
                        }
                        cur++;
                    }
                    add_attr(attrs, start_pos, cur, &d_string);
                    continue;
                }
                /* Comment */
                if (text[cur] == '#') {
                    add_attr(attrs, cur, len, &d_comment);
                    cur = len;
                    continue;
                }
                
                /* Numbers */
                if (g_ascii_isdigit(text[cur])) {
                    size_t start_pos = cur;
                    while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '.')) {
                        cur++;
                    }
                    add_attr(attrs, start_pos, cur, &d_number);
                    continue;
                }

                /* Identifiers / Keywords */
                if (g_ascii_isalpha(text[cur]) || text[cur] == '_') {
                    size_t start_pos = cur;
                    while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) {
                        cur++;
                    }
                    size_t word_len = cur - start_pos;
                    const char *word_start = text + start_pos;

                    /* Check Helpers (self) or Function/Class Definitions */
                    if (expect_func) {
                        add_attr(attrs, start_pos, cur, &d_function);
                        expect_func = FALSE;
                    } 
                    else if (expect_class) {
                        add_attr(attrs, start_pos, cur, &d_type);
                        expect_class = FALSE;
                    }
                    else if (strncmp(word_start, "self", word_len) == 0 && word_len == 4) {
                        add_attr(attrs, start_pos, cur, &d_variable);
                    }
                    else if (is_word_in_list(word_start, word_len, py_keywords)) {
                        add_attr(attrs, start_pos, cur, &d_keyword);
                        /* Check if this keyword triggers next-token coloring */
                        if (strncmp(word_start, "def", word_len) == 0 && word_len == 3) expect_func = TRUE;
                        else if (strncmp(word_start, "class", word_len) == 0 && word_len == 5) expect_class = TRUE;
                    }
                    else if (is_word_in_list(word_start, word_len, py_bools)) {
                        add_attr(attrs, start_pos, cur, &d_number); /* Orange for bools */
                    }
                    else if (is_word_in_list(word_start, word_len, py_builtins)) {
                        add_attr(attrs, start_pos, cur, &d_builtin);
                    }
                    /* Else plain/variable */
                    continue;
                }

                /* Decorators */
                if (text[cur] == '@') {
                    size_t start_pos = cur;
                    cur++;
                    while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) {
                        cur++;
                    }
                    add_attr(attrs, start_pos, cur, &d_decorator);
                    continue;
                }

                /* Skip other chars */
                cur++;
            } 
            else {
                /* Should handle multi-line string states inside loop if needed, 
                   but we handled the initial entry. If we are here, we are presumably 
                   scanning inside a string?
                   Wait, lines 745+ handled 'start_state == IN_TRIPLE...'.
                   We need to handle transitions *back* to string state if we re-enter it?
                   No, 'state' variable is updated.
                   We need logic for when 'state != ROOT' inside the loop.
                   Lines 830+ in original code had logic for this.
                   My replacement logic above (745-770) handles it *before* the loop for initial state.
                   Inside the loop, I need: */
                if (state == STATE_IN_TRIPLE_DQ_STRING) {
                    while (cur + 2 < len) {
                        if (g_str_has_prefix(text + cur, "\"\"\"") && text[cur-1] != '\\') {
                            cur += 3;
                            state = STATE_ROOT;
                            goto state_root_check; /* Jump back to root check? Or continue */
                        }
                        cur++;
                    }
                    if (state == STATE_IN_TRIPLE_DQ_STRING) cur = len;
                    /* Attr added? We need to add attr for this chunk? */
                    /* No, usually we add attr for whole line if single state? */
                    /* Wait, linear tokenizer adds attrs incrementally. */
                }
                else if (state == STATE_IN_TRIPLE_SQ_STRING) {
                     while (cur + 2 < len) {
                        if (g_str_has_prefix(text + cur, "'''") && text[cur-1] != '\\') {
                            cur += 3;
                            state = STATE_ROOT;
                            goto state_root_check;
                        }
                        cur++;
                    }
                    if (state == STATE_IN_TRIPLE_SQ_STRING) cur = len;
                }
                else {
                    /* Should not happen */
                    cur++;
                }
                
                state_root_check:;
            }
        }
    } else if (ctx->lang == LANG_BASH) {
        gboolean is_command_pos = TRUE;
        
        while (cur < len) {
             /* Handle Spaces */
             if (g_ascii_isspace(text[cur])) {
                 if (text[cur] == '\n') is_command_pos = TRUE;
                 cur++;
                 continue;
             }
             
             /* Comments */
             if (text[cur] == '#') {
                 add_attr(attrs, cur, len, &d_comment);
                 cur = len;
                 continue;
             }
             
             /* Strings - Double Quote */
             if (text[cur] == '"') {
                 size_t start = cur++;
                 while (cur < len) {
                     if (text[cur] == '"' && text[cur-1] != '\\') {
                         cur++;
                         break;
                     }
                     cur++;
                 }
                 add_attr(attrs, start, cur, &d_string);
                 is_command_pos = FALSE; 
                 continue;
             }
             
             /* Strings - Single Quote */
             if (text[cur] == '\'') {
                 size_t start = cur++;
                 while (cur < len) {
                     if (text[cur] == '\'') {
                         cur++;
                         break;
                     }
                     cur++;
                 }
                 add_attr(attrs, start, cur, &d_string);
                 is_command_pos = FALSE;
                 continue;
             }

             /* Backticks (Command Substitution) - Treat as string for now */
             if (text[cur] == '`') {
                 size_t start = cur++;
                 while (cur < len) {
                     if (text[cur] == '`' && text[cur-1] != '\\') {
                         cur++;
                         break;
                     }
                     cur++;
                 }
                 add_attr(attrs, start, cur, &d_string);
                 is_command_pos = FALSE;
                 continue;
             }
             
             /* Variables */
             if (text[cur] == '$') {
                 size_t start = cur;
                 /* Simple variable match */
                 GMatchInfo *mi_var;
                 if (g_regex_match(ctx->sh_variable, text + cur, 0, &mi_var)) {
                     int s, e;
                     if (g_match_info_fetch_pos(mi_var, 0, &s, &e)) {
                         add_attr(attrs, cur + s, cur + e, &d_variable);
                         cur += e;
                     } else {
                         cur++;
                     }
                     g_match_info_free(mi_var);
                 } else {
                     cur++;
                 }
                 is_command_pos = FALSE;
                 continue;
             }
             
             /* Operators - Reset Command Pos */
             /* | & ; ( ) { } < > */
             if (strchr("|&;(){}<>", text[cur])) {
                 if (text[cur] == ';' || text[cur] == '|' || text[cur] == '&') {
                     is_command_pos = TRUE;
                 }
                 cur++;
                 continue;
             }

             /* Escapes \ */
             if (text[cur] == '\\') {
                 cur++;
                 if (cur < len) cur++;
                 continue;
             }
             
             /* Words (Commands, Keywords, Args) */
             /* Delimiters: space, quotes, operators, $ (var), ` (backtick), [ ] (test) */
             /* Added [] and ` to delimiters checking */
             if (!strchr(" \t\n\"'#|&;(){}$[]`<>", text[cur])) {
                 size_t start = cur;
                 while (cur < len && !strchr(" \t\n\"'#|&;(){}$[]`<>", text[cur])) {
                     cur++;
                 }
                 
                 int word_len = cur - start;
                 char *word = g_strndup(text + start, word_len);
                 
                 /* Check Keyword */
                 GMatchInfo *mi_kw;
                 if (g_regex_match(ctx->sh_keywords, word, 0, &mi_kw)) {
                     if (g_match_info_matches(mi_kw)) {
                          int s, e;
                          g_match_info_fetch_pos(mi_kw, 0, &s, &e);
                          if (e - s == word_len) {
                              add_attr(attrs, start, cur, &d_keyword);
                              
                              if (g_strcmp0(word, "if") == 0 || g_strcmp0(word, "then") == 0 || 
                                  g_strcmp0(word, "else") == 0 || g_strcmp0(word, "elif") == 0 ||
                                  g_strcmp0(word, "do") == 0 || g_strcmp0(word, "while") == 0 ||
                                  g_strcmp0(word, "until") == 0 || g_strcmp0(word, "time") == 0 ||
                                  g_strcmp0(word, "fi") == 0 || g_strcmp0(word, "done") == 0 ||
                                  g_strcmp0(word, "esac") == 0) {
                                  is_command_pos = TRUE;
                              } else {
                                  is_command_pos = FALSE;
                              }
                              g_match_info_free(mi_kw);
                              g_free(word);
                              continue;
                          }
                     }
                     g_match_info_free(mi_kw);
                 }
                 
                 /* Check Argument (starts with -) */
                 if (word[0] == '-') {
                     add_attr(attrs, start, cur, &d_param);
                     is_command_pos = FALSE;
                 }
                 /* Check Command Position */
                 else if (is_command_pos) {
                     add_attr(attrs, start, cur, &d_function);
                     is_command_pos = FALSE;
                 }
                 
                 g_free(word);
                 continue;
             }
             
             cur++;
        }
         

    } else if (ctx->lang == LANG_JAVASCRIPT) {
        /* JS Keywords */
        GMatchInfo *mi;
        if (g_regex_match(ctx->js_keywords, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_attr(attrs, s, e, &d_keyword);
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        if (g_regex_match(ctx->js_builtins, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_attr(attrs, s, e, &d_builtin);
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        /* Func defs: function name */
        if (g_regex_match(ctx->js_func_def, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int ks, ke, ns, ne;
                if (g_match_info_fetch_pos(mi, 1, &ks, &ke)) add_attr(attrs, ks, ke, &d_keyword);
                if (g_match_info_fetch_pos(mi, 2, &ns, &ne)) add_attr(attrs, ns, ne, &d_function);
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        /* Numbers */
        if (g_regex_match(ctx->js_number, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_attr(attrs, s, e, &d_number);
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        /* Strings/Comments state machine */
        while (cur < len) {
            if (state == STATE_ROOT) {
                if (text[cur] == '"') {
                    state = STATE_IN_DOUBLE_QUOTE;
                    size_t start_pos = cur;
                    cur++;
                    while (cur < len) {
                        if (text[cur] == '"' && text[cur-1] != '\\') {
                            cur++;
                            state = STATE_ROOT;
                            break;
                        }
                        cur++;
                    }
                    add_attr(attrs, start_pos, cur, &d_string);
                } else if (text[cur] == '\'') {
                    state = STATE_IN_SINGLE_QUOTE;
                    size_t start_pos = cur;
                    cur++;
                    while (cur < len) {
                        if (text[cur] == '\'' && text[cur-1] != '\\') {
                            cur++;
                            state = STATE_ROOT;
                            break;
                        }
                        cur++;
                    }
                    add_attr(attrs, start_pos, cur, &d_string);
                } else if (text[cur] == '`') {
                    /* Template literal - simplify as single line for now or simple multiline */
                    /* Note: multiline templates needs state. Re-use TRIPLE_DQ state slot for Template? */
                    state = STATE_IN_TRIPLE_DQ_STRING; /* reusing state 5 */
                    size_t start_pos = cur;
                    cur++;
                    while (cur < len) {
                        if (text[cur] == '`' && text[cur-1] != '\\') {
                            cur++;
                            state = STATE_ROOT;
                            break;
                        }
                        cur++;
                    }
                    add_attr(attrs, start_pos, cur, &d_string);
                } else if (text[cur] == '/' && cur+1 < len && text[cur+1] == '/') {
                    add_attr(attrs, cur, len, &d_comment);
                    cur = len;
                } else if (text[cur] == '/' && cur+1 < len && text[cur+1] == '*') {
                    state = STATE_IN_ML_COMMENT;
                    size_t start_pos = cur;
                    cur += 2;
                    while (cur + 1 < len) {
                        if (text[cur] == '*' && text[cur+1] == '/') {
                            cur += 2;
                            state = STATE_ROOT;
                            break;
                        }
                        cur++;
                    }
                    add_attr(attrs, start_pos, cur, &d_comment);
                } else {
                    cur++;
                }
            } else if (state == STATE_IN_DOUBLE_QUOTE) {
                size_t start_pos = cur;
                while (cur < len) {
                    if (text[cur] == '"' && (cur==0 || text[cur-1] != '\\')) {
                        cur++;
                        state = STATE_ROOT;
                        break;
                    }
                    cur++;
                }
                add_attr(attrs, start_pos, cur, &d_string);
            } else if (state == STATE_IN_SINGLE_QUOTE) {
                size_t start_pos = cur;
                while (cur < len) {
                    if (text[cur] == '\'' && (cur==0 || text[cur-1] != '\\')) {
                        cur++;
                        state = STATE_ROOT;
                        break;
                    }
                    cur++;
                }
                add_attr(attrs, start_pos, cur, &d_string);
            } else if (state == STATE_IN_ML_COMMENT) {
                size_t start_pos = cur;
                while (cur + 1 < len) {
                    if (text[cur] == '*' && text[cur+1] == '/') {
                        cur += 2;
                        state = STATE_ROOT;
                        break;
                    }
                    cur++;
                }
                add_attr(attrs, start_pos, cur, &d_comment);
            } else if (state == STATE_IN_TRIPLE_DQ_STRING) { /* Template literal continuation */
                size_t start_pos = cur;
                while (cur < len) {
                    if (text[cur] == '`' && (cur==0 || text[cur-1] != '\\')) {
                        cur++;
                        state = STATE_ROOT;
                        break;
                    }
                    cur++;
                }
                add_attr(attrs, start_pos, cur, &d_string);
            } else {
                state = STATE_ROOT;
                cur++;
            }
        }
    } else if (ctx->lang == LANG_JSON) {
        GMatchInfo *mi;
        if (g_regex_match(ctx->json_key, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                /* Key is like "foo": - highlight "foo" as type/key, : as punctuation */
                add_attr(attrs, s, e-1, &d_variable); /* JSON Key -> Red (Variable) like Svite */
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        if (g_regex_match(ctx->json_string, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_attr(attrs, s, e, &d_string);
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        if (g_regex_match(ctx->json_keywords, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_attr(attrs, s, e, &d_number); /* true/false -> Orange (Number) in Svite JSON */
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        if (g_regex_match(ctx->json_number, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_attr(attrs, s, e, &d_number);
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
    } else if (ctx->lang == LANG_YAML) {
        GMatchInfo *mi;
        
        /* Keys (Red) and Values (Green) */
        if (g_regex_match(ctx->yaml_keys, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int pre_s, pre_e;
                int key_s, key_e;
                int full_s, full_e;
                
                /* Group 1: Prefix (- )?, Group 2: Key. Full Match: Prefix+Key+Colon */
                g_match_info_fetch_pos(mi, 0, &full_s, &full_e);
                
                /* Default everything after the key match to Green (String) */
                /* This handles "key: value" where value is unquoted text */
                add_attr(attrs, full_e, len, &d_string);
                
                /* Highlight the Key itself (Group 2) in Red */
                if (g_match_info_fetch_pos(mi, 2, &key_s, &key_e)) {
                    add_attr(attrs, key_s, key_e, &d_variable); /* YAML Key -> Red */
                }
                
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        /* Scalars (True/False/Null) - Orange */
        if (g_regex_match(ctx->yaml_scalars, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_attr(attrs, s, e, &d_number);
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        /* Comments - Grey (Last to override others) */
        if (g_regex_match(ctx->yaml_comment, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_attr(attrs, s, e, &d_comment);
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
    } else if (ctx->lang == LANG_XML) {
        GMatchInfo *mi;
        /* Comments */
        if (g_regex_match(ctx->xml_comment_start, text, 0, &mi)) {
             while (g_match_info_matches(mi)) {
                 int s, e;
                 g_match_info_fetch_pos(mi, 0, &s, &e);
                 add_attr(attrs, s, len, &d_comment); /* Assume single line or until end */
                 /* TODO: Proper state machine for XML comments */
                 state = STATE_IN_ML_COMMENT;
                 g_match_info_next(mi, NULL);
             }
        }
        g_match_info_free(mi);
        
        /* Tags */
        if (state == STATE_ROOT) {
            if (g_regex_match(ctx->xml_tag_open, text, 0, &mi)) {
                while (g_match_info_matches(mi)) {
                    int s, e;
                    g_match_info_fetch_pos(mi, 0, &s, &e);
                    add_attr(attrs, s, e, &d_tag); /* Red tags */
                    g_match_info_next(mi, NULL);
                }
            }
            g_match_info_free(mi);
            
            if (g_regex_match(ctx->xml_tag_close, text, 0, &mi)) {
                while (g_match_info_matches(mi)) {
                    int s, e;
                    g_match_info_fetch_pos(mi, 0, &s, &e);
                    add_attr(attrs, s, e, &d_tag);
                    g_match_info_next(mi, NULL);
                }
            }
            g_match_info_free(mi);
            
            /* Attributes */
            if (g_regex_match(ctx->xml_attr, text, 0, &mi)) {
                while (g_match_info_matches(mi)) {
                    int s, e;
                    g_match_info_fetch_pos(mi, 1, &s, &e); /* Group 1 name */
                    add_attr(attrs, s, e, &d_attribute); /* Orange attributes */
                    g_match_info_next(mi, NULL);
                }
            }
            g_match_info_free(mi);
            
            /* Strings (values) - Simple regex */
            GRegex *dq = g_regex_new("\"[^\"]*\"", G_REGEX_OPTIMIZE, 0, NULL);
            if (g_regex_match(dq, text, 0, &mi)) {
                while (g_match_info_matches(mi)) {
                    int s, e;
                    g_match_info_fetch_pos(mi, 0, &s, &e);
                    add_attr(attrs, s, e, &d_string);
                    g_match_info_next(mi, NULL);
                }
            }
            g_match_info_free(mi);
            g_regex_unref(dq);
        }
    } else if (ctx->lang == LANG_DESKTOP) {
        GMatchInfo *mi;
        if (g_regex_match(ctx->desktop_comment, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_attr(attrs, s, e, &d_comment);
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        if (g_regex_match(ctx->desktop_section, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_attr(attrs, s, e, &d_keyword); /* Keywords color for Section */
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        if (g_regex_match(ctx->desktop_key, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int ks, ke;
                int full_s, full_e;
                g_match_info_fetch_pos(mi, 0, &full_s, &full_e);
                g_match_info_fetch_pos(mi, 1, &ks, &ke); /* Group 1 key */
                add_attr(attrs, ks, ke, &d_tag); /* Key -> Red */
                
                /* Highlight everything after the key match (=) as string (Green) */
                /* The regex matches "Key=", so full_e is after = */
                add_attr(attrs, full_e, len, &d_string); 
                
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        if (g_regex_match(ctx->desktop_arg, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_attr(attrs, s, e, &d_param); /* Argument -> Orange */
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        /* Strings */
        if (g_regex_match(ctx->desktop_string_dq, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_attr(attrs, s, e, &d_string);
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        if (g_regex_match(ctx->desktop_string_sq, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_attr(attrs, s, e, &d_string);
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
    }
    
    /* Save end state ... */
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
