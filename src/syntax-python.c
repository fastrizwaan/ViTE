#include "syntax-internal.h"
#include <string.h>

/* Python Keyword Lists */
static const char *py_keywords[] = {
    "as", "assert", "async", "await", "break", "class", "continue", "def", "del",
    "elif", "else", "except", "finally", "for", "from", "global", "if", "import",
    "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise", "return", "try",
    "while", "with", "yield", "and", NULL
};

static const char *py_bools[] = {
    "False", "None", "True", NULL
};

static const char *py_builtins[] = {
    "abs", "all", "any", "ascii", "bin", "bool", "bytearray", "bytes", "callable",
    "chr", "classmethod", "compile", "complex", "delattr", "dict", "dir", "divmod",
    "enumerate", "eval", "exec", "filter", "float", "format", "frozenset", "getattr",
    "globals", "hasattr", "hash", "help", "hex", "id", "input", "int", "isinstance",
    "issubclass", "iter", "len", "list", "locals", "map", "max", "memoryview", "min",
    "next", "object", "oct", "open", "ord", "pow", "print", "property", "range",
    "repr", "reversed", "round", "set", "setattr", "slice", "sorted", "staticmethod",
    "str", "sum", "super", "tuple", "type", "vars", "zip", "__import__", NULL
};

static const char *py_special_vars[] = {
    "__name__", "__file__", "__doc__", "__package__", "__loader__", "__spec__",
    "__cached__", "__dict__", NULL
};


void 
syntax_highlight_python(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index)
{
    size_t cur = 0;
    gboolean expect_func = FALSE;
    gboolean expect_class = FALSE;
    gboolean in_lambda_def = FALSE;
    int paren_depth = 0;

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
                /* Check if preceding char was '.' (Attribute) */
                gboolean is_attr = (start_pos > 0 && text[start_pos-1] == '.');
                
                while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) {
                    cur++;
                }
                size_t word_len = cur - start_pos;
                const char *word_start = text + start_pos;

                /* Look ahead for '(': Function Call */
                gboolean is_call = FALSE;
                size_t pcall = cur;
                while (pcall < len && g_ascii_isspace(text[pcall])) pcall++;
                if (pcall < len && text[pcall] == '(') is_call = TRUE;

                /* Look ahead for '=' or ':=': Assignment LHS */
                gboolean is_assignment = FALSE;
                size_t p2 = cur;
                int p2_depth = 0;
                while (p2 < len) {
                    if (g_ascii_isspace(text[p2])) {
                        p2++;
                    } else if (text[p2] == ',') {
                        p2++;
                    } else if (g_ascii_isalnum(text[p2]) || text[p2] == '_' || text[p2] == '.') {
                        p2++;
                    } else if (text[p2] == '(' || text[p2] == '[' || text[p2] == '{') {
                        p2_depth++;
                        p2++;
                    } else if (text[p2] == ')' || text[p2] == ']' || text[p2] == '}') {
                        if (p2_depth > 0) p2_depth--;
                        else break;
                        p2++;
                    } else if (text[p2] == '*') {
                        p2++;
                    } else if (text[p2] == '=') {
                        if (p2 + 1 < len && text[p2+1] == '=') break; /* == */
                        if (p2_depth == 0) is_assignment = TRUE;
                        break;
                    } else if (text[p2] == ':' && p2 + 1 < len && text[p2+1] == '=') {
                        if (p2_depth == 0) is_assignment = TRUE;
                        break;
                    } else if (text[p2] == '#') {
                        break;
                    } else {
                        break;
                    }
                }

                /* 1. Keywords / Bools / Builtins */
                /* 'self' -> Yellow (d_type) */
                if (word_len == 4 && strncmp(word_start, "self", 4) == 0) {
                     add_attr(attrs, start_pos, cur, &d_type); 
                }
                else if (is_word_in_list(word_start, word_len, py_keywords)) {
                    add_attr(attrs, start_pos, cur, &d_keyword);
                    /* Check for 'lambda' to start lambda param state */
                    if (word_len == 6 && strncmp(word_start, "lambda", 6) == 0) {
                        in_lambda_def = TRUE;
                    }
                }
                else if (is_word_in_list(word_start, word_len, py_bools)) {
                     add_attr(attrs, start_pos, cur, &d_number);
                }
                else if (is_word_in_list(word_start, word_len, py_builtins)) {
                     add_attr(attrs, start_pos, cur, &d_builtin);
                }
                /* 1.5. Special Dunder Variables */
                else if (word_len >= 4 && word_start[0] == '_' && word_start[1] == '_' && 
                         word_start[word_len-1] == '_' && word_start[word_len-2] == '_') {
                     if (is_word_in_list(word_start, word_len, py_special_vars)) {
                         add_attr(attrs, start_pos, cur, &d_variable); /* Red */
                     } else {
                         add_attr(attrs, start_pos, cur, &d_logical);  /* Cyan */
                     }
                }
                /* 2. Function Call */
                else if (is_call) {
                     add_attr(attrs, start_pos, cur, &d_function);
                }
                /* 3. Attributes */
                else if (is_attr) {
                     /* Check if owner is 'self'. 
                        Look back from start_pos-1 (the dot). 
                     */
                     gboolean owner_is_self = FALSE;
                     if (start_pos > 1) {
                         size_t p = start_pos - 1; /* at dot */
                         if (p > 0) p--; /* before dot */
                         /* skip whitespace backwards? usually none, but safe to skip */
                         // while (p > 0 && g_ascii_isspace(text[p])) p--; 
                         
                         /* Check if 4 chars ending at p are 'self' */
                         /* We need to be careful about bounds and ensuring logic matches 'self' word */
                         if (p >= 3 && strncmp(text + p - 3, "self", 4) == 0) {
                             /* Check boundary: p-4 shouldn't be identifier char */
                             if (p == 3 || (p > 3 && !g_ascii_isalnum(text[p-4]) && text[p-4] != '_')) {
                                 owner_is_self = TRUE;
                             }
                         }
                     }
                     
                     if (owner_is_self) {
                         add_attr(attrs, start_pos, cur, &d_variable_c); /* self.XXX -> Grey */
                     } else {
                         add_attr(attrs, start_pos, cur, &d_type); /* other.XXX -> Yellow */
                     }
                }
                /* 4. Lambda Params -> Orange */
                else if (in_lambda_def) {
                    add_attr(attrs, start_pos, cur, &d_attribute); /* Orange */
                }
                /* 5. Types (CamelCase) -> Yellow */
                else if (g_ascii_isupper(word_start[0]) && !is_all_caps(word_start, word_len)) {
                     add_attr(attrs, start_pos, cur, &d_type);
                }
                /* 6. Assignment LHS / Keyword Arg -> Red */
                else if (is_assignment) {
                     add_attr(attrs, start_pos, cur, &d_variable);
                }
                /* 7. Inside Parens (Args) -> Red */
                else if (paren_depth > 0) {
                     add_attr(attrs, start_pos, cur, &d_variable);
                }
                /* 8. Default Variable -> Grey */
                else {
                    add_attr(attrs, start_pos, cur, &d_variable_c);
                }
                continue;
            }

            /* Decorators */
            if (text[cur] == '@') {
                size_t start_pos = cur;
                cur++;
                while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_' || text[cur] == '.')) {
                    cur++;
                }
                add_attr(attrs, start_pos, cur, &d_decorator);
                continue;
            }
            
            /* Punctuation */
            /* Handle '.' specifically as Grey */
            if (text[cur] == '.') {
                 add_attr(attrs, cur, cur+1, &d_variable_c);
                 cur++;
                 continue;
            }
            
            /* Colon terminates lambda definition */
            if (text[cur] == ':') {
                 in_lambda_def = FALSE;
                 add_attr(attrs, cur, cur+1, &d_punctuation);
                 cur++;
                 continue;
            }
            
            /* Parens track depth & Rainbow Brackets */
            /* Cycle: Orange -> Purple -> Cyan */
            if (strchr("([{", text[cur])) {
                 const PangoColor *bracket_color;
                 int depth_mod = paren_depth % 3;
                 
                 if (depth_mod == 0) bracket_color = &d_punctuation; /* Orange */
                 else if (depth_mod == 1) bracket_color = &d_keyword; /* Purple */
                 else bracket_color = &d_logical; /* Cyan */
                 
                 add_attr(attrs, cur, cur+1, bracket_color);
                 paren_depth++;
                 cur++;
                 continue;
            }
            if (strchr(")]}", text[cur])) {
                 if (paren_depth > 0) paren_depth--;
                 
                 const PangoColor *bracket_color;
                 int depth_mod = paren_depth % 3;
                 
                 if (depth_mod == 0) bracket_color = &d_punctuation; /* Orange */
                 else if (depth_mod == 1) bracket_color = &d_keyword; /* Purple */
                 else bracket_color = &d_logical; /* Cyan */
                 
                 add_attr(attrs, cur, cur+1, bracket_color);
                 cur++;
                 continue;
            }
            
            /* Other Punctuation */
            if (strchr(":;,", text[cur])) {
                 add_attr(attrs, cur, cur+1, &d_punctuation);
                 cur++;
                 continue;
            }
            
            /* Operators - Logical cyan */
            if (strchr("=+-*/%&|^<>!~", text[cur])) {
                 /* = & | explicitly requested as Cyan */
                 if (strchr("=&|", text[cur])) {
                     /* Careful with ==, &&, || */
                     add_attr(attrs, cur, cur+1, &d_logical);
                     cur++;
                     continue;
                 }
                 
                 /* Compare ops: ==, !=, <=, >= */
                 if (cur + 1 < len) {
                     if ((text[cur] == '=' && text[cur+1] == '=') ||
                         (text[cur] == '!' && text[cur+1] == '=') ||
                         (text[cur] == '<' && text[cur+1] == '=') ||
                         (text[cur] == '>' && text[cur+1] == '=')) {
                         add_attr(attrs, cur, cur+2, &d_logical);
                         cur += 2;
                         continue;
                     }
                 }
                 /* Single Ops */
                 if (text[cur] == '<' || text[cur] == '>') {
                     add_attr(attrs, cur, cur+1, &d_logical);
                     cur++;
                     continue;
                 }
                 
                 /* Other math ops: +, -, *, /, % */
                 /* Can stay keyword (purple) or operator (orange). Let's keep Keyword/Purple for now */
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
