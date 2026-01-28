#include "syntax-internal.h"
#include <string.h>

static const char *rust_keywords[] = {
    "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", 
    "enum", "extern", "false", "fn", "for", "if", "impl", "in", "let", "loop", 
    "match", "mod", "move", "mut", "pub", "ref", "return", 
    "static", "struct", "super", "trait", "true", "type", "unsafe", "use", 
    "where", "while", "abstract", "become", "box", "do", "final", "macro", 
    "override", "priv", "typeof", "unsized", "virtual", "yield", "try", "union",
    NULL
};

static const char *rust_types[] = {
    "bool", "char", "i8", "i16", "i32", "i64", "i128", "isize", 
    "u8", "u16", "u32", "u64", "u128", "usize", "f32", "f64", "str", 
    "String", "Vec", "Option", "Result", "Box", "Rc", "Arc", "Cell", "RefCell",
    "HashMap", "BTreeMap", "HashSet", "BTreeSet", "LinkedList", "BinaryHeap",
    NULL
};

static const char *rust_constants[] = {
    "true", "false", "Some", "None", "Ok", "Err", NULL
};

void 
syntax_highlight_rust(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index)
{
    size_t cur = 0;

    while (cur < len) {
        /* Nested Block Comments */
        if (state >= STATE_RUST_ML_COMMENT) {
            size_t start_pos = cur;
            while (cur + 1 < len) {
                 if (text[cur] == '/' && text[cur+1] == '*') {
                      state++;
                      cur += 2;
                      continue;
                 }
                 if (text[cur] == '*' && text[cur+1] == '/') {
                      state--;
                      cur += 2;
                      if (state < STATE_RUST_ML_COMMENT) {
                          state = STATE_ROOT;
                          break;
                      }
                      continue;
                 }
                 cur++;
            }
            if (state >= STATE_RUST_ML_COMMENT) cur = len; 
            add_attr(attrs, start_pos, cur, &d_comment);
            continue;
        }

        /* String State */
        if (state == STATE_IN_DOUBLE_QUOTE) {
            size_t start_pos = cur;
            while (cur < len) {
                if (text[cur] == '"') {
                    add_attr(attrs, start_pos, cur + 1, &d_string); 
                    cur++;
                    state = STATE_ROOT;
                    start_pos = cur; 
                    break;
                }
                if (text[cur] == '\\') {
                    add_attr(attrs, start_pos, cur, &d_string);
                    size_t esc_start = cur;
                    cur++;
                    if (cur < len) cur++; 
                    add_attr(attrs, esc_start, cur, &d_builtin); 
                    start_pos = cur;
                    continue; 
                }
                cur++;
            }
            if (state == STATE_IN_DOUBLE_QUOTE) {
                add_attr(attrs, start_pos, cur, &d_string);
            }
            continue;
        }

        if (state == STATE_ROOT) {
            /* Whitespace */
            if (g_ascii_isspace(text[cur])) {
                cur++;
                continue;
            }

            /* Comments */
            if (text[cur] == '/' && cur+1 < len) {
                if (text[cur+1] == '/') {
                    add_attr(attrs, cur, len, &d_comment);
                    cur = len;
                    continue;
                }
                if (text[cur+1] == '*') {
                    state = STATE_RUST_ML_COMMENT;
                    cur += 2;
                    add_attr(attrs, cur - 2, cur, &d_comment);
                    continue;
                }
            }

            /* Raw Strings r#... */
            if (text[cur] == 'r' && cur + 1 < len && (text[cur+1] == '#' || text[cur+1] == '"')) {
                if (text[cur+1] == '"' || (cur+2 < len && text[cur+1] == '#' && text[cur+2] == '"')) {
                     add_attr(attrs, cur, len, &d_string); 
                     cur = len; 
                     continue;
                }
            }

            /* Byte Strings b"..." */
            if (text[cur] == 'b' && cur + 1 < len && text[cur+1] == '"') {
                size_t start_pos = cur;
                cur += 2; 
                add_attr(attrs, start_pos, cur, &d_string); 
                state = STATE_IN_DOUBLE_QUOTE;
                continue;
            }

            /* Standard Strings "..." */
            if (text[cur] == '"') {
                size_t start_pos = cur;
                cur++;
                add_attr(attrs, start_pos, cur, &d_string); 
                state = STATE_IN_DOUBLE_QUOTE;
                continue;
            }

            /* Characters '...' */
            if (text[cur] == '\'') {
                size_t start_pos = cur;
                
                // Lifetime 'ident
                if (cur + 1 < len && (g_ascii_isalpha(text[cur+1]) || text[cur+1] == '_')) {
                     gboolean is_char = FALSE;
                     size_t p = cur + 1;
                     if (p < len && text[p] == '\\') { p+=2; } else { p++; } 
                     if (p < len && text[p] == '\'') { is_char = TRUE; }
                     
                     if (is_char) {
                         p = cur + 1;
                         if (text[p] == '\\') {
                              add_attr(attrs, start_pos, p, &d_string);
                              add_attr(attrs, p, p+2, &d_builtin); 
                              p += 2;
                              add_attr(attrs, p, p+1, &d_string); 
                              cur = p + 1;
                         } else {
                              cur = p + 1;
                              add_attr(attrs, start_pos, cur, &d_string);
                         }
                         continue;
                     } else {
                         // Lifetime -> Yellow (d_type)
                         cur++;
                         while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                         add_attr(attrs, start_pos, cur, &d_type); 
                         continue;
                     }
                } else if (cur + 1 < len && text[cur+1] == '\\') {
                    // Escaped char '\n'
                    size_t p = cur + 2;
                    if (p < len) p++; 
                    if (p < len && text[p] == '\'') {
                        add_attr(attrs, start_pos, cur+1, &d_string); 
                        add_attr(attrs, cur+1, cur+3, &d_builtin); 
                        add_attr(attrs, cur+3, cur+4, &d_string); 
                        cur = cur + 4;
                        continue;
                    }
                }
                
                add_attr(attrs, cur, cur + 1, &d_punctuation);
                cur++;
                continue;
            }

            /* Attributes #[...] or #![...] */
            if (text[cur] == '#' && cur + 1 < len && (text[cur+1] == '[' || text[cur+1] == '!')) {
                // Highlight #, [, ! as Grey
                add_attr(attrs, cur, cur+1, &d_variable_c); // #
                cur++;
                if (cur < len && text[cur] == '!') {
                     add_attr(attrs, cur, cur+1, &d_variable_c); // !
                     cur++;
                }
                // now at [
                if (cur < len && text[cur] == '[') {
                     add_attr(attrs, cur, cur+1, &d_variable_c); // [
                     cur++;
                     
                     // Parse inside attribute
                     while (cur < len && text[cur] != ']') {
                         if (g_ascii_isspace(text[cur])) {
                             cur++;
                             continue;
                         }
                         
                         // Punctuation inside attribute (parentheses, commas, equals) -> Grey
                         if (strchr("(),=", text[cur])) {
                             add_attr(attrs, cur, cur+1, &d_variable_c);
                             cur++;
                             continue;
                         }
                         
                         // String inside attribute -> Green
                         if (text[cur] == '"') {
                             size_t s_start = cur;
                             cur++;
                             while (cur < len && text[cur] != '"') {
                                 if (text[cur] == '\\') cur++;
                                 cur++;
                             }
                             if (cur < len) cur++;
                             add_attr(attrs, s_start, cur, &d_string);
                             continue;
                         }
                         
                         // Identifiers
                         if (g_ascii_isalpha(text[cur]) || text[cur] == '_') {
                             size_t id_start = cur;
                             while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                             
                             // Check context: are we inside parens? 
                             // Simplified: Treat all identifiers inside #[...] as Yellow (types/args) unless it's the very first one?
                             // Actually, user wants "derive" or "route" to be Grey, and inner args Yellow.
                             // But tracking "first" is hard in a simple loop without state.
                             // Heuristic: If followed by '(', it's likely the attribute name -> Grey.
                             // Else -> Yellow.
                             
                             size_t p = cur;
                             while (p < len && g_ascii_isspace(text[p])) p++;
                             if (p < len && text[p] == '(') {
                                 add_attr(attrs, id_start, cur, &d_variable_c); // Attribute Name (derive, route)
                             } else {
                                 // Check if it's a key in key="value". If next char is =, then Grey.
                                 if (p < len && text[p] == '=') {
                                     add_attr(attrs, id_start, cur, &d_variable_c); // Key
                                 } else {
                                     add_attr(attrs, id_start, cur, &d_type); // Value/Arg (Yellow)
                                 }
                             }
                             continue;
                         }
                         
                         cur++;
                     }
                     
                     // closing ]
                     if (cur < len && text[cur] == ']') {
                         add_attr(attrs, cur, cur+1, &d_variable_c);
                         cur++;
                     }
                     continue;
                }
            }

            /* Numbers */
            if (g_ascii_isdigit(text[cur])) {
                size_t start_pos = cur;
                if (text[cur] == '0' && cur+1 < len && strchr("xXbBoO", text[cur+1])) {
                    cur += 2;
                    while (cur < len && (g_ascii_isxdigit(text[cur]) || text[cur] == '_')) cur++;
                } else {
                    while (cur < len && (g_ascii_isdigit(text[cur]) || text[cur] == '_' || text[cur] == '.')) {
                         if (text[cur] == '.') {
                             if (cur+1 < len && (text[cur+1] == '.' || g_ascii_isalpha(text[cur+1]))) {
                                 break;
                             }
                         }
                         cur++;
                    }
                    if (cur < len && (text[cur] == 'e' || text[cur] == 'E')) {
                        cur++;
                        if (cur < len && (text[cur] == '+' || text[cur] == '-')) cur++;
                        while (cur < len && (g_ascii_isdigit(text[cur]) || text[cur] == '_')) cur++;
                    }
                }
                while (cur < len && (g_ascii_isalnum(text[cur]))) cur++; 
                
                add_attr(attrs, start_pos, cur, &d_number);
                continue;
            }

            /* Identifiers */
            if (g_ascii_isalpha(text[cur]) || text[cur] == '_') {
                size_t start_pos = cur;
                while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                size_t word_len = cur - start_pos;
                const char *word = text + start_pos;

                // Handle Self/self specifically
                if ((word_len == 4 && strncmp(word, "self", 4) == 0) || 
                    (word_len == 4 && strncmp(word, "Self", 4) == 0)) {
                    add_attr(attrs, start_pos, cur, &d_type);
                    continue;
                }

                if (cur < len && text[cur] == '!') {
                    cur++; 
                    add_attr(attrs, start_pos, cur, &d_function); 
                    continue;
                }
                
                size_t p = cur;
                while (p < len && g_ascii_isspace(text[p])) p++;
                if (p < len && text[p] == '(') {
                    if (is_word_in_list(word, word_len, rust_keywords)) {
                        add_attr(attrs, start_pos, cur, &d_keyword);
                    } else {
                        add_attr(attrs, start_pos, cur, &d_function);
                    }
                    continue;
                }

                if (is_word_in_list(word, word_len, rust_keywords)) {
                     add_attr(attrs, start_pos, cur, &d_keyword);
                } else if (is_word_in_list(word, word_len, rust_types)) {
                     add_attr(attrs, start_pos, cur, &d_type);
                } else if (is_word_in_list(word, word_len, rust_constants)) {
                     add_attr(attrs, start_pos, cur, &d_constant);
                } else if (g_ascii_isupper(word[0])) {
                     add_attr(attrs, start_pos, cur, &d_type);
                } else {
                     add_attr(attrs, start_pos, cur, &d_variable);
                }
                continue;
            }

            /* Punctuation / Operators */
            if (strchr("(){}[],.;", text[cur])) {
                add_attr(attrs, cur, cur + 1, &d_punctuation);
                cur++;
                continue;
            }
            if (strchr("+-*/%=&|<>!^:", text[cur])) {
                 if (cur + 1 < len) {
                      // Check for compound operators
                      if (text[cur+1] == '=' || (text[cur] == '-' && text[cur+1] == '>') || 
                          (text[cur] == ':' && text[cur+1] == ':') ||
                          (text[cur] == '&' && text[cur+1] == '&') || // &&
                          (text[cur] == '|' && text[cur+1] == '|')) { // ||
                          
                          // && and || -> Cyan
                          if ((text[cur] == '&' && text[cur+1] == '&') || 
                              (text[cur] == '|' && text[cur+1] == '|')) {
                              add_attr(attrs, cur, cur + 2, &d_logical);
                          } else {
                              // compound ops like ==, +=, etc -> Cyan
                              add_attr(attrs, cur, cur + 2, &d_logical);
                          }
                          cur += 2;
                          continue;
                      }
                 }
                 
                 // Single Char Operator
                 // Check for '&' -> Grey
                 if (text[cur] == '&') {
                     add_attr(attrs, cur, cur + 1, &d_variable_c); // Grey
                 } else {
                     add_attr(attrs, cur, cur + 1, &d_logical); // Cyan
                 }
                 cur++;
                 continue;
            }

            cur++;
        } else {
            // Unknown state fallthrough
            state = STATE_ROOT;
            cur++;
        }
    }

    set_line_end_state(ctx, line_index, state);
}
