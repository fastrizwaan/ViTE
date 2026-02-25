#include "syntax-internal.h"
#include <string.h>

/* JS Keywords */
static const char *js_keywords[] = {
    "async", "await", "break", "case", "catch", "class", "const", "continue", "debugger",
    "default", "delete", "do", "else", "export", "extends", "finally", "for", "from",
    "function", "get", "if", "import", "in", "instanceof", "let", "new", "of", "return",
    "set", "static", "super", "switch", "this", "throw", "try", "typeof", "var", "void",
    "while", "with", "yield", "true", "false", "null", "undefined", "NaN", NULL
};

static const char *js_builtins[] = {
    "Array", "Boolean", "Date", "Error", "Function", "JSON", "Map", "Math", "Number",
    "Object", "Promise", "Proxy", "RegExp", "Set", "String", "Symbol", "WeakMap",
    "WeakSet", "console", "document", "window", "global", "module", "exports", "require",
    "process", NULL
};


void 
syntax_highlight_js(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index)
{
    size_t cur = 0;
    gboolean prev_is_value = FALSE; /* Heuristic for Regex vs Div */
    gboolean expect_func = FALSE;

    while (cur < len) {
        /* State: Comments & Strings */
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
            add_color_attr(ctx, attrs, start_pos, cur, COLOR_COMMENT);
            continue;
        }
        if (state == STATE_IN_TRIPLE_DQ_STRING) { /* Reused for Template ` */
            size_t start_pos = cur;
            while (cur < len) {
                if (text[cur] == '`' && (cur == 0 || text[cur-1] != '\\')) {
                    cur++;
                    state = STATE_ROOT;
                    prev_is_value = TRUE;
                    break;
                }
                cur++;
            }
            add_color_attr(ctx, attrs, start_pos, cur, COLOR_STRING);
            continue;
        }

        if (state == STATE_ROOT) {
            /* Comments */
            if (text[cur] == '/' && cur+1 < len && text[cur+1] == '/') {
                add_color_attr(ctx, attrs, cur, len, COLOR_COMMENT);
                cur = len;
                break;
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
                add_color_attr(ctx, attrs, start_pos, cur, COLOR_COMMENT);
                continue; /* Don't set prev_is_value for comment? */
            }

            /* Regex vs Division */
            if (text[cur] == '/') {
                if (prev_is_value) {
                    /* Division */
                    /* Just an operator (Operator/Keyword color) */
                    add_color_attr(ctx, attrs, cur, cur+1, COLOR_KEYWORD);
                    cur++;
                    prev_is_value = FALSE; /* Op consumes value */
                } else {
                    /* Regex Literal */
                    size_t start_pos = cur;
                    cur++; /* Skip opening / */
                    while (cur < len) {
                        if (text[cur] == '/' && text[cur-1] != '\\') {
                            cur++;
                            /* Skip flags */
                            while (cur < len && g_ascii_isalpha(text[cur])) cur++;
                            prev_is_value = TRUE; /* Regex literal is a value */
                            break;
                        }
                        cur++;
                    }
                    add_color_attr(ctx, attrs, start_pos, cur, COLOR_STRING); /* Regex colored as string */
                }
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
                add_color_attr(ctx, attrs, start_pos, cur, COLOR_STRING);
                 prev_is_value = TRUE;
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
                add_color_attr(ctx, attrs, start_pos, cur, COLOR_STRING);
                 prev_is_value = TRUE;
                continue;
            }
            /* Template Literal */
            if (text[cur] == '`') {
                state = STATE_IN_TRIPLE_DQ_STRING; /* reused for template */
                size_t start_pos = cur;
                cur++;
                while (cur < len) {
                    if (text[cur] == '`' && text[cur-1] != '\\') {
                        cur++;
                        state = STATE_ROOT;
                        prev_is_value = TRUE;
                        break;
                    }
                    cur++;
                }
                add_color_attr(ctx, attrs, start_pos, cur, COLOR_STRING);
                continue;
            }

            /* Numbers */
            if (g_ascii_isdigit(text[cur])) {
                size_t start_pos = cur;
                while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '.')) {
                    cur++;
                }
                add_color_attr(ctx, attrs, start_pos, cur, COLOR_NUMBER);
                prev_is_value = TRUE;
                continue;
            }

            /* Identifiers */
            if (g_ascii_isalpha(text[cur]) || text[cur] == '_' || text[cur] == '$') {
                size_t start_pos = cur;
                while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_' || text[cur] == '$')) {
                    cur++;
                }
                size_t word_len = cur - start_pos;
                const char *word_start = text + start_pos;
                
                if (expect_func) {
                    add_color_attr(ctx, attrs, start_pos, cur, COLOR_FUNCTION);
                    expect_func = FALSE;
                    prev_is_value = TRUE; /* Function Name is a value-ish */
                } 
                else if (is_word_in_list(word_start, word_len, js_keywords)) {
                    add_color_attr(ctx, attrs, start_pos, cur, COLOR_KEYWORD);
                    prev_is_value = FALSE; /* Keywords usually start statements/clauses */
                     /* Special case: 'this', 'true', 'false', 'null', 'undefined', 'NaN' are values */
                    if (strncmp(word_start, "this", word_len) == 0 ||
                        strncmp(word_start, "true", word_len) == 0 ||
                        strncmp(word_start, "false", word_len) == 0 ||
                        strncmp(word_start, "null", word_len) == 0 ||
                        strncmp(word_start, "undefined", word_len) == 0 ||
                        strncmp(word_start, "NaN", word_len) == 0) {
                        prev_is_value = TRUE;
                    }
                    else if (strncmp(word_start, "function", word_len) == 0 && word_len == 8) {
                         expect_func = TRUE;
                    }
                } 
                else if (is_word_in_list(word_start, word_len, js_builtins)) {
                    add_color_attr(ctx, attrs, start_pos, cur, COLOR_BUILTIN);
                    prev_is_value = TRUE;
                } 
                else {
                    /* Check for function call */
                    size_t peek = cur;
                    while (peek < len && g_ascii_isspace(text[peek])) peek++;
                    if (peek < len && text[peek] == '(') add_color_attr(ctx, attrs, start_pos, cur, COLOR_FUNCTION);
                    
                    prev_is_value = TRUE; /* Identifier is a value */
                }
                continue;
            }
            
            /* Punctuation */
            if (strchr("()[]{};,", text[cur])) {
                if (text[cur] == ')' || text[cur] == ']') prev_is_value = TRUE;
                else prev_is_value = FALSE;
                add_color_attr(ctx, attrs, cur, cur+1, COLOR_PUNCTUATION);
                cur++;
                continue;
            }
            /* Operators */
            if (strchr("=+-*&|!<>?:", text[cur])) {
                 prev_is_value = FALSE;
                 add_color_attr(ctx, attrs, cur, cur+1, COLOR_KEYWORD);
                 cur++;
                 continue;
            }
            
            /* Whitespace */
             if (g_ascii_isspace(text[cur])) {
                cur++;
                continue;
            }

            cur++;
        }
    }
    
    set_line_end_state(ctx, line_index, state);
}

void 
syntax_highlight_json(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index)
{
    size_t cur = 0;
    while (cur < len) {
        /* Whitespace */
        if (g_ascii_isspace(text[cur])) {
            cur++;
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
            
            /* Check if Key (followed by :) */
            size_t peek = cur;
            while (peek < len && g_ascii_isspace(text[peek])) peek++;
            if (peek < len && text[peek] == ':') {
                add_color_attr(ctx, attrs, start_pos, cur, COLOR_VARIABLE); /* Key -> Red */
            } else {
                add_color_attr(ctx, attrs, start_pos, cur, COLOR_STRING); /* Value -> Green */
            }
            continue;
        }
        
        /* Numbers */
        if (g_ascii_isdigit(text[cur]) || text[cur] == '-') {
            size_t start_pos = cur;
            cur++;
            while (cur < len && (g_ascii_isdigit(text[cur]) || text[cur] == '.' || text[cur] == 'e' || text[cur] == 'E' || text[cur] == '+' || text[cur] == '-')) {
                cur++;
            }
            add_color_attr(ctx, attrs, start_pos, cur, COLOR_NUMBER);
            continue;
        }
        
        /* Keywords (true, false, null) */
        if (g_ascii_isalpha(text[cur])) {
            size_t start_pos = cur;
            while (cur < len && g_ascii_isalpha(text[cur])) cur++;
            size_t len = cur - start_pos;
            if ((len == 4 && strncmp(text+start_pos, "true", 4) == 0) ||
                (len == 5 && strncmp(text+start_pos, "false", 5) == 0) ||
                (len == 4 && strncmp(text+start_pos, "null", 4) == 0)) {
                add_color_attr(ctx, attrs, start_pos, cur, COLOR_NUMBER);
            }
            continue;
        }
        
        if (strchr("{}[],:", text[cur])) {
            add_color_attr(ctx, attrs, cur, cur+1, COLOR_PUNCTUATION);
        }

        cur++;
    }
    set_line_end_state(ctx, line_index, state);
}
