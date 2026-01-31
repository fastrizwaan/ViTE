#include "syntax-internal.h"
#include <string.h>

/* --- Colors --- */
gboolean is_dark_mode = TRUE;

/* Dark Theme (One Dark) */
PangoColor d_keyword;   /* #c678dd Purple */
PangoColor d_builtin;   /* #56b6c2 Cyan */
PangoColor d_string;    /* #98c379 Green */
PangoColor d_comment;   /* #7f848e Grey */
PangoColor d_number;    /* #d19a66 Orange */
PangoColor d_function;  /* #61afef Blue */
PangoColor d_type;      /* #e5c07b Yellow/Gold (Class) */
PangoColor d_decorator; /* #56b6c2 Cyan */
PangoColor d_variable;  /* #e06c75 Red */
PangoColor d_variable_c; /* #d1d1d1 Light Grey */
PangoColor d_constant;  /* #e06c75 Red (Macros, Enums) */
PangoColor d_tag;       /* #e06c75 Red */
PangoColor d_operator;     /* #d19a66 Orange */
PangoColor d_punctuation;  /* #d19a66 Orange */
PangoColor d_attribute; /* #d19a66 Orange */
PangoColor d_param;     /* #d19a66 Orange (Argument) */
PangoColor d_property;  /* #56b6c2 Cyan */
PangoColor d_preproc;   /* #c678dd Purple */
PangoColor d_logical;   /* #56b6c2 Cyan */

/* Light Theme (One Light) */
PangoColor l_keyword;   /* #a626a4 Purple */
PangoColor l_builtin;   /* #0184bc Cyan/Blue */
PangoColor l_string;    /* #50a14f Green */
PangoColor l_comment;   /* #5c6370 Grey */
PangoColor l_number;    /* #986801 Orange */
PangoColor l_operator;     /* #986801 Orange */
PangoColor l_punctuation;  /* #986801 Orange */
PangoColor l_function;  /* #4078f2 Blue */
PangoColor l_type;      /* #c18401 Orange/Gold */
PangoColor l_decorator; /* #a626a4 Purple */
PangoColor l_variable;  /* #e45649 Red */
PangoColor l_variable_c; /* #383a42 Dark Grey */
PangoColor l_constant;  /* #e45649 Red */
PangoColor l_tag;       /* #e45649 Red */
PangoColor l_attribute; /* #986801 Orange */
PangoColor l_param;     /* #986801 Orange */
PangoColor l_property;  /* #0184bc Cyan */
PangoColor l_preproc;   /* #a626a4 Purple */
PangoColor l_logical;   /* #0184bc Cyan */

static gboolean colors_initialized = FALSE;

static void
syntax_cache_entry_free(gpointer data)
{
    if (!data) return;
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
    pango_color_parse(&d_comment, "#7f848e");
    pango_color_parse(&d_number, "#d19a66");
    pango_color_parse(&d_function, "#61afef");
    pango_color_parse(&d_type, "#e5c07b");
    pango_color_parse(&d_decorator, "#56b6c2");
    pango_color_parse(&d_variable, "#e06c75");
    pango_color_parse(&d_variable_c, "#d1d1d1");
    pango_color_parse(&d_constant, "#e06c75");
    pango_color_parse(&d_tag, "#e06c75");
    pango_color_parse(&d_operator, "#d19a66");
    pango_color_parse(&d_logical, "#56b6c2");
    pango_color_parse(&d_punctuation, "#d19a66");
    pango_color_parse(&d_attribute, "#d19a66");
    pango_color_parse(&d_param, "#e06c75");
    pango_color_parse(&d_property, "#56b6c2");
    pango_color_parse(&d_preproc, "#c678dd");

    /* Light */
    pango_color_parse(&l_keyword, "#a626a4");
    pango_color_parse(&l_builtin, "#0184bc");
    pango_color_parse(&l_string, "#50a14f");
    pango_color_parse(&l_comment, "#5c6370");
    pango_color_parse(&l_number, "#986801");
    pango_color_parse(&l_operator, "#986801");
    pango_color_parse(&l_logical, "#0184bc");
    pango_color_parse(&l_punctuation, "#986801");
    pango_color_parse(&l_function, "#4078f2");
    pango_color_parse(&l_type, "#c18401");
    pango_color_parse(&l_decorator, "#a626a4");
    pango_color_parse(&l_variable, "#e45649");
    pango_color_parse(&l_variable_c, "#383a42");
    pango_color_parse(&l_constant, "#e45649");
    pango_color_parse(&l_tag, "#e45649");
    pango_color_parse(&l_attribute, "#986801");
    pango_color_parse(&l_param, "#e45649");
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
    
    SyntaxContext *ctx = g_new0(SyntaxContext, 1);
    ctx->ref_count = 1;
    ctx->lang = LANG_NONE;
    ctx->state_chain = g_byte_array_new();
    ctx->valid_up_to = 0;
    
    /* Compile Regexes */
    
    /* Bash */
    ctx->sh_keywords = g_regex_new("\\b(if|then|else|elif|fi|case|esac|for|select|while|until|do|done|in|function|time|coproc|declare|typeset|local|readonly|export|unset|set|shopt|trap|source|alias|unalias|break|continue|return|exit|eval|exec)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->sh_builtins = g_regex_new("\\b(echo|printf|cd|pwd|ls|cp|mv|rm|mkdir|rmdir|touch|cat|grep|sed|awk|find|chmod|chown|kill|ps|jobs|bg|fg|history|read|wait|sleep|true|false|make|install|flatpak|git|node|npm|pip|python|python3|gcc|g\\+\\+|clang|docker|systemctl|journalctl)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    /* ctx->sh_comment, etc - migrated to linear scanner but kept some for partial match inside loop if needed, 
       but actually the linear scanner does manual matching now for most things to avoid regex overhead per char.
       We kept sh_variable usage in syntax-shell.c though. */
    ctx->sh_variable = g_regex_new("(\\$[a-zA-Z_][a-zA-Z0-9_]*|\\$\\{[^}]+\\}|\\$[0-9*@#?!$-])", G_REGEX_OPTIMIZE, 0, NULL);
    
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
    ctx->line_cache = g_ptr_array_new_with_free_func(syntax_cache_entry_free);

    return ctx;
}

void
syntax_context_free(SyntaxContext *ctx)
{
    syntax_context_unref(ctx);
}

SyntaxContext *
syntax_context_ref(SyntaxContext *ctx)
{
    if (ctx) g_atomic_int_inc(&ctx->ref_count);
    return ctx;
}

void
syntax_context_unref(SyntaxContext *ctx)
{
    if (!ctx) return;
    if (!g_atomic_int_dec_and_test(&ctx->ref_count)) return;

    if (ctx->state_chain) g_byte_array_unref(ctx->state_chain);
    
    if (ctx->sh_keywords) g_regex_unref(ctx->sh_keywords);
    if (ctx->sh_builtins) g_regex_unref(ctx->sh_builtins);
    if (ctx->sh_variable) g_regex_unref(ctx->sh_variable);

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

    if (ctx->line_cache) {
        g_ptr_array_unref(ctx->line_cache);
    }
    
    g_free(ctx);
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
    } else if (strcmp(lang_name, "rust") == 0 || strcmp(lang_name, "rs") == 0 || strcmp(lang_name, "rst") == 0) {
        ctx->lang = LANG_RUST;
    } else {
        ctx->lang = LANG_NONE;
    }
    
    /* Clear states and cache */
    g_byte_array_set_size(ctx->state_chain, 0);
    ctx->valid_up_to = 0;
    if (ctx->line_cache) g_ptr_array_set_size(ctx->line_cache, 0);
}

SyntaxLanguage
syntax_context_get_language(SyntaxContext *ctx)
{
    if (!ctx) return LANG_NONE;
    return ctx->lang;
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
        case LANG_RUST: return "Rust";
        case LANG_NONE: 
        default: return "Plain Text";
    }
}

const char *
syntax_detect_language(const char *content)
{
    if (!content) return NULL;
    
    /* Shebang Check */
    if (g_str_has_prefix(content, "#!")) {
        if (strstr(content, "bash") || strstr(content, "sh")) return "bash";
        if (strstr(content, "python")) return "python";
        if (strstr(content, "node")) return "javascript";
        if (strstr(content, "perl")) return NULL; /* Not supported yet */
    }
    
    /* XML / HTML */
    if (strstr(content, "<?xml") || strstr(content, "<!DOCTYPE html") || strstr(content, "<html")) {
        return "xml";
    }
    
    /* Desktop Entry */
    if (strstr(content, "[Desktop Entry]")) {
        return "desktop";
    }
    
    /* C Headers */
    if (strstr(content, "#include <") || strstr(content, "#include \"")) {
        return "c";
    }
    
    /* JSON (heuristic: starts with { or [ and looks like valid JSON start) */
    /* Skip whitespace */
    const char *p = content;
    while (*p && g_ascii_isspace(*p)) p++;
    if (*p == '{' || *p == '[') {
        if (strstr(content, "\"")) return "json";
    }
    
    /* Rust Heuristics */
    if (strstr(content, "fn main()") || strstr(content, "use std::") || 
        strstr(content, "pub mod") || strstr(content, "pub struct") || 
        strstr(content, "pub fn") || strstr(content, "#[derive(")) {
        return "rust";
    }
    
    /* Python Heuristics */
    /* Look for "def " or "import " at start of lines, or "if __name__" */
    if (strstr(content, "def ") && strstr(content, ":")) return "python";
    if (strstr(content, "import ") && strstr(content, "from ")) return "python";
    if (strstr(content, "if __name__ == \"__main__\":")) return "python";

    if (strstr(content, "if __name__ == \"__main__\":")) return "python";

    /* YAML Heuristics */
    if (g_str_has_prefix(content, "%YAML") || g_str_has_prefix(content, "---")) return "yaml";
    /* Look for typical "key: value" or "- item" at start */
    /* Check first line manually */
    {
        const char *s = content;
        while (*s && g_ascii_isspace(*s)) s++;
        
        /* List item */
        if (*s == '-') return "yaml";
        
        /* Key: Value */
        if (g_ascii_isalnum(*s)) {
            const char *k = s;
            while (*k && (g_ascii_isalnum(*k) || *k == '-' || *k == '_')) k++;
            if (*k == ':' && (g_ascii_isspace(k[1]) || k[1] == '\0')) return "yaml";
        }
    }

    return NULL;
}


void
syntax_context_apply_edit(SyntaxContext *ctx, size_t start_line, int line_delta)
{
    /* 1. Shift the state chain to preserve future states for convergence check */
    if (ctx->state_chain->len > start_line) {
        if (line_delta > 0) {
            /* Insertion: Shift data up to make room */
            size_t old_len = ctx->state_chain->len;
            size_t move_count = old_len - start_line;
            g_byte_array_set_size(ctx->state_chain, old_len + line_delta);
            /* Memmove: dest, src, length */
            memmove(ctx->state_chain->data + start_line + line_delta, 
                    ctx->state_chain->data + start_line, 
                    move_count);
            /* Invalidate the inserted gap (optional, set to ROOT) */
            memset(ctx->state_chain->data + start_line, STATE_ROOT, line_delta);
        } else if (line_delta < 0) {
            /* Deletion: Shift data down */
            size_t start_src = start_line + (-line_delta);
            if (start_src < ctx->state_chain->len) {
                size_t move_count = ctx->state_chain->len - start_src;
                memmove(ctx->state_chain->data + start_line,
                        ctx->state_chain->data + start_src,
                        move_count);
                g_byte_array_set_size(ctx->state_chain, ctx->state_chain->len + line_delta);
            } else {
                /* Deleting everything until end or beyond */
                g_byte_array_set_size(ctx->state_chain, start_line);
            }
        }
    }
    /* Reset valid_up_to to the start of edit. We must re-scan from here. */
    if (ctx->valid_up_to > start_line) {
        ctx->valid_up_to = start_line;
    }

    /* 2. Shift the cache (keep existing logic) */
    if (ctx->line_cache) {
        if (line_delta > 0) {
            /* Insertion: insert NULL entries */
            /* Guard: Only insert if start_line is within the currently cached range.
               If we are inserting beyond the cache, we don't need to shift anything. */
            if (start_line <= ctx->line_cache->len) {
                for (int i = 0; i < line_delta; i++) {
                    g_ptr_array_insert(ctx->line_cache, start_line, NULL);
                }
            }
        } else if (line_delta < 0) {
            /* Deletion: remove entries */
            int to_remove = -line_delta;
            if (start_line < ctx->line_cache->len) {
                int count = MIN(to_remove, (int)(ctx->line_cache->len - start_line));
                g_ptr_array_remove_range(ctx->line_cache, start_line, count);
            }
        }
        
        /* 3. Invalidate current line entry if it's within bounds. */
        if (start_line < ctx->line_cache->len) {
             SyntaxCacheEntry *old = g_ptr_array_index(ctx->line_cache, start_line);
             if (old) {
                 /* Replace with NULL to trigger re-highlight */
                 g_ptr_array_remove_index(ctx->line_cache, start_line);
                 g_ptr_array_insert(ctx->line_cache, start_line, NULL);
             }
        }
    }
}

void
syntax_context_invalidate_all(SyntaxContext *ctx)
{
    g_byte_array_set_size(ctx->state_chain, 0);
    ctx->valid_up_to = 0;
    if (ctx->line_cache) {
        g_ptr_array_set_size(ctx->line_cache, 0);
    }
}

void
syntax_context_invalidate_cache(SyntaxContext *ctx)
{
    /* Only clear the attribute cache, preserving state chain.
       Useful for theme changes where syntax logic is unchanged but colors change. */
    if (ctx->line_cache) {
        g_ptr_array_set_size(ctx->line_cache, 0);
    }
}

/* Helper to add attribute - selects color based on theme */
void
add_attr(PangoAttrList *attrs, int start, int end, const PangoColor *color_ref)
{
    if (start >= end) return;
    if (!attrs) return; /* Skip attribute creation if only computing state */
    
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
        else if (color_ref == &l_operator) effective_color = &d_operator;
        else if (color_ref == &l_punctuation) effective_color = &d_punctuation;
        else if (color_ref == &l_function) effective_color = &d_function;
        else if (color_ref == &l_variable) effective_color = &d_variable;
        else if (color_ref == &l_variable_c) effective_color = &d_variable_c;
        else if (color_ref == &l_constant) effective_color = &d_constant;
    } else {
        /* If pointers are to dark (default statics), map to light */
        if (color_ref == &d_keyword) effective_color = &l_keyword;
        else if (color_ref == &d_type) effective_color = &l_type;
        else if (color_ref == &d_string) effective_color = &l_string;
        else if (color_ref == &d_comment) effective_color = &l_comment;
        else if (color_ref == &d_preproc) effective_color = &l_preproc;
        else if (color_ref == &d_number) effective_color = &l_number;
        else if (color_ref == &d_operator) effective_color = &l_operator;
        else if (color_ref == &d_punctuation) effective_color = &l_punctuation;
        else if (color_ref == &d_function) effective_color = &l_function;
        else if (color_ref == &d_variable) effective_color = &l_variable;
        else if (color_ref == &d_variable_c) effective_color = &l_variable_c;
        else if (color_ref == &d_constant) effective_color = &l_constant;
        else if (color_ref == &d_logical) effective_color = &l_logical;
        else if (color_ref == &d_param) effective_color = &l_param;
        else if (color_ref == &d_tag) effective_color = &l_tag;
        else if (color_ref == &d_attribute) effective_color = &l_attribute;
        else if (color_ref == &d_property) effective_color = &l_property;
        
        /* Refine: Constants/Numbers are Orange in One Light, not Purple */
        if (color_ref == &d_number || color_ref == &d_type) effective_color = &l_number; 
    }
    
    PangoAttribute *attr = pango_attr_foreground_new(effective_color->red, effective_color->green, effective_color->blue);
    attr->start_index = start;
    attr->end_index = end;
    pango_attr_list_insert(attrs, attr);
}

/* Helper to get next state */
SyntaxState
get_line_start_state(SyntaxContext *ctx, size_t line_index)
{
    if (line_index == 0) return STATE_ROOT;
    if (line_index - 1 < ctx->state_chain->len) {
        return (SyntaxState)ctx->state_chain->data[line_index - 1];
    }
    return STATE_ROOT; /* Default if unknown, though usually we process in order */
}

void
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

gboolean
is_all_caps(const char *s, size_t len)
{
    if (len == 0 || !g_ascii_isupper(s[0])) return FALSE;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '_') continue;
        if (g_ascii_isdigit(s[i])) continue;
        if (!g_ascii_isupper(s[i])) return FALSE;
    }
    return TRUE;
}

gboolean 
is_word_in_list(const char *word, size_t len, const char **list) 
{
    /* Simple linear scan for now */
    for (int i = 0; list[i]; i++) {
        if (strncmp(word, list[i], len) == 0 && list[i][len] == '\0') return TRUE;
    }
    return FALSE;
}

size_t
syntax_get_processed_line_count(SyntaxContext *ctx)
{
    return ctx->valid_up_to;
}

PangoAttrList *
syntax_process_line(SyntaxContext *ctx, size_t line_index, const char *text, gboolean compute_attributes)
{
    if (ctx->lang == LANG_NONE) return compute_attributes ? pango_attr_list_new() : NULL;
    
    SyntaxState start_state = get_line_start_state(ctx, line_index);
    guint content_hash = g_str_hash(text);
    
    /* Cache lookup - Only if we need attributes */
    if (compute_attributes && ctx->line_cache && line_index < ctx->line_cache->len) {
        SyntaxCacheEntry *cached = g_ptr_array_index(ctx->line_cache, line_index);
        if (cached && cached->content_hash == content_hash && cached->start_state == start_state) {
            /* Cache hit - return a copy of the cached attrs */
            return pango_attr_list_ref(cached->attrs);
        }
    }
    
    PangoAttrList *attrs = compute_attributes ? pango_attr_list_new() : NULL;
    size_t len = strlen(text);
    
    /* Dispatch to language handlers */
    switch (ctx->lang) {
        case LANG_C:
            syntax_highlight_c(ctx, attrs, text, len, start_state, line_index);
            break;
        case LANG_PYTHON:
            syntax_highlight_python(ctx, attrs, text, len, start_state, line_index);
            break;
        case LANG_BASH:
            syntax_highlight_bash(ctx, attrs, text, len, start_state, line_index);
            break;
        case LANG_JAVASCRIPT:
            syntax_highlight_js(ctx, attrs, text, len, start_state, line_index);
            break;
        case LANG_JSON:
            syntax_highlight_json(ctx, attrs, text, len, start_state, line_index);
            break;
        case LANG_YAML:
            syntax_highlight_yaml(ctx, attrs, text, len, start_state, line_index);
            break;
        case LANG_XML:
            syntax_highlight_xml(ctx, attrs, text, len, start_state, line_index);
            break;
        case LANG_DESKTOP:
            syntax_highlight_desktop(ctx, attrs, text, len, start_state, line_index);
            break;
        case LANG_RUST:
            syntax_highlight_rust(ctx, attrs, text, len, start_state, line_index);
            break;
        default:
             /* Just save state if we don't have a handler (shouldn't happen if lang!=NONE) */
             set_line_end_state(ctx, line_index, start_state);
             break;
    }

    /* Store in cache */
    if (ctx->line_cache && attrs) {
        SyntaxCacheEntry *entry = g_new(SyntaxCacheEntry, 1);
        entry->content_hash = content_hash;
        entry->start_state = start_state;
        entry->attrs = pango_attr_list_ref(attrs);
        
        if (line_index >= ctx->line_cache->len) {
            g_ptr_array_set_size(ctx->line_cache, line_index + 1);
        } else {
            /* Free old entry if any */
            SyntaxCacheEntry *old = g_ptr_array_index(ctx->line_cache, line_index);
            if (old) syntax_cache_entry_free(old);
        }
        ctx->line_cache->pdata[line_index] = entry;
    }
    
    return attrs;
}

PangoAttrList *
syntax_highlight_line(SyntaxContext *ctx, size_t line_index, const char *text)
{
    return syntax_process_line(ctx, line_index, text, TRUE);
}
