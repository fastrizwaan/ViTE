#include "syntax-internal.h"
#include <string.h>

static const char *c_control_keywords[] = {
    "break", "case", "continue", "default", "do", "else", 
    "for", "goto", "if", "return", "switch", "while", 
    /* Primitive types in VSCode C grammar map to Cyan, matching control keywords */
    "bool", "char", "double", "float", "int", "long", "short", "signed", "unsigned", "void", 
    "size_t", "ssize_t", "ptrdiff_t", "int8_t", "uint8_t", "int16_t", "uint16_t",
    "int32_t", "uint32_t", "int64_t", "uint64_t", "intptr_t", "uintptr_t", NULL
};

static const char *c_storage_modifiers[] = {
    "auto", "const", "extern", "inline", "register", "restrict", "static", "volatile", NULL
};

static const char *c_keywords[] = {
    "catch", "class", "delete", "friend", "namespace", "new", 
    "operator", "private", "protected", "public", "sizeof", "template", "this", 
    "throw", "try", "typename", "using", "virtual", NULL
};

static const char *c_special_constants[] = {
    "NULL", "TRUE", "FALSE", "true", "false", NULL
};

/* glib specific types */
static const char *glib_types[] = {
    "gboolean", "gpointer", "gconstpointer", "gchar", "guchar", "gint", "guint",
    "gshort", "gushort", "glong", "gulong", "gint8", "guint8", "gint16", "guint16",
    "gint32", "guint32", "gint64", "guint64", "gfloat", "gdouble", "gsize", "gssize",
    "goffset", "gintptr", "guintptr", "gunichar", "GObject", "GType", "GError", 
    "GList", "GSList", "GHashTable", "GPtrArray", "GBytes", "GString", NULL
};

/* standard C structs/unions mapping to Blue */
static const char *std_types[] = {
    "struct", "union", "enum", "typedef",
    "_Bool", "_Complex", "_Imaginary", NULL
};

void 
syntax_highlight_c(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index)
{
    /* Fast Path: State computation only (no attributes) */
    if (!attrs) {
        size_t cur = 0;
        int paren_depth = 0;
        
        while (cur < len) {
            /* 1. Handling Multi-line Comment States */
            if (state == STATE_IN_ML_COMMENT || state == STATE_C_ENUM_ML_COMMENT || state == STATE_C_PARAMS_ML_COMMENT) {
                const char *found = strstr(text + cur, "*/");
                if (found) {
                     cur = (found - text) + 2;
                     if (state == STATE_C_ENUM_ML_COMMENT) state = STATE_C_ENUM;
                     else if (state == STATE_C_PARAMS_ML_COMMENT) state = STATE_C_PARAMS;
                     else state = STATE_ROOT;
                } else {
                     cur = len;
                }
                continue;
            }
            
            /* 2. Fast Scan for Triggers */
            /* We can skip until we see interesting characters: / " ' { } ( ) ; */
            /* Using strhpbrk or manual loop. Manual loop often faster for small sets. */
            /* Optimization: Skip plain alphanumeric/space runs quickly */
            /* But we need to catch 'enum' keyword if we want to maintain ENUM state. 
               However, strictly solving the user's issue (ML comments/quotes) allows us to be lazier about ENUMs 
               if we accept that jumping into a huge Enum might lose coloring initially. 
               Let's try to maintain it if possible, but prioritize speed. 
            */
            
            char c = text[cur];
            
            if (c == '/') {
                if (cur + 1 < len) {
                    if (text[cur+1] == '/') { cur = len; continue; }
                    if (text[cur+1] == '*') {
                        if (state == STATE_C_ENUM) state = STATE_C_ENUM_ML_COMMENT;
                        else if (state == STATE_C_PARAMS) state = STATE_C_PARAMS_ML_COMMENT;
                        else state = STATE_IN_ML_COMMENT;
                        cur += 2;
                        continue;
                    }
                }
            } else if (c == '"' || c == '\'') {
                char quote = c;
                cur++;
                while (cur < len) {
                    if (text[cur] == quote && text[cur-1] != '\\') {
                        cur++;
                        break;
                    }
                    if (text[cur] == '\\') cur++; /* Skip escaped char */
                    cur++;
                }
                continue;
            } else if (state == STATE_ROOT) {
                 /* Basic keyword detection for 'enum' */
                 if (c == 'e' && strncmp(text + cur, "enum", 4) == 0) {
                     /* check boundary */
                     if (cur + 4 >= len || !g_ascii_isalnum(text[cur+4])) {
                         state = STATE_C_ENUM_WAIT_LBRACE;
                         cur += 4;
                         continue;
                     }
                 }
                 /* Detect function calls? Too complex for fast path, relies on parens */
                 /* We'll skip entering PARAMS state in fast path for now. 
                    This means function params might be less colorful after a massive jump, 
                    but comments will be correct. */
            } else if (state == STATE_C_ENUM_WAIT_LBRACE) {
                if (c == '{') state = STATE_C_ENUM;
                else if (c == ';') state = STATE_ROOT;
            } else if (state == STATE_C_ENUM) {
                if (c == '}') state = STATE_ROOT;
            } else if (state == STATE_C_PARAMS) {
                if (c == ')') {
                    if (paren_depth == 0) state = STATE_ROOT;
                    else paren_depth--;
                } else if (c == '(') {
                    paren_depth++;
                }
            }
            
            cur++;
        }
        
        set_line_end_state(ctx, line_index, state);
        return;
    }


    size_t cur = 0;
    int paren_depth = 0;
    while (cur < len) {
        if (state == STATE_C_ENUM_ML_COMMENT) {
            size_t start_pos = cur;
            while (cur + 1 < len) {
                 if (text[cur] == '*' && text[cur+1] == '/') {
                      cur += 2;
                      state = STATE_C_ENUM;
                      break;
                 }
                 cur++;
            }
            if (state == STATE_C_ENUM_ML_COMMENT) cur = len;
            add_color_attr(attrs, start_pos, cur, COLOR_COMMENT);
            continue;
        }
        if (state == STATE_C_PARAMS_ML_COMMENT) {
            size_t start_pos = cur;
            while (cur + 1 < len) {
                 if (text[cur] == '*' && text[cur+1] == '/') {
                      cur += 2;
                      state = STATE_C_PARAMS;
                      break;
                 }
                 cur++;
            }
            if (state == STATE_C_PARAMS_ML_COMMENT) cur = len;
            add_color_attr(attrs, start_pos, cur, COLOR_COMMENT);
            continue;
        }
        if (state == STATE_C_ENUM_WAIT_LBRACE) {
            if (g_ascii_isspace(text[cur])) { cur++; continue; }
            if (text[cur] == '{') {
                state = STATE_C_ENUM;
                add_color_attr(attrs, cur, cur + 1, COLOR_PUNCTUATION);
                cur++;
                continue;
            }
            if (text[cur] == ';') {
                state = STATE_ROOT;
                add_color_attr(attrs, cur, cur + 1, COLOR_PUNCTUATION);
                cur++;
                continue;
            }
            if (g_ascii_isalpha(text[cur]) || text[cur] == '_') {
                size_t s_pos = cur;
                while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                add_color_attr(attrs, s_pos, cur, COLOR_TYPE);
                continue;
            }
            cur++;
            continue;
        }
        if (state == STATE_C_ENUM) {
            if (g_ascii_isspace(text[cur])) { cur++; continue; }
            if (text[cur] == '/') {
                if (cur + 1 < len && text[cur+1] == '/') {
                    add_color_attr(attrs, cur, len, COLOR_COMMENT);
                    cur = len;
                    continue;
                }
                if (cur + 1 < len && text[cur+1] == '*') {
                    state = STATE_C_ENUM_ML_COMMENT;
                    size_t start_pos = cur;
                    cur += 2;
                    while (cur + 1 < len) {
                         if (text[cur] == '*' && text[cur+1] == '/') {
                              cur += 2;
                              state = STATE_C_ENUM;
                              break;
                         }
                         cur++;
                    }
                    if (state == STATE_C_ENUM_ML_COMMENT) cur = len;
                    add_color_attr(attrs, start_pos, cur, COLOR_COMMENT);
                    continue;
                }
            }
            if (text[cur] == '}') {
                state = STATE_ROOT;
                add_color_attr(attrs, cur, cur + 1, COLOR_PUNCTUATION);
                cur++;
                continue;
            }
            if (g_ascii_isalpha(text[cur]) || text[cur] == '_') {
                size_t s_pos = cur;
                while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                add_color_attr(attrs, s_pos, cur, COLOR_CONSTANT);
                continue;
            }
            if (g_ascii_isdigit(text[cur])) {
                 size_t s_pos = cur;
                 while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '.')) cur++;
                 add_color_attr(attrs, s_pos, cur, COLOR_NUMBER);
                 continue;
            }
            if (strchr("=,+-*/%&|^<>!?:", text[cur])) {
                add_color_attr(attrs, cur, cur + 1, COLOR_OPERATOR);
                cur++;
                continue;
            }
            cur++;
            continue;
        }
        if (state == STATE_C_PARAMS) {
            /* Skip spaces */
            if (g_ascii_isspace(text[cur])) {
                cur++;
                continue;
            }
            if (text[cur] == '/' && cur+1 < len && text[cur+1] == '/') {
                add_color_attr(attrs, cur, len, COLOR_COMMENT);
                cur = len;
                continue;
            }
            if (text[cur] == '/' && cur+1 < len && text[cur+1] == '*') {
                state = STATE_C_PARAMS_ML_COMMENT;
                size_t start_pos = cur;
                cur += 2;
                while (cur + 1 < len) {
                     if (text[cur] == '*' && text[cur+1] == '/') {
                          cur += 2;
                          state = STATE_C_PARAMS;
                          break;
                     }
                     cur++;
                }
                if (state == STATE_C_PARAMS_ML_COMMENT) cur = len;
                add_color_attr(attrs, start_pos, cur, COLOR_COMMENT);
                continue;
            }
            
            /* Handle Strings in Params (copied from ROOT) */
            if (text[cur] == '"') {
                size_t start_pos = cur;
                cur++;
                while (cur < len) {
                     if (text[cur] == '"' && text[cur-1] != '\\') {
                         cur++;
                         break;
                     }
                     /* Escape Sequences */
                     if (text[cur] == '\\') {
                         if (cur > start_pos) add_color_attr(attrs, start_pos, cur, COLOR_STRING);
                         size_t esc_start = cur;
                         cur++;
                         if (cur < len) {
                             /* Simple escapes */
                             if (strchr("ntr0\\\"\'abfv?", text[cur])) {
                                 cur++;
                             } else if (text[cur] == 'x') {
                                 cur++;
                                 while (cur < len && g_ascii_isxdigit(text[cur])) cur++;
                             } else if (g_ascii_isdigit(text[cur])) { /* Octal-ish */
                                 while (cur < len && g_ascii_isdigit(text[cur])) cur++;
                             } else {
                                 cur++;
                             }
                         }
                         add_color_attr(attrs, esc_start, cur, COLOR_BUILTIN); /* Cyan */
                         start_pos = cur;
                         continue;
                     }
                     /* Format Specifiers: %... */
                     if (text[cur] == '%') {
                         /* Add attribute for string part before % */
                         if (cur > start_pos) {
                             add_color_attr(attrs, start_pos, cur, COLOR_STRING);
                         }
                         
                         size_t fmt_start = cur;
                         cur++;
                         /* Parse flags */
                         while (cur < len && strchr("-+ #0", text[cur])) cur++;
                         /* Parse width */
                         if (cur < len && text[cur] == '*') cur++;
                         else while (cur < len && g_ascii_isdigit(text[cur])) cur++;
                         /* Parse precision */
                         if (cur < len && text[cur] == '.') {
                             cur++;
                             if (cur < len && text[cur] == '*') cur++;
                             else while (cur < len && g_ascii_isdigit(text[cur])) cur++;
                         }
                         /* Parse length modifiers */
                         if (cur < len) {
                             if (strchr("hljztL", text[cur])) {
                                 cur++;
                                 if (cur < len && text[cur-1] == 'l' && text[cur] == 'l') cur++;
                                 else if (cur < len && text[cur-1] == 'h' && text[cur] == 'h') cur++;
                             }
                         }
                         /* Parse specifier */
                         if (cur < len && strchr("diuoxXfFeEgGaAcspn%", text[cur])) {
                             cur++;
                             add_color_attr(attrs, fmt_start, cur, COLOR_NUMBER);
                         } else {
                             add_color_attr(attrs, fmt_start, cur, COLOR_STRING); 
                         }
                         start_pos = cur; /* Reset start for next string chunk */
                         continue;
                     }
                     cur++;
                }
                /* Add remaining string part */
                if (cur > start_pos) {
                    add_color_attr(attrs, start_pos, cur, COLOR_STRING);
                }
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
                     /* Escape Sequences */
                     if (text[cur] == '\\') {
                         if (cur > start_pos) add_color_attr(attrs, start_pos, cur, COLOR_STRING);
                         size_t esc_start = cur;
                         cur++;
                         if (cur < len) {
                             if (strchr("ntr0\\\"\'abfv?", text[cur])) cur++;
                             else if (text[cur] == 'x') { cur++; while (cur < len && g_ascii_isxdigit(text[cur])) cur++; }
                             else if (g_ascii_isdigit(text[cur])) { while (cur < len && g_ascii_isdigit(text[cur])) cur++; }
                             else cur++;
                         }
                         add_color_attr(attrs, esc_start, cur, COLOR_BUILTIN);
                         start_pos = cur;
                         continue;
                     }
                     cur++;
                }
                add_color_attr(attrs, start_pos, cur, COLOR_STRING);
                continue;
            }
            /* Check for ')' moved to generic bracket block below */

            if (text[cur] == '!' && (cur + 1 >= len || text[cur+1] != '=')) {
                add_color_attr(attrs, cur, cur + 1, COLOR_LOGICAL);
                cur++;
                continue;
            }
            if (text[cur] == '&' && cur + 1 < len && text[cur+1] == '&') {
                add_color_attr(attrs, cur, cur + 2, COLOR_LOGICAL);
                cur += 2;
                continue;
            }
            if (text[cur] == '|' && cur + 1 < len && text[cur+1] == '|') {
                add_color_attr(attrs, cur, cur + 2, COLOR_LOGICAL);
                cur += 2;
                continue;
            }
            if (text[cur] == '-' && cur + 1 < len && text[cur+1] == '>') {
                add_color_attr(attrs, cur, cur + 2, COLOR_VARIABLE);
                cur += 2;
                /* Highlight member after -> in red */
                while (cur < len && g_ascii_isspace(text[cur])) cur++;
                if (cur < len && (g_ascii_isalpha(text[cur]) || text[cur] == '_')) {
                    size_t m_start = cur;
                    while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                    add_color_attr(attrs, m_start, cur, COLOR_VARIABLE_C);
                }
                continue;
            }
            if (text[cur] == '*' || text[cur] == '&') {
                add_color_attr(attrs, cur, cur + 1, COLOR_OPERATOR);
                cur++;
                continue;
            }
            if (text[cur] == ',') {
                /* Default color for comma */
                cur++;
                continue;
            }
            if (g_ascii_isdigit(text[cur])) {
                size_t start_pos = cur;
                gboolean is_hex = FALSE;
                gboolean has_dot = FALSE;
                gboolean has_exp = FALSE;
                
                if (text[cur] == '0' && cur+1 < len && (text[cur+1] == 'x' || text[cur+1] == 'X')) {
                    is_hex = TRUE;
                    cur += 2;
                    while (cur < len && (g_ascii_isxdigit(text[cur]) || text[cur] == '.' || text[cur] == 'p' || text[cur] == 'P')) {
                        if (text[cur] == '.') has_dot = TRUE;
                        if (text[cur] == 'p' || text[cur] == 'P') has_exp = TRUE;
                        cur++;
                    }
                } else {
                    while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '.')) {
                        if (text[cur] == '.') has_dot = TRUE;
                        if (text[cur] == 'e' || text[cur] == 'E') has_exp = TRUE;
                        cur++;
                    }
                }
                
                size_t num_end = cur;
                while (num_end > start_pos) {
                    char c = text[num_end-1];
                    if (c == 'f' || c == 'F') {
                        if (!is_hex || has_dot || has_exp) { num_end--; continue; }
                    } else if (c == 'l' || c == 'L' || c == 'u' || c == 'U') {
                        num_end--;
                        continue;
                    }
                    break;
                }
                if (num_end > start_pos) add_color_attr(attrs, start_pos, num_end, COLOR_NUMBER);
                if (num_end < cur) add_color_attr(attrs, num_end, cur, COLOR_VARIABLE_C);
                continue;
            }
            
            if (g_ascii_isalpha(text[cur]) || text[cur] == '_') {
                size_t start_pos = cur;
                while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) {
                    cur++;
                }
                size_t word_len = cur - start_pos;
                const char *word_start = text + start_pos;
                
                gboolean is_keyword = is_word_in_list(word_start, word_len, c_keywords);
                gboolean is_control_keyword = is_word_in_list(word_start, word_len, c_control_keywords);
                gboolean is_storage_modifier = is_word_in_list(word_start, word_len, c_storage_modifiers);
                gboolean is_glib_type = is_word_in_list(word_start, word_len, glib_types);
                gboolean is_std_type = is_word_in_list(word_start, word_len, std_types);
                gboolean is_pointer_access = FALSE;
                
                /* Peek for -> */
                size_t p = cur;
                while (p < len && g_ascii_isspace(text[p])) p++;
                if (p + 1 < len && text[p] == '-' && text[p+1] == '>') is_pointer_access = TRUE;

                /* Check for nested function call */
                gboolean is_func_call = FALSE;
                size_t peek = cur;
                while (peek < len && g_ascii_isspace(text[peek])) peek++;
                /* Comment awareness in peek? Simplified for now */
                if (peek < len && text[peek] == '(') is_func_call = TRUE;

                if (is_keyword) {
                    add_color_attr(attrs, start_pos, cur, COLOR_KEYWORD);
                } else if (is_control_keyword) {
                    add_color_attr(attrs, start_pos, cur, COLOR_KEYWORD_CONTROL);
                } else if (is_storage_modifier) {
                    add_color_attr(attrs, start_pos, cur, COLOR_STORAGE);
                } else if (is_func_call) {
                    add_color_attr(attrs, start_pos, cur, COLOR_FUNCTION);
                } else if (is_word_in_list(word_start, word_len, c_special_constants)) {
                    add_color_attr(attrs, start_pos, cur, COLOR_CONSTANT_LANG);
                } else if (is_pointer_access || is_glib_type) {
                    add_color_attr(attrs, start_pos, cur, COLOR_TYPE);
                } else if (is_std_type || (word_len > 2 && word_start[word_len-1] == 't' && word_start[word_len-2] == '_')) {
                    add_color_attr(attrs, start_pos, cur, COLOR_STORAGE);
                } else if (g_ascii_isupper(word_start[0])) {
                    /* Types are conventionally capitalized */
                    add_color_attr(attrs, start_pos, cur, COLOR_TYPE);
                } else {
                    /* Parameter Name */
                    add_color_attr(attrs, start_pos, cur, COLOR_PARAM);
                }
                continue;
            }
            /* Rainbow Brackets in Params */
            if (strchr("([{", text[cur])) {
                 add_color_attr(attrs, cur, cur + 1, COLOR_PUNCTUATION);
                 paren_depth++;
                 cur++;
                 continue;
            }
            if (strchr(")]}", text[cur])) {
                 if (paren_depth > 0) paren_depth--;
                 add_color_attr(attrs, cur, cur + 1, COLOR_PUNCTUATION);
                 
                 /* If closing param paren, switch state */
                 if (text[cur] == ')' && paren_depth == 0) {
                     state = STATE_ROOT;
                 }
                 cur++;
                 continue;
            }

            /* Default fallback for punctuation in params */
            if (strchr(".;", text[cur])) {
                add_color_attr(attrs, cur, cur + 1, COLOR_PUNCTUATION);
                cur++;
                continue;
            } else if (text[cur] == '-' && cur + 1 < len && text[cur+1] == '>') {
                add_color_attr(attrs, cur, cur + 2, COLOR_VARIABLE);
                cur += 2;
                /* Highlight member after -> */
                while (cur < len && g_ascii_isspace(text[cur])) cur++;
                if (cur < len && (g_ascii_isalpha(text[cur]) || text[cur] == '_')) {
                    size_t m_start = cur;
                    while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                    size_t m_end = cur;
                    
                    /* Peek for another -> to see if this is intermediate */
                    size_t p = m_end;
                    while (p < len && g_ascii_isspace(text[p])) p++;
                    if (p + 1 < len && text[p] == '-' && text[p+1] == '>') {
                        add_color_attr(attrs, m_start, m_end, COLOR_TYPE);
                    } else {
                        add_color_attr(attrs, m_start, m_end, COLOR_VARIABLE_C);
                    }
                    /* We don't continue here; the loop will continue from cur (m_end) */
                }
                continue;
            } else if (text[cur] == '!' && (cur + 1 >= len || text[cur+1] != '=')) {
                add_color_attr(attrs, cur, cur + 1, COLOR_LOGICAL);
            } else if (text[cur] == '&' && cur + 1 < len && text[cur+1] == '&') {
                add_color_attr(attrs, cur, cur + 2, COLOR_LOGICAL);
                cur++;
            } else if (text[cur] == '|' && cur + 1 < len && text[cur+1] == '|') {
                add_color_attr(attrs, cur, cur + 2, COLOR_LOGICAL);
                cur++;
            } else {
                /* Arithmetic and Assignment Operators in params */
                gboolean handled = FALSE;
                if (cur + 1 < len) {
                    if ((text[cur] == '+' || text[cur] == '-' || text[cur] == '*' || 
                         text[cur] == '/' || text[cur] == '%' || text[cur] == '=' ||
                         text[cur] == '<' || text[cur] == '>' || text[cur] == '!' ||
                         text[cur] == '&' || text[cur] == '|' || text[cur] == '^') && text[cur+1] == '=') {
                        add_color_attr(attrs, cur, cur + 2, COLOR_OPERATOR);
                        cur++;
                        handled = TRUE;
                    } else if (text[cur] == '<' && text[cur+1] == '<') {
                        add_color_attr(attrs, cur, cur + 2, COLOR_OPERATOR);
                        cur++;
                        handled = TRUE;
                    } else if (text[cur] == '>' && text[cur+1] == '>') {
                        add_color_attr(attrs, cur, cur + 2, COLOR_OPERATOR);
                        cur++;
                        handled = TRUE;
                    }
                }
                if (!handled && strchr("+-*/%=<>^|&?:", text[cur])) {
                    add_color_attr(attrs, cur, cur + 1, COLOR_OPERATOR);
                }
            }
            cur++;
            continue;
        }

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
            add_color_attr(attrs, start_pos, cur, COLOR_COMMENT);
            continue;
        }
        
        if (state == STATE_ROOT) {
            /* Whitespace */
            if (g_ascii_isspace(text[cur])) {
                cur++;
                continue;
            }
            
            /* Punctuation (Braces, parens, semicolons, etc) */
            if (strchr("{}()[].,;", text[cur])) {
                add_color_attr(attrs, cur, cur + 1, COLOR_PUNCTUATION);
                cur++;
                continue;
            }
            
            /* Comments */
            if (text[cur] == '/' && cur+1 < len && text[cur+1] == '/') {
                add_color_attr(attrs, cur, len, COLOR_COMMENT);
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
                add_color_attr(attrs, start_pos, cur, COLOR_COMMENT);
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
                     /* Format Specifiers: %... */
                     if (text[cur] == '%') {
                         /* Add attribute for string part before % */
                         if (cur > start_pos) {
                             add_color_attr(attrs, start_pos, cur, COLOR_STRING);
                         }
                         
                         size_t fmt_start = cur;
                         cur++;
                         /* Parse flags */
                         while (cur < len && strchr("-+ #0", text[cur])) cur++;
                         /* Parse width */
                         if (cur < len && text[cur] == '*') cur++;
                         else while (cur < len && g_ascii_isdigit(text[cur])) cur++;
                         /* Parse precision */
                         if (cur < len && text[cur] == '.') {
                             cur++;
                             if (cur < len && text[cur] == '*') cur++;
                             else while (cur < len && g_ascii_isdigit(text[cur])) cur++;
                         }
                         /* Parse length modifiers */
                         if (cur < len) {
                             if (strchr("hljztL", text[cur])) {
                                 cur++;
                                 if (cur < len && text[cur-1] == 'l' && text[cur] == 'l') cur++;
                                 else if (cur < len && text[cur-1] == 'h' && text[cur] == 'h') cur++;
                             }
                         }
                         /* Parse specifier */
                         if (cur < len && strchr("diuoxXfFeEgGaAcspn%", text[cur])) {
                             cur++;
                             add_color_attr(attrs, fmt_start, cur, COLOR_NUMBER);
                         } else {
                             add_color_attr(attrs, fmt_start, cur, COLOR_STRING); 
                         }
                         start_pos = cur; /* Reset start for next string chunk */
                         continue;
                     }
                     cur++;
                }
                /* Add remaining string part */
                if (cur > start_pos) {
                    add_color_attr(attrs, start_pos, cur, COLOR_STRING);
                }
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
                     /* Escape Sequences */
                     if (text[cur] == '\\') {
                         if (cur > start_pos) add_color_attr(attrs, start_pos, cur, COLOR_STRING);
                         size_t esc_start = cur;
                         cur++;
                         if (cur < len) {
                             if (strchr("ntr0\\\"\'abfv?", text[cur])) cur++;
                             else if (text[cur] == 'x') { cur++; while (cur < len && g_ascii_isxdigit(text[cur])) cur++; }
                             else if (g_ascii_isdigit(text[cur])) { while (cur < len && g_ascii_isdigit(text[cur])) cur++; }
                             else cur++;
                         }
                         add_color_attr(attrs, esc_start, cur, COLOR_BUILTIN);
                         start_pos = cur;
                         continue;
                     }
                     cur++;
                }
                add_color_attr(attrs, start_pos, cur, COLOR_STRING);
                continue;
            }
            
            /* Preprocessor */
            if (text[cur] == '#') {
                size_t start_pos = cur;
                cur++;
                add_color_attr(attrs, start_pos, cur, COLOR_KEYWORD_CONTROL);
                
                /* Allow space between # and directive */
                while (cur < len && g_ascii_isspace(text[cur])) cur++;
                
                size_t directive_start = cur;
                while (cur < len && g_ascii_isalpha(text[cur])) {
                    cur++;
                }
                size_t directive_len = cur - directive_start;
                const char *directive = text + directive_start;

                add_color_attr(attrs, directive_start, cur, COLOR_KEYWORD_CONTROL);

                if (directive_len == 7 && strncmp(directive, "include", 7) == 0) {
                    /* Skip whitespace */
                    while (cur < len && g_ascii_isspace(text[cur])) cur++;
                    
                    if (cur < len && (text[cur] == '"' || text[cur] == '<' || text[cur] == '\'')) {
                        char open = text[cur];
                        char close = (open == '<') ? '>' : open;
                        size_t path_start = cur;
                        cur++;
                        
                        /* Color the open quote/bracket */
                        add_color_attr(attrs, path_start, cur, COLOR_STRING);
                        
                        size_t inner_start = cur;
                        while (cur < len && text[cur] != close) cur++;
                        
                        /* Color the inner path */
                        if (cur > inner_start) {
                            add_color_attr(attrs, inner_start, cur, COLOR_STRING);
                        }
                        
                        /* Color the close quote/bracket */
                        if (cur < len) {
                            add_color_attr(attrs, cur, cur + 1, COLOR_STRING);
                            cur++; /* include closer */
                        }
                    }
                } else if ((directive_len == 6 && strncmp(directive, "define", 6) == 0) ||
                           (directive_len == 5 && strncmp(directive, "ifdef", 5) == 0) ||
                           (directive_len == 6 && strncmp(directive, "ifndef", 6) == 0) ||
                           (directive_len == 2 && strncmp(directive, "if", 2) == 0) ||
                           (directive_len == 4 && strncmp(directive, "elif", 4) == 0) ||
                           (directive_len == 5 && strncmp(directive, "undef", 5) == 0)) {
                    /* First identifier after these directives is a macro name -> blue */
                    while (cur < len && g_ascii_isspace(text[cur])) cur++;
                    if (cur < len && (g_ascii_isalpha(text[cur]) || text[cur] == '_')) {
                        size_t macro_start = cur;
                        while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) {
                            cur++;
                        }
                        add_color_attr(attrs, macro_start, cur, COLOR_FUNCTION);
                    }
                }
                continue;
            }

            /* Numbers */
            if (g_ascii_isdigit(text[cur])) {
                size_t start_pos = cur;
                gboolean is_hex = FALSE;
                gboolean has_dot = FALSE;
                gboolean has_exp = FALSE;
                
                if (text[cur] == '0' && cur+1 < len && (text[cur+1] == 'x' || text[cur+1] == 'X')) {
                    is_hex = TRUE;
                    cur += 2;
                    while (cur < len && (g_ascii_isxdigit(text[cur]) || text[cur] == '.' || text[cur] == 'p' || text[cur] == 'P')) {
                        if (text[cur] == '.') has_dot = TRUE;
                        if (text[cur] == 'p' || text[cur] == 'P') has_exp = TRUE;
                        cur++;
                    }
                } else {
                    while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '.')) {
                        if (text[cur] == '.') has_dot = TRUE;
                        if (text[cur] == 'e' || text[cur] == 'E') has_exp = TRUE;
                        cur++;
                    }
                }
                
                size_t num_end = cur;
                while (num_end > start_pos) {
                    char c = text[num_end-1];
                    if (c == 'f' || c == 'F') {
                        if (!is_hex || has_dot || has_exp) { num_end--; continue; }
                    } else if (c == 'l' || c == 'L' || c == 'u' || c == 'U') {
                        num_end--;
                        continue;
                    }
                    break;
                }
                if (num_end > start_pos) add_color_attr(attrs, start_pos, num_end, COLOR_NUMBER);
                if (num_end < cur) add_color_attr(attrs, num_end, cur, COLOR_VARIABLE_C);
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

                gboolean is_keyword = is_word_in_list(word_start, word_len, c_keywords);
                gboolean is_control_keyword = is_word_in_list(word_start, word_len, c_control_keywords);
                gboolean is_storage_modifier = is_word_in_list(word_start, word_len, c_storage_modifiers);
                gboolean is_glib_type = is_word_in_list(word_start, word_len, glib_types);
                gboolean is_std_type = is_word_in_list(word_start, word_len, std_types);
                gboolean is_pointer_access = FALSE;
                
                /* Peek for -> */
                size_t p = cur;
                while (p < len && g_ascii_isspace(text[p])) p++;
                if (p + 1 < len && text[p] == '-' && text[p+1] == '>') is_pointer_access = TRUE;

                if (is_keyword) {
                    add_color_attr(attrs, start_pos, cur, COLOR_KEYWORD);
                    if (word_len == 4 && strncmp(word_start, "enum", 4) == 0) {
                        state = STATE_C_ENUM_WAIT_LBRACE;
                    }
                } else if (is_control_keyword) {
                    add_color_attr(attrs, start_pos, cur, COLOR_KEYWORD_CONTROL);
                } else if (is_storage_modifier) {
                    add_color_attr(attrs, start_pos, cur, COLOR_STORAGE);
                } else if (is_func_call) {
                     add_color_attr(attrs, start_pos, cur, COLOR_FUNCTION);
                     state = STATE_C_PARAMS;
                } else if (is_word_in_list(word_start, word_len, c_special_constants)) {
                    add_color_attr(attrs, start_pos, cur, COLOR_CONSTANT_LANG);
                } else if (is_pointer_access || is_glib_type) {
                    add_color_attr(attrs, start_pos, cur, COLOR_TYPE);
                } else if (is_std_type || (word_len > 2 && word_start[word_len-1] == 't' && word_start[word_len-2] == '_')) {
                    add_color_attr(attrs, start_pos, cur, COLOR_STORAGE);
                }
                continue;
            }

            /* Operators and Punctuation */
            /* Rainbow Brackets */
            if (strchr("([{", text[cur])) {
                 add_color_attr(attrs, cur, cur + 1, COLOR_PUNCTUATION);
                 paren_depth++;
                 cur++;
                 continue;
            }
            if (strchr(")]}", text[cur])) {
                 if (paren_depth > 0) paren_depth--;
                 add_color_attr(attrs, cur, cur + 1, COLOR_PUNCTUATION);
                 cur++;
                 continue;
            }
            if (text[cur] == '-' && cur + 1 < len && text[cur+1] == '>') {
                add_color_attr(attrs, cur, cur + 2, COLOR_VARIABLE);
                cur += 2;
                /* Highlight member after -> in red */
                while (cur < len && g_ascii_isspace(text[cur])) cur++;
                if (cur < len && (g_ascii_isalpha(text[cur]) || text[cur] == '_')) {
                    size_t m_start = cur;
                    while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                    size_t m_end = cur;

                    /* Peek for another -> to see if this is intermediate */
                    size_t p = m_end;
                    while (p < len && g_ascii_isspace(text[p])) p++;
                    if (p + 1 < len && text[p] == '-' && text[p+1] == '>') {
                        add_color_attr(attrs, m_start, m_end, COLOR_TYPE);
                    } else {
                        add_color_attr(attrs, m_start, m_end, COLOR_VARIABLE_C);
                    }
                }
                continue;
            }
            if (text[cur] == '!' && (cur + 1 >= len || text[cur+1] != '=')) {
                add_color_attr(attrs, cur, cur + 1, COLOR_LOGICAL);
                cur++;
                continue;
            }
            if (text[cur] == '&' && cur + 1 < len && text[cur+1] == '&') {
                add_color_attr(attrs, cur, cur + 2, COLOR_LOGICAL);
                cur += 2;
                continue;
            }
            if (text[cur] == '|' && cur + 1 < len && text[cur+1] == '|') {
                add_color_attr(attrs, cur, cur + 2, COLOR_LOGICAL);
                cur += 2;
                continue;
            }
            
            /* Compound Operators */
            if (cur + 1 < len) {
                if ((text[cur] == '+' || text[cur] == '-' || text[cur] == '*' || 
                     text[cur] == '/' || text[cur] == '%' || text[cur] == '=' ||
                     text[cur] == '<' || text[cur] == '>' || text[cur] == '!' ||
                     text[cur] == '&' || text[cur] == '|' || text[cur] == '^') && text[cur+1] == '=') {
                    add_color_attr(attrs, cur, cur + 2, COLOR_OPERATOR);
                    cur += 2;
                    continue;
                }
                if (text[cur] == '<' && text[cur+1] == '<') {
                    add_color_attr(attrs, cur, cur + 2, COLOR_OPERATOR);
                    cur += 2;
                    continue;
                }
                if (text[cur] == '>' && text[cur+1] == '>') {
                    add_color_attr(attrs, cur, cur + 2, COLOR_OPERATOR);
                    cur += 2;
                    continue;
                }
            }

            if (strchr("+-*/%=<>^|&?:", text[cur])) {
                add_color_attr(attrs, cur, cur + 1, COLOR_OPERATOR);
                cur++;
                continue;
            }

            if (strchr(".;", text[cur])) {
                add_color_attr(attrs, cur, cur + 1, COLOR_PUNCTUATION);
                cur++;
                continue;
            }

            cur++;
        } else {
            /* Should not be here if STATE_IN_ML_COMMENT handled above */
             state = STATE_ROOT;
             cur++;
        }
        
        /* IMPORTANT: Save the computed state! */
        set_line_end_state(ctx, line_index, state);
        return;
    }
    
    set_line_end_state(ctx, line_index, state);
}
