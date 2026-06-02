#include "syntax-internal.h"
#include <string.h>

/* --- Theme-Driven Color System --- */

static void
syntax_cache_entry_free(gpointer data)
{
    if (!data) return;
    SyntaxCacheEntry *entry = data;
    if (entry->attrs) pango_attr_list_unref(entry->attrs);
    g_free(entry);
}

/* Stub: kept for API compatibility, but theme state now lives in theme-manager */
void
syntax_set_theme_mode(gboolean is_dark)
{
    (void)is_dark; /* Theme mode is now managed by theme_manager */
}

gboolean
syntax_get_theme_mode(void)
{
    const ViteTheme *theme = theme_manager_get_current();
    return theme ? theme->is_dark : TRUE;
}

/* New, clean color attribute function: looks up color from current theme with language override check */
void
add_color_attr(SyntaxContext *ctx, PangoAttrList *attrs, int start, int end, ViteColorSlot slot)
{
    if (start >= end) return;
    if (!attrs) return;
    if (slot < 0 || slot >= COLOR_SLOT_COUNT) return;

    const ViteTheme *theme = theme_manager_get_current();
    if (!theme) return;

    const PangoColor *c = NULL;
    guint8 style_mask = 0;

    if (ctx) {
        SyntaxLanguage lang = syntax_context_get_language(ctx);
        if (lang > 0 && lang < VITE_LANG_COUNT) {
            if (theme->has_lang_syntax[lang][slot]) {
                c = &theme->syntax_lang[lang][slot];
            }
            if (theme->has_lang_style_set[lang][slot]) {
                style_mask = theme->syntax_lang_style[lang][slot];
            }
        }
    }
    
    if (!c) {
        c = &theme->syntax[slot];
    }
    
    /* Fallback to non-lang style if lang-specific one wasn't explicitly set */
    if (ctx) {
        SyntaxLanguage lang = syntax_context_get_language(ctx);
        if (lang > 0 && lang < VITE_LANG_COUNT && !theme->has_lang_style_set[lang][slot]) {
            style_mask = theme->syntax_style[slot];
        }
    } else {
        style_mask = theme->syntax_style[slot];
    }

    if (c) {
        PangoAttribute *attr = pango_attr_foreground_new(c->red, c->green, c->blue);
        attr->start_index = start;
        attr->end_index = end;
        pango_attr_list_insert(attrs, attr);
    }

    if (style_mask & VITE_FONT_STYLE_BOLD) {
        PangoAttribute *attr = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
        attr->start_index = start;
        attr->end_index = end;
        pango_attr_list_insert(attrs, attr);
    }

    if (style_mask & VITE_FONT_STYLE_ITALIC) {
        PangoAttribute *attr = pango_attr_style_new(PANGO_STYLE_ITALIC);
        attr->start_index = start;
        attr->end_index = end;
        pango_attr_list_insert(attrs, attr);
    }

    if (style_mask & VITE_FONT_STYLE_UNDERLINE) {
        PangoAttribute *attr = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
        attr->start_index = start;
        attr->end_index = end;
        pango_attr_list_insert(attrs, attr);
    }
}

SyntaxContext *
syntax_context_new(void)
{
    SyntaxContext *ctx = g_new0(SyntaxContext, 1);
    ctx->ref_count = 1;
    ctx->lang = LANG_NONE;
    ctx->state_chain = g_byte_array_new();
    ctx->range_overrides = g_array_new(FALSE, FALSE, sizeof(SyntaxRangeOverride));
    ctx->valid_up_to = 0;
    
    /* Compile Regexes */
    
    /* Bash */
    ctx->sh_keywords = g_regex_new("\\b(if|then|else|elif|fi|case|esac|for|select|while|until|do|done|in|function|time|coproc|declare|typeset|local|readonly|export|unset|set|shopt|trap|source|alias|unalias|break|continue|return|exit|eval|exec)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->sh_builtins = g_regex_new("\\b(echo|printf|cd|pwd|ls|cp|mv|rm|mkdir|rmdir|touch|cat|grep|sed|awk|find|chmod|chown|kill|ps|jobs|bg|fg|history|read|wait|sleep|true|false|make|install|flatpak|git|node|npm|pip|python|python3|gcc|g\\+\\+|clang|docker|systemctl|journalctl)\\b", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->sh_variable = g_regex_new("(\\$[a-zA-Z_][a-zA-Z0-9_]*|\\$\\{[^}]+\\}|\\$[0-9*@#?!$-])", G_REGEX_OPTIMIZE, 0, NULL);
    
    /* XML */
    ctx->xml_tag_open = g_regex_new("</?([- \\w.:]+)", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->xml_tag_close = g_regex_new("/?>", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->xml_attr = g_regex_new("\\s([-\\w.:]+)=", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->xml_comment_start = g_regex_new("<!--", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->xml_comment_end = g_regex_new("-->", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->xml_cdata_start = g_regex_new("<!\\[CDATA\\[", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->xml_cdata_end = g_regex_new("\\]\\]>", G_REGEX_OPTIMIZE, 0, NULL);
    ctx->xml_string_dq = g_regex_new("\"[^\"]*\"", G_REGEX_OPTIMIZE, 0, NULL);

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
    if (ctx->range_overrides) g_array_unref(ctx->range_overrides);
    
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
    if (ctx->xml_string_dq) g_regex_unref(ctx->xml_string_dq);

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

static SyntaxLanguage
syntax_language_from_string(const char *lang_name)
{
    if (!lang_name) return LANG_NONE;
    if (strcmp(lang_name, "c") == 0 || strcmp(lang_name, "cpp") == 0 || strcmp(lang_name, "h") == 0) return LANG_C;
    if (strcmp(lang_name, "python") == 0 || strcmp(lang_name, "py") == 0) return LANG_PYTHON;
    if (strcmp(lang_name, "bash") == 0 || strcmp(lang_name, "sh") == 0 || strcmp(lang_name, "zsh") == 0) return LANG_BASH;
    if (strcmp(lang_name, "javascript") == 0 || strcmp(lang_name, "js") == 0 || strcmp(lang_name, "ts") == 0) return LANG_JAVASCRIPT;
    if (strcmp(lang_name, "json") == 0) return LANG_JSON;
    if (strcmp(lang_name, "yaml") == 0 || strcmp(lang_name, "yml") == 0) return LANG_YAML;
    if (strcmp(lang_name, "xml") == 0 || strcmp(lang_name, "html") == 0 || strcmp(lang_name, "xsl") == 0 || strcmp(lang_name, "svg") == 0) return LANG_XML;
    if (strcmp(lang_name, "desktop") == 0) return LANG_DESKTOP;
    if (strcmp(lang_name, "rust") == 0 || strcmp(lang_name, "rs") == 0 || strcmp(lang_name, "rst") == 0) return LANG_RUST;
    if (strcmp(lang_name, "markdown") == 0 || strcmp(lang_name, "md") == 0 || strcmp(lang_name, "mkd") == 0) return LANG_MARKDOWN;
    return LANG_NONE;
}

void
syntax_context_set_language(SyntaxContext *ctx, const char *lang_name)
{
    ctx->lang = syntax_language_from_string(lang_name);
    
    /* Clear states, cache, and byte overrides */
    g_byte_array_set_size(ctx->state_chain, 0);
    if (ctx->range_overrides) g_array_set_size(ctx->range_overrides, 0);
    ctx->valid_up_to = 0;
    if (ctx->line_cache) g_ptr_array_set_size(ctx->line_cache, 0);
}

void
syntax_context_set_language_for_byte_range(SyntaxContext *ctx, size_t start_off, size_t end_off, const char *lang_name)
{
    if (!ctx || start_off >= end_off) return;
    
    SyntaxLanguage new_lang = syntax_language_from_string(lang_name);
    
    SyntaxRangeOverride override = { start_off, end_off, new_lang };
    g_array_append_val(ctx->range_overrides, override);
    
    /* We invalidate everything so it forces a redraw and re-parse. 
       We could be smarter but byte ranges can span anywhere. */
    ctx->valid_up_to = 0;
    if (ctx->line_cache) {
        g_ptr_array_set_size(ctx->line_cache, 0);
    }
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
        case LANG_MARKDOWN: return "Markdown";
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
        const char *newline = strchr(content, '\n');
        size_t line_len = newline ? (size_t)(newline - content) : strlen(content);
        char *first_line = g_strndup(content, line_len);
        
        const char *ret = NULL;
        if (strstr(first_line, "bash") || strstr(first_line, "sh")) ret = "bash";
        else if (strstr(first_line, "python")) ret = "python";
        else if (strstr(first_line, "node")) ret = "javascript";
        else if (strstr(first_line, "perl")) ret = NULL; /* Not supported yet */
        
        g_free(first_line);
        if (ret) return ret;
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
    if (strstr(content, "def ") && strstr(content, ":")) return "python";
    if (strstr(content, "import ") && strstr(content, "from ")) return "python";
    if (strstr(content, "if __name__ == \"__main__\":")) return "python";

    if (strstr(content, "if __name__ == \"__main__\":")) return "python";

    /* YAML Heuristics */
    if (g_str_has_prefix(content, "%YAML") || g_str_has_prefix(content, "---")) return "yaml";
    {
        const char *s = content;
        while (*s && g_ascii_isspace(*s)) s++;
        
        if (*s == '-') return "yaml";
        
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
    if (ctx->state_chain->len > start_line) {
        if (line_delta > 0) {
            size_t old_len = ctx->state_chain->len;
            size_t move_count = old_len - start_line;
            g_byte_array_set_size(ctx->state_chain, old_len + line_delta);
            memmove(ctx->state_chain->data + start_line + line_delta, 
                    ctx->state_chain->data + start_line, 
                    move_count);
            memset(ctx->state_chain->data + start_line, STATE_ROOT, line_delta);
        } else if (line_delta < 0) {
            size_t start_src = start_line + (-line_delta);
            if (start_src < ctx->state_chain->len) {
                size_t move_count = ctx->state_chain->len - start_src;
                memmove(ctx->state_chain->data + start_line,
                        ctx->state_chain->data + start_src,
                        move_count);
                g_byte_array_set_size(ctx->state_chain, ctx->state_chain->len + line_delta);
            } else {
                g_byte_array_set_size(ctx->state_chain, start_line);
            }
        }
    }


    if (ctx->valid_up_to > start_line) {
        ctx->valid_up_to = start_line;
    }

    if (ctx->line_cache) {
        if (line_delta > 0) {
            if (start_line <= ctx->line_cache->len) {
                for (int i = 0; i < line_delta; i++) {
                    g_ptr_array_insert(ctx->line_cache, start_line, NULL);
                }
            }
        } else if (line_delta < 0) {
            int to_remove = -line_delta;
            if (start_line < ctx->line_cache->len) {
                int count = MIN(to_remove, (int)(ctx->line_cache->len - start_line));
                g_ptr_array_remove_range(ctx->line_cache, start_line, count);
            }
        }
        
        if (start_line < ctx->line_cache->len) {
             SyntaxCacheEntry *old = g_ptr_array_index(ctx->line_cache, start_line);
             if (old) {
                 g_ptr_array_remove_index(ctx->line_cache, start_line);
                 g_ptr_array_insert(ctx->line_cache, start_line, NULL);
             }
        }
    }
}
void
syntax_context_apply_byte_edit(SyntaxContext *ctx, size_t offset, int64_t delta_len)
{
    if (!ctx || !ctx->range_overrides) return;
    
    for (guint i = 0; i < ctx->range_overrides->len; i++) {
        SyntaxRangeOverride *range = &g_array_index(ctx->range_overrides, SyntaxRangeOverride, i);
        
        if (delta_len > 0) { // insertion
            if (offset <= range->start_off) {
                range->start_off += delta_len;
                range->end_off += delta_len;
            } else if (offset < range->end_off) {
                range->end_off += delta_len;
            }
        } else if (delta_len < 0) { // deletion
            size_t del_len = -delta_len;
            if (offset + del_len <= range->start_off) {
                // deletion is entirely before the range
                range->start_off -= del_len;
                range->end_off -= del_len;
            } else if (offset <= range->start_off && offset + del_len >= range->end_off) {
                // range is entirely deleted
                g_array_remove_index(ctx->range_overrides, i);
                i--; // adjust index
                continue;
            } else if (offset <= range->start_off) {
                // overlaps start of range
                range->start_off = offset;
                range->end_off -= del_len;
            } else if (offset < range->end_off) {
                // overlaps end of range or inside range
                if (offset + del_len >= range->end_off) {
                    range->end_off = offset;
                } else {
                    range->end_off -= del_len;
                }
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
    if (ctx->line_cache) {
        g_ptr_array_set_size(ctx->line_cache, 0);
    }
}

/* Helper to get next state */
SyntaxState
get_line_start_state(SyntaxContext *ctx, size_t line_index)
{
    if (line_index == 0) return STATE_ROOT;
    if (line_index - 1 < ctx->state_chain->len) {
        return (SyntaxState)(ctx->state_chain->data[line_index - 1] & 0x1F);
    }
    return STATE_ROOT; /* Default if unknown, though usually we process in order */
}

int
get_line_start_bracket_depth(SyntaxContext *ctx, size_t line_index)
{
    if (line_index == 0) return 0;
    if (line_index - 1 < ctx->state_chain->len) {
        return (ctx->state_chain->data[line_index - 1] >> 5) & 0x07;
    }
    return 0;
}

void
set_line_end_state(SyntaxContext *ctx, size_t line_index, SyntaxState state)
{
    set_line_end_state_with_depth(ctx, line_index, state, 0);
}

void
set_line_end_state_with_depth(SyntaxContext *ctx, size_t line_index, SyntaxState state, int bracket_depth)
{
    if (line_index >= ctx->state_chain->len) {
        /* fill gaps with ROOT if any (shouldn't happen with sequential access) */
        size_t old_len = ctx->state_chain->len;
        g_byte_array_set_size(ctx->state_chain, line_index + 1);
        for (size_t i = old_len; i < line_index; i++) {
            ctx->state_chain->data[i] = STATE_ROOT;
        }
    }
    
    /* Clamp depth to 0-7 to fit in 3 bits */
    if (bracket_depth > 7) bracket_depth = 7;
    if (bracket_depth < 0) bracket_depth = 0;
    
    ctx->state_chain->data[line_index] = (guint8)((state & 0x1F) | (bracket_depth << 5));
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
syntax_process_line_len(SyntaxContext *ctx, size_t line_index, size_t line_start_off, const char *text, size_t len, gboolean compute_attributes)
{
    SyntaxLanguage current_lang = ctx->lang;
    if (current_lang == LANG_NONE && (!ctx->range_overrides || ctx->range_overrides->len == 0)) {
        return compute_attributes ? pango_attr_list_new() : NULL;
    }
    
    SyntaxState start_state = get_line_start_state(ctx, line_index);
    guint content_hash = 0;
    
    if (compute_attributes) {
        content_hash = g_str_hash(text); 
        
        if (ctx->line_cache && line_index < ctx->line_cache->len) {
            SyntaxCacheEntry *cached = g_ptr_array_index(ctx->line_cache, line_index);
            if (cached && cached->content_hash == content_hash && cached->start_state == start_state) {
                return pango_attr_list_ref(cached->attrs);
            }
        }
    }
    
    PangoAttrList *attrs = compute_attributes ? pango_attr_list_new() : NULL;

    if (len > 4096) {
        return NULL;
    }
    
    /* Default processing */
    if (current_lang != LANG_NONE) {
        switch (current_lang) {
            case LANG_C: syntax_highlight_c(ctx, attrs, text, len, start_state, line_index); break;
            case LANG_PYTHON: syntax_highlight_python(ctx, attrs, text, len, start_state, line_index); break;
            case LANG_BASH: syntax_highlight_bash(ctx, attrs, text, len, start_state, line_index); break;
            case LANG_JAVASCRIPT: syntax_highlight_js(ctx, attrs, text, len, start_state, line_index); break;
            case LANG_JSON: syntax_highlight_json(ctx, attrs, text, len, start_state, line_index); break;
            case LANG_YAML: syntax_highlight_yaml(ctx, attrs, text, len, start_state, line_index); break;
            case LANG_XML: syntax_highlight_xml(ctx, attrs, text, len, start_state, line_index); break;
            case LANG_DESKTOP: syntax_highlight_desktop(ctx, attrs, text, len, start_state, line_index); break;
            case LANG_RUST: syntax_highlight_rust(ctx, attrs, text, len, start_state, line_index); break;
            case LANG_MARKDOWN: syntax_highlight_markdown(ctx, attrs, text, len, start_state, line_index); break;
            default: set_line_end_state(ctx, line_index, start_state); break;
        }
    } else {
        set_line_end_state(ctx, line_index, start_state);
    }
    
    /* Apply regional overrides */
    if (compute_attributes && ctx->range_overrides) {
        size_t line_end_off = line_start_off + len;
        
        for (guint i = 0; i < ctx->range_overrides->len; i++) {
            SyntaxRangeOverride *range = &g_array_index(ctx->range_overrides, SyntaxRangeOverride, i);
            
            /* Check intersection */
            if (range->start_off < line_end_off && range->end_off > line_start_off) {
                size_t intersect_start = MAX(range->start_off, line_start_off) - line_start_off;
                size_t intersect_end = MIN(range->end_off, line_end_off) - line_start_off;
                size_t intersect_len = intersect_end - intersect_start;
                
                if (intersect_len == 0) continue;
                
                /* Substring to parse */
                char *sub_text = g_strndup(text + intersect_start, intersect_len);
                PangoAttrList *sub_attrs = pango_attr_list_new();
                
                /* Pass ROOT state for substring parsing */
                SyntaxState sub_state = STATE_ROOT;
                
                switch (range->language) {
                    case LANG_C: syntax_highlight_c(ctx, sub_attrs, sub_text, intersect_len, sub_state, 0); break;
                    case LANG_PYTHON: syntax_highlight_python(ctx, sub_attrs, sub_text, intersect_len, sub_state, 0); break;
                    case LANG_BASH: syntax_highlight_bash(ctx, sub_attrs, sub_text, intersect_len, sub_state, 0); break;
                    case LANG_JAVASCRIPT: syntax_highlight_js(ctx, sub_attrs, sub_text, intersect_len, sub_state, 0); break;
                    case LANG_JSON: syntax_highlight_json(ctx, sub_attrs, sub_text, intersect_len, sub_state, 0); break;
                    case LANG_YAML: syntax_highlight_yaml(ctx, sub_attrs, sub_text, intersect_len, sub_state, 0); break;
                    case LANG_XML: syntax_highlight_xml(ctx, sub_attrs, sub_text, intersect_len, sub_state, 0); break;
                    case LANG_DESKTOP: syntax_highlight_desktop(ctx, sub_attrs, sub_text, intersect_len, sub_state, 0); break;
                    case LANG_RUST: syntax_highlight_rust(ctx, sub_attrs, sub_text, intersect_len, sub_state, 0); break;
                    case LANG_MARKDOWN: syntax_highlight_markdown(ctx, sub_attrs, sub_text, intersect_len, sub_state, 0); break;
                    case LANG_NONE: {
                        const ViteTheme *theme = theme_manager_get_current();
                        if (theme) {
                            PangoAttribute *attr_fg = pango_attr_foreground_new(
                                theme->editor_fg.red * 65535,
                                theme->editor_fg.green * 65535,
                                theme->editor_fg.blue * 65535);
                            attr_fg->start_index = 0;
                            attr_fg->end_index = intersect_len;
                            pango_attr_list_insert(sub_attrs, attr_fg);
                            
                            PangoAttribute *attr_w = pango_attr_weight_new(PANGO_WEIGHT_NORMAL);
                            attr_w->start_index = 0; attr_w->end_index = intersect_len;
                            pango_attr_list_insert(sub_attrs, attr_w);
                            
                            PangoAttribute *attr_s = pango_attr_style_new(PANGO_STYLE_NORMAL);
                            attr_s->start_index = 0; attr_s->end_index = intersect_len;
                            pango_attr_list_insert(sub_attrs, attr_s);
                            
                            PangoAttribute *attr_u = pango_attr_underline_new(PANGO_UNDERLINE_NONE);
                            attr_u->start_index = 0; attr_u->end_index = intersect_len;
                            pango_attr_list_insert(sub_attrs, attr_u);
                        }
                        break;
                    }
                    default: break;
                }
                
                /* Splice sub_attrs into attrs shifted by intersect_start. 
                   Pass 0 as length since we are overlaying, not inserting new text. */
                pango_attr_list_splice(attrs, sub_attrs, intersect_start, 0);
                pango_attr_list_unref(sub_attrs);
                g_free(sub_text);
            }
        }
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
syntax_process_line(SyntaxContext *ctx, size_t line_index, size_t line_start_off, const char *text, gboolean compute_attributes)
{
    return syntax_process_line_len(ctx, line_index, line_start_off, text, strlen(text), compute_attributes);
}

PangoAttrList *
syntax_highlight_line(SyntaxContext *ctx, size_t line_index, size_t line_start_off, const char *text)
{
    return syntax_process_line(ctx, line_index, line_start_off, text, TRUE);
}
