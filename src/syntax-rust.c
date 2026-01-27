#include "syntax-internal.h"
#include <string.h>

static const char *rust_keywords[] = {
    "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", 
    "enum", "extern", "false", "fn", "for", "if", "impl", "in", "let", "loop", 
    "match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self", 
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
        if (state == STATE_RUST_ML_COMMENT) {
            size_t start_pos = cur;
            while (cur + 1 < len) {
                 if (text[cur] == '*' && text[cur+1] == '/') {
                      cur += 2;
                      state = STATE_ROOT;
                      break;
                 }
                 /* Handle nested comments? Simple depth tracking is hard with single state enum */
                 cur++;
            }
            if (state == STATE_RUST_ML_COMMENT) cur = len;
            add_attr(attrs, start_pos, cur, &d_comment);
            continue;
        }

        /* Generic String State (Reuse generic states for simplicity) */
        if (state == STATE_IN_DOUBLE_QUOTE) {
            size_t start_pos = cur;
            while (cur < len) {
                if (text[cur] == '"' && (cur == 0 || text[cur-1] != '\\')) {
                    cur++;
                    state = STATE_ROOT;
                    break;
                }
                if (text[cur] == '\\') {
                    cur++;
                    if (cur < len) cur++;
                    continue; 
                }
                cur++;
            }
            add_attr(attrs, start_pos, cur, &d_string);
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
                    if (state == STATE_RUST_ML_COMMENT) cur = len;
                    add_attr(attrs, start_pos, cur, &d_comment);
                    continue;
                }
            }

            /* Raw Strings r#... */
            if (text[cur] == 'r' && cur + 1 < len && (text[cur+1] == '#' || text[cur+1] == '"')) {
                // Simplified raw string: treat as string if starts with r" or r#"
                // Full r#...# support is complex, treat as normal string for now start
                if (text[cur+1] == '"' || (cur+2 < len && text[cur+1] == '#' && text[cur+2] == '"')) {
                     size_t start_pos = cur;
                     add_attr(attrs, cur, len, &d_string); // Fallback: highlight rest as string for safety
                     cur = len; 
                     continue;
                }
            }

            /* Byte Strings b"..." */
            if (text[cur] == 'b' && cur + 1 < len && text[cur+1] == '"') {
                size_t start_pos = cur;
                cur += 2; /* Skip b" */
                state = STATE_IN_DOUBLE_QUOTE;
                /* Process remainder as string */
                while (cur < len) {
                    if (text[cur] == '"' && text[cur-1] != '\\') {
                        cur++;
                        state = STATE_ROOT;
                        break;
                    }
                    if (text[cur] == '\\') {
                        cur++;
                        if (cur < len) cur++;
                        continue;
                    }
                    cur++;
                }
                add_attr(attrs, start_pos, cur, &d_string);
                continue;
            }

            /* Standard Strings "..." */
            if (text[cur] == '"') {
                size_t start_pos = cur;
                cur++;
                state = STATE_IN_DOUBLE_QUOTE;
                while (cur < len) {
                    if (text[cur] == '"' && text[cur-1] != '\\') {
                        cur++;
                        state = STATE_ROOT;
                        break;
                    }
                    if (text[cur] == '\\') {
                        // Escape sequences
                        size_t esc_start = cur;
                        cur++;
                        if (cur < len) {
                            if (text[cur] == 'u' && cur+1 < len && text[cur+1] == '{') {
                                // Unicode \u{...}
                                cur += 2;
                                while(cur < len && text[cur] != '}') cur++;
                                if (cur < len) cur++;
                            } else {
                                cur++;
                            }
                        }
                        add_attr(attrs, esc_start, cur, &d_builtin); // Highlight escapes
                        continue;
                    }
                    cur++;
                }
                add_attr(attrs, start_pos, cur, &d_string);
                continue;
            }

            /* Characters '...' */
            /* Care must be taken to distinguish 'lifetime vs 'char' */
            if (text[cur] == '\'') {
                size_t start_pos = cur;
                
                // Check if it's a lifetime: 'ident
                if (cur + 1 < len && (g_ascii_isalpha(text[cur+1]) || text[cur+1] == '_')) {
                     // Could be lifetime OR char 'a'
                     // Heuristic: if char, it ends with '
                     
                     // Look ahead for '
                     gboolean is_char = FALSE;
                     size_t p = cur + 1;
                     if (p < len && text[p] == '\\') { p+=2; } else { p++; } // simple char
                     if (p < len && text[p] == '\'') { is_char = TRUE; }
                     
                     if (is_char) {
                         cur = p + 1;
                         add_attr(attrs, start_pos, cur, &d_string);
                         continue;
                     } else {
                         // Lifetime
                         cur++;
                         while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                         add_attr(attrs, start_pos, cur, &d_decorator); // Using decorator color for lifetimes
                         continue;
                     }
                } else if (cur + 1 < len && text[cur+1] == '\\') {
                    // Escaped char '\n'
                    size_t p = cur + 2;
                    if (p < len) p++; // skip escaped char
                    if (p < len && text[p] == '\'') {
                        cur = p + 1;
                        add_attr(attrs, start_pos, cur, &d_string);
                        continue;
                    }
                }
                
                // Fallback
                cur++;
                add_attr(attrs, start_pos, cur, &d_punctuation);
                continue;
            }

            /* Attributes #[...] or #![...] */
            if (text[cur] == '#' && cur + 1 < len && (text[cur+1] == '[' || text[cur+1] == '!')) {
                size_t start_pos = cur;
                cur++;
                if (cur < len && text[cur] == '!') cur++;
                if (cur < len && text[cur] == '[') {
                    // Consume until ]
                     while (cur < len && text[cur] != ']') cur++;
                     if (cur < len) cur++;
                     add_attr(attrs, start_pos, cur, &d_preproc);
                     continue;
                }
            }

            /* Numbers */
            if (g_ascii_isdigit(text[cur])) {
                size_t start_pos = cur;
                // Hex 0x, Binary 0b, Octal 0o
                if (text[cur] == '0' && cur+1 < len && strchr("xXbBoO", text[cur+1])) {
                    cur += 2;
                    while (cur < len && (g_ascii_isxdigit(text[cur]) || text[cur] == '_')) cur++;
                } else {
                    // Decimal / Float
                    while (cur < len && (g_ascii_isdigit(text[cur]) || text[cur] == '_' || text[cur] == '.')) {
                         // Don't consume . if it's method call or range ..
                         if (text[cur] == '.') {
                             if (cur+1 < len && (text[cur+1] == '.' || g_ascii_isalpha(text[cur+1]))) {
                                 break;
                             }
                         }
                         cur++;
                    }
                    // Exponent
                    if (cur < len && (text[cur] == 'e' || text[cur] == 'E')) {
                        cur++;
                        if (cur < len && (text[cur] == '+' || text[cur] == '-')) cur++;
                        while (cur < len && (g_ascii_isdigit(text[cur]) || text[cur] == '_')) cur++;
                    }
                }
                // Suffixes inside number (u8, f32 etc)? Usually separate or attached.
                while (cur < len && (g_ascii_isalnum(text[cur]))) cur++; // Consume type suffix like 10u8
                
                add_attr(attrs, start_pos, cur, &d_number);
                continue;
            }

            /* Identifiers */
            if (g_ascii_isalpha(text[cur]) || text[cur] == '_') {
                size_t start_pos = cur;
                while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                size_t word_len = cur - start_pos;
                const char *word = text + start_pos;

                // Check for macro !
                if (cur < len && text[cur] == '!') {
                    cur++; // Include ! in macro name
                    add_attr(attrs, start_pos, cur, &d_function); // Macros -> function color
                    continue;
                }
                
                 // Check function call ( ident ( )
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
                     // Capitalized -> usually Type or EnumVariant
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
                 // Check for compound like ==, !=, >=, <=, etc.
                 if (cur + 1 < len) {
                      if (text[cur+1] == '=' || (text[cur] == '-' && text[cur+1] == '>') || 
                          (text[cur] == ':' && text[cur+1] == ':')) {
                          add_attr(attrs, cur, cur + 2, &d_operator);
                          cur += 2;
                          continue;
                      }
                 }
                 add_attr(attrs, cur, cur + 1, &d_operator);
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
