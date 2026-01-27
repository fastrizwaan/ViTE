#include "syntax-internal.h"
#include <string.h>

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


void 
syntax_highlight_python(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index)
{
    size_t cur = 0;
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
                else {
                    /* Variable - use d_variable (Red) instead of C variable color */
                    add_attr(attrs, start_pos, cur, &d_variable);
                }
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
            
            /* Operators / Punctuation */
            if (strchr("()[]{}:;.,", text[cur])) {
                 add_attr(attrs, cur, cur+1, &d_punctuation);
                 cur++;
                 continue;
            }
            if (strchr("=+-*/%&|^<>!~", text[cur])) {
                 add_attr(attrs, cur, cur+1, &d_keyword);
                 cur++;
                 continue;
            }

            /* Skip other chars */
            cur++;
        } 
        else {
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
    
    set_line_end_state(ctx, line_index, state);
}
