#include "syntax-internal.h"
#include <string.h>

static const char *c_keywords[] = {
    "auto", "bool", "break", "case", "catch", "char", "class", "const", "continue", 
    "default", "delete", "do", "double", "else", "enum", "extern", "float", 
    "for", "friend", "goto", "if", "inline", "int", "long", "namespace", "new", 
    "operator", "private", "protected", "public", "register", "restrict", "return", 
    "short", "signed", "sizeof", "static", "struct", "switch", "template", "this", 
    "throw", "try", "typedef", "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "while", 
    "_Bool", "_Complex", "_Imaginary", NULL
};

static const char *c_special_constants[] = {
    "NULL", "TRUE", "FALSE", "true", "false", NULL
};

static const char *glib_types[] = {
    "gboolean", "gpointer", "gconstpointer", "gchar", "guchar", "gint", "guint",
    "gshort", "gushort", "glong", "gulong", "gint8", "guint8", "gint16", "guint16",
    "gint32", "guint32", "gint64", "guint64", "gsize", "gssize", "goffset",
    "gfloat", "gdouble", "gunichar", "GObject", "GType", "GError", "GList", "GSList", "GHashTable",
    "GPtrArray", "GBytes", "GString", NULL
};

static const char *std_types[] = {
    "size_t", "ssize_t", "ptrdiff_t", "int8_t", "uint8_t", "int16_t", "uint16_t",
    "int32_t", "uint32_t", "int64_t", "uint64_t", "intptr_t", "uintptr_t", NULL
};

void 
syntax_highlight_c(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index)
{
    size_t cur = 0;
    int paren_depth = (state == STATE_C_PARAMS) ? 1 : 0;
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
            add_attr(attrs, start_pos, cur, &d_comment);
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
            add_attr(attrs, start_pos, cur, &d_comment);
            continue;
        }
        if (state == STATE_C_ENUM_WAIT_LBRACE) {
            if (g_ascii_isspace(text[cur])) { cur++; continue; }
            if (text[cur] == '{') {
                state = STATE_C_ENUM;
                add_attr(attrs, cur, cur + 1, &d_punctuation);
                cur++;
                continue;
            }
            if (text[cur] == ';') {
                state = STATE_ROOT;
                add_attr(attrs, cur, cur + 1, &d_punctuation);
                cur++;
                continue;
            }
            if (g_ascii_isalpha(text[cur]) || text[cur] == '_') {
                size_t s_pos = cur;
                while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                add_attr(attrs, s_pos, cur, &d_type);
                continue;
            }
            cur++;
            continue;
        }
        if (state == STATE_C_ENUM) {
            if (g_ascii_isspace(text[cur])) { cur++; continue; }
            if (text[cur] == '/') {
                if (cur + 1 < len && text[cur+1] == '/') {
                    add_attr(attrs, cur, len, &d_comment);
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
                    add_attr(attrs, start_pos, cur, &d_comment);
                    continue;
                }
            }
            if (text[cur] == '}') {
                state = STATE_ROOT;
                add_attr(attrs, cur, cur + 1, &d_punctuation);
                cur++;
                continue;
            }
            if (g_ascii_isalpha(text[cur]) || text[cur] == '_') {
                size_t s_pos = cur;
                while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                add_attr(attrs, s_pos, cur, &d_constant);
                continue;
            }
            if (g_ascii_isdigit(text[cur])) {
                 size_t s_pos = cur;
                 while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '.')) cur++;
                 add_attr(attrs, s_pos, cur, &d_number);
                 continue;
            }
            if (strchr("=,+-*/%&|^<>!?:", text[cur])) {
                add_attr(attrs, cur, cur + 1, &d_keyword);
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
                add_attr(attrs, cur, len, &d_comment);
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
                add_attr(attrs, start_pos, cur, &d_comment);
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
                         if (cur > start_pos) add_attr(attrs, start_pos, cur, &d_string);
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
                         add_attr(attrs, esc_start, cur, &d_builtin); /* Cyan */
                         start_pos = cur;
                         continue;
                     }
                     /* Format Specifiers: %... */
                     if (text[cur] == '%') {
                         /* Add attribute for string part before % */
                         if (cur > start_pos) {
                             add_attr(attrs, start_pos, cur, &d_string);
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
                             add_attr(attrs, fmt_start, cur, &d_number);
                         } else {
                             add_attr(attrs, fmt_start, cur, &d_string); 
                         }
                         start_pos = cur; /* Reset start for next string chunk */
                         continue;
                     }
                     cur++;
                }
                /* Add remaining string part */
                if (cur > start_pos) {
                    add_attr(attrs, start_pos, cur, &d_string);
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
                         if (cur > start_pos) add_attr(attrs, start_pos, cur, &d_string);
                         size_t esc_start = cur;
                         cur++;
                         if (cur < len) {
                             if (strchr("ntr0\\\"\'abfv?", text[cur])) cur++;
                             else if (text[cur] == 'x') { cur++; while (cur < len && g_ascii_isxdigit(text[cur])) cur++; }
                             else if (g_ascii_isdigit(text[cur])) { while (cur < len && g_ascii_isdigit(text[cur])) cur++; }
                             else cur++;
                         }
                         add_attr(attrs, esc_start, cur, &d_builtin);
                         start_pos = cur;
                         continue;
                     }
                     cur++;
                }
                add_attr(attrs, start_pos, cur, &d_string);
                continue;
            }
            /* Check for ')' moved to generic bracket block below */

            if (text[cur] == '!' && (cur + 1 >= len || text[cur+1] != '=')) {
                add_attr(attrs, cur, cur + 1, &d_logical);
                cur++;
                continue;
            }
            if (text[cur] == '&' && cur + 1 < len && text[cur+1] == '&') {
                add_attr(attrs, cur, cur + 2, &d_logical);
                cur += 2;
                continue;
            }
            if (text[cur] == '|' && cur + 1 < len && text[cur+1] == '|') {
                add_attr(attrs, cur, cur + 2, &d_logical);
                cur += 2;
                continue;
            }
            if (text[cur] == '-' && cur + 1 < len && text[cur+1] == '>') {
                add_attr(attrs, cur, cur + 2, &d_variable_c);
                cur += 2;
                /* Highlight member after -> in red */
                while (cur < len && g_ascii_isspace(text[cur])) cur++;
                if (cur < len && (g_ascii_isalpha(text[cur]) || text[cur] == '_')) {
                    size_t m_start = cur;
                    while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                    add_attr(attrs, m_start, cur, &d_variable);
                }
                continue;
            }
            if (text[cur] == '*' || text[cur] == '&') {
                add_attr(attrs, cur, cur + 1, &d_keyword);
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
                if (num_end > start_pos) add_attr(attrs, start_pos, num_end, &d_number);
                if (num_end < cur) add_attr(attrs, num_end, cur, &d_variable);
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
                gboolean is_type_list = is_word_in_list(word_start, word_len, glib_types) || 
                                      is_word_in_list(word_start, word_len, std_types);
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
                    add_attr(attrs, start_pos, cur, &d_keyword);
                } else if (is_func_call) {
                    add_attr(attrs, start_pos, cur, &d_function);
                } else if (is_word_in_list(word_start, word_len, c_special_constants) || 
                           is_pointer_access || is_type_list || 
                           g_ascii_isupper(word_start[0]) || 
                           (word_len > 2 && word_start[word_len-1] == 't' && word_start[word_len-2] == '_')) {
                    add_attr(attrs, start_pos, cur, &d_type);
                } else {
                    /* Parameter Name */
                    add_attr(attrs, start_pos, cur, &d_param);
                }
                continue;
            }
            /* Rainbow Brackets in Params */
            if (strchr("([{", text[cur])) {
                 const PangoColor *bracket_color;
                 int depth_mod = paren_depth % 3;
                 if (depth_mod == 0) bracket_color = &d_punctuation;
                 else if (depth_mod == 1) bracket_color = &d_keyword;
                 else bracket_color = &d_logical;
                 add_attr(attrs, cur, cur + 1, bracket_color);
                 paren_depth++;
                 cur++;
                 continue;
            }
            if (strchr(")]}", text[cur])) {
                 if (paren_depth > 0) paren_depth--;
                 const PangoColor *bracket_color;
                 int depth_mod = paren_depth % 3;
                 if (depth_mod == 0) bracket_color = &d_punctuation;
                 else if (depth_mod == 1) bracket_color = &d_keyword;
                 else bracket_color = &d_logical;
                 add_attr(attrs, cur, cur + 1, bracket_color);
                 
                 /* If closing param paren, switch state */
                 if (text[cur] == ')' && paren_depth == 0) {
                     state = STATE_ROOT;
                 }
                 cur++;
                 continue;
            }

            /* Default fallback for punctuation in params */
            if (strchr(".;", text[cur])) {
                add_attr(attrs, cur, cur + 1, &d_punctuation);
                cur++;
                continue;
            } else if (text[cur] == '-' && cur + 1 < len && text[cur+1] == '>') {
                add_attr(attrs, cur, cur + 2, &d_variable_c);
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
                        add_attr(attrs, m_start, m_end, &d_type);
                    } else {
                        add_attr(attrs, m_start, m_end, &d_variable);
                    }
                    /* We don't continue here; the loop will continue from cur (m_end) */
                }
                continue;
            } else if (text[cur] == '!' && (cur + 1 >= len || text[cur+1] != '=')) {
                add_attr(attrs, cur, cur + 1, &d_logical);
            } else if (text[cur] == '&' && cur + 1 < len && text[cur+1] == '&') {
                add_attr(attrs, cur, cur + 2, &d_logical);
                cur++;
            } else if (text[cur] == '|' && cur + 1 < len && text[cur+1] == '|') {
                add_attr(attrs, cur, cur + 2, &d_logical);
                cur++;
            } else {
                /* Arithmetic and Assignment Operators in params */
                gboolean handled = FALSE;
                if (cur + 1 < len) {
                    if ((text[cur] == '+' || text[cur] == '-' || text[cur] == '*' || 
                         text[cur] == '/' || text[cur] == '%' || text[cur] == '=' ||
                         text[cur] == '<' || text[cur] == '>' || text[cur] == '!' ||
                         text[cur] == '&' || text[cur] == '|' || text[cur] == '^') && text[cur+1] == '=') {
                        add_attr(attrs, cur, cur + 2, &d_keyword);
                        cur++;
                        handled = TRUE;
                    } else if (text[cur] == '<' && text[cur+1] == '<') {
                        add_attr(attrs, cur, cur + 2, &d_keyword);
                        cur++;
                        handled = TRUE;
                    } else if (text[cur] == '>' && text[cur+1] == '>') {
                        add_attr(attrs, cur, cur + 2, &d_keyword);
                        cur++;
                        handled = TRUE;
                    }
                }
                if (!handled && strchr("+-*/%=<>^|&?:", text[cur])) {
                    add_attr(attrs, cur, cur + 1, &d_keyword);
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
                     /* Format Specifiers: %... */
                     if (text[cur] == '%') {
                         /* Add attribute for string part before % */
                         if (cur > start_pos) {
                             add_attr(attrs, start_pos, cur, &d_string);
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
                             add_attr(attrs, fmt_start, cur, &d_number);
                         } else {
                             add_attr(attrs, fmt_start, cur, &d_string); 
                         }
                         start_pos = cur; /* Reset start for next string chunk */
                         continue;
                     }
                     cur++;
                }
                /* Add remaining string part */
                if (cur > start_pos) {
                    add_attr(attrs, start_pos, cur, &d_string);
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
                         if (cur > start_pos) add_attr(attrs, start_pos, cur, &d_string);
                         size_t esc_start = cur;
                         cur++;
                         if (cur < len) {
                             if (strchr("ntr0\\\"\'abfv?", text[cur])) cur++;
                             else if (text[cur] == 'x') { cur++; while (cur < len && g_ascii_isxdigit(text[cur])) cur++; }
                             else if (g_ascii_isdigit(text[cur])) { while (cur < len && g_ascii_isdigit(text[cur])) cur++; }
                             else cur++;
                         }
                         add_attr(attrs, esc_start, cur, &d_builtin);
                         start_pos = cur;
                         continue;
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
                /* Allow space between # and directive */
                while (cur < len && g_ascii_isspace(text[cur])) cur++;
                
                size_t directive_start = cur;
                while (cur < len && g_ascii_isalpha(text[cur])) {
                    cur++;
                }
                size_t directive_len = cur - directive_start;
                const char *directive = text + directive_start;

                add_attr(attrs, start_pos, cur, &d_preproc);

                if (directive_len == 7 && strncmp(directive, "include", 7) == 0) {
                    /* Skip whitespace */
                    while (cur < len && g_ascii_isspace(text[cur])) cur++;
                    
                    if (cur < len && (text[cur] == '"' || text[cur] == '<' || text[cur] == '\'')) {
                        char open = text[cur];
                        char close = (open == '<') ? '>' : open;
                        size_t path_start = cur;
                        cur++;
                        while (cur < len && text[cur] != close) cur++;
                        if (cur < len) cur++; /* include closer */
                        add_attr(attrs, path_start, cur, &d_string);
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
                        add_attr(attrs, macro_start, cur, &d_function);
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
                if (num_end > start_pos) add_attr(attrs, start_pos, num_end, &d_number);
                if (num_end < cur) add_attr(attrs, num_end, cur, &d_variable);
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
                gboolean is_type_list = is_word_in_list(word_start, word_len, glib_types) || 
                                      is_word_in_list(word_start, word_len, std_types);
                gboolean is_pointer_access = FALSE;
                
                /* Peek for -> */
                size_t p = cur;
                while (p < len && g_ascii_isspace(text[p])) p++;
                if (p + 1 < len && text[p] == '-' && text[p+1] == '>') is_pointer_access = TRUE;

                if (is_keyword) {
                    add_attr(attrs, start_pos, cur, &d_keyword);
                    if (word_len == 4 && strncmp(word_start, "enum", 4) == 0) {
                        state = STATE_C_ENUM_WAIT_LBRACE;
                    }
                } else if (is_func_call) {
                     add_attr(attrs, start_pos, cur, &d_function);
                     state = STATE_C_PARAMS;
                     paren_depth++; /* Entering params implies 1 level deeper conceptually or just track open paren */
                } else if (is_word_in_list(word_start, word_len, c_special_constants) ||
                           is_pointer_access || is_type_list ||
                           g_ascii_isupper(word_start[0]) || 
                           (word_len > 2 && word_start[word_len-1] == 't' && word_start[word_len-2] == '_')) {
                    add_attr(attrs, start_pos, cur, &d_type);
                } else if (is_all_caps(word_start, word_len)) {
                    add_attr(attrs, start_pos, cur, &d_constant);
                } else {
                    add_attr(attrs, start_pos, cur, &d_variable_c);
                }
                continue;
            }

            /* Operators and Punctuation */
            /* Rainbow Brackets */
            if (strchr("([{", text[cur])) {
                 const PangoColor *bracket_color;
                 int depth_mod = paren_depth % 3;
                 if (depth_mod == 0) bracket_color = &d_punctuation;
                 else if (depth_mod == 1) bracket_color = &d_keyword;
                 else bracket_color = &d_logical;
                 add_attr(attrs, cur, cur + 1, bracket_color);
                 paren_depth++;
                 cur++;
                 continue;
            }
            if (strchr(")]}", text[cur])) {
                 if (paren_depth > 0) paren_depth--;
                 const PangoColor *bracket_color;
                 int depth_mod = paren_depth % 3;
                 if (depth_mod == 0) bracket_color = &d_punctuation;
                 else if (depth_mod == 1) bracket_color = &d_keyword;
                 else bracket_color = &d_logical;
                 add_attr(attrs, cur, cur + 1, bracket_color);
                 cur++;
                 continue;
            }
            if (text[cur] == '-' && cur + 1 < len && text[cur+1] == '>') {
                add_attr(attrs, cur, cur + 2, &d_variable_c);
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
                        add_attr(attrs, m_start, m_end, &d_type);
                    } else {
                        add_attr(attrs, m_start, m_end, &d_variable);
                    }
                }
                continue;
            }
            if (text[cur] == '!' && (cur + 1 >= len || text[cur+1] != '=')) {
                add_attr(attrs, cur, cur + 1, &d_logical);
                cur++;
                continue;
            }
            if (text[cur] == '&' && cur + 1 < len && text[cur+1] == '&') {
                add_attr(attrs, cur, cur + 2, &d_logical);
                cur += 2;
                continue;
            }
            if (text[cur] == '|' && cur + 1 < len && text[cur+1] == '|') {
                add_attr(attrs, cur, cur + 2, &d_logical);
                cur += 2;
                continue;
            }
            
            /* Compound Operators */
            if (cur + 1 < len) {
                if ((text[cur] == '+' || text[cur] == '-' || text[cur] == '*' || 
                     text[cur] == '/' || text[cur] == '%' || text[cur] == '=' ||
                     text[cur] == '<' || text[cur] == '>' || text[cur] == '!' ||
                     text[cur] == '&' || text[cur] == '|' || text[cur] == '^') && text[cur+1] == '=') {
                    add_attr(attrs, cur, cur + 2, &d_keyword);
                    cur += 2;
                    continue;
                }
                if (text[cur] == '<' && text[cur+1] == '<') {
                    add_attr(attrs, cur, cur + 2, &d_keyword);
                    cur += 2;
                    continue;
                }
                if (text[cur] == '>' && text[cur+1] == '>') {
                    add_attr(attrs, cur, cur + 2, &d_keyword);
                    cur += 2;
                    continue;
                }
            }

            if (strchr("+-*/%=<>^|&?:", text[cur])) {
                add_attr(attrs, cur, cur + 1, &d_keyword);
                cur++;
                continue;
            }

            if (strchr(".;", text[cur])) {
                add_attr(attrs, cur, cur + 1, &d_punctuation);
                cur++;
                continue;
            }

            cur++;
        } else {
            /* Should not be here if STATE_IN_ML_COMMENT handled above */
             state = STATE_ROOT;
             cur++;
        }
    }
    
    set_line_end_state(ctx, line_index, state);
}
