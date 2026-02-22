#include "syntax-internal.h"
#include <string.h>

void 
syntax_highlight_yaml(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index)
{
    size_t cur = 0;
    gboolean key_scanned = FALSE;
    
    while (cur < len) {
        /* Whitespace */
        if (g_ascii_isspace(text[cur])) {
            cur++;
            continue;
        }
        
        /* Comment (Anywhere) */
        if (text[cur] == '#') {
            add_color_attr(attrs, cur, len, COLOR_COMMENT);
            cur = len;
            continue;
        }
        
        /* Key Scan (Find first colon not in quotes) */
        if (!key_scanned) {
            /* Look ahead for colon */
            size_t probe = cur;
            gboolean in_dq = FALSE;
            gboolean in_sq = FALSE;
            gboolean found_colon = FALSE;
            gboolean complex_key = FALSE;
            
            /* Initial check: does it look like a complex key start? ? */
            if (text[cur] == '?') complex_key = TRUE;
            
            while (probe < len) {
                if (text[probe] == '\\') {
                    probe += 2; continue;
                }
                if (text[probe] == '"' && !in_sq) in_dq = !in_dq;
                if (text[probe] == '\'' && !in_dq) in_sq = !in_sq;
                if (text[probe] == '#' && !in_dq && !in_sq) break; 
                if (text[probe] == ':' && !in_dq && !in_sq) {
                    found_colon = TRUE;
                    break;
                }
                probe++;
            }
            
            if (found_colon && !complex_key) {
                /* Highlight everything up to colon as Key (Red) */
                add_color_attr(attrs, cur, probe, COLOR_VARIABLE);
                cur = probe + 1; /* Skip colon */
                
                /* Highlight the colon itself as Punctuation (Orange) per request */
                add_color_attr(attrs, probe, probe + 1, COLOR_PUNCTUATION);
                
                key_scanned = TRUE;
                continue;
            } else {
                /* No colon found on this line, or it's a complex key. 
                   fall through to value/token parsing. */
                key_scanned = TRUE;
            }
        }
        
        /* Value / Token Parsing */
        
        /* Tag !!type */
        if (text[cur] == '!' && cur + 1 < len && text[cur+1] == '!') {
             size_t start = cur;
             cur += 2;
             while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
             add_color_attr(attrs, start, cur, COLOR_KEYWORD); /* Purple */
             continue;
        }
        
        /* Anchors & Aliases (* &) */
        if (text[cur] == '*' || text[cur] == '&') {
             if (cur+1 < len && !g_ascii_isspace(text[cur+1])) {
                 size_t start = cur;
                 cur++;
                 add_color_attr(attrs, start, cur, COLOR_KEYWORD); /* Prefix: Purple */
                 
                 size_t name_start = cur;
                 while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '-' || text[cur] == '_')) cur++;
                 add_color_attr(attrs, name_start, cur, COLOR_TYPE); /* Name: Yellow */
                 continue;
             }
        }
        
        /* Block Indicators (| >) */
        if (text[cur] == '|' || text[cur] == '>') {
             size_t start = cur;
             cur++;
             if (cur < len && (text[cur] == '-' || text[cur] == '+')) cur++;
             add_color_attr(attrs, start, cur, COLOR_KEYWORD); /* Purple */
             continue;
        }
        
        /* Brackets */
        if (strchr("[]{}()", text[cur])) {
             add_color_attr(attrs, cur, cur+1, COLOR_PUNCTUATION); /* Orange */
             cur++;
             continue;
        }
        
        /* Strings */
        if (text[cur] == '"') {
            size_t start_pos = cur;
            cur++; /* Skip open */
            add_color_attr(attrs, start_pos, cur, COLOR_STRING); 
            
            size_t seg_start = cur;
            while (cur < len) {
                if (text[cur] == '"') {
                    if (cur > seg_start) add_color_attr(attrs, seg_start, cur, COLOR_STRING);
                    add_color_attr(attrs, cur, cur+1, COLOR_STRING); /* Close quote */
                    cur++;
                    break;
                }
                
                /* Escapes */
                if (text[cur] == '\\') {
                    if (cur > seg_start) add_color_attr(attrs, seg_start, cur, COLOR_STRING);
                    size_t esc_end = cur + 1;
                    if (esc_end < len) {
                        char type = text[esc_end];
                        esc_end++; 
                        if (strchr("uUx", type)) {
                            int digits = 0;
                            int max = (type == 'u') ? 4 : (type == 'U') ? 8 : 2;
                            while (esc_end < len && g_ascii_isxdigit(text[esc_end]) && digits < max) {
                                esc_end++; digits++;
                            }
                        }
                    }
                    add_color_attr(attrs, cur, esc_end, COLOR_BUILTIN); /* Cyan */
                    cur = esc_end;
                    seg_start = cur;
                    continue;
                }
                
                /* Numbers inside String */
                if (g_ascii_isdigit(text[cur])) {
                     if (cur > seg_start) add_color_attr(attrs, seg_start, cur, COLOR_STRING);
                     
                     size_t num_start = cur;
                     while (cur < len && (g_ascii_isdigit(text[cur]) || text[cur] == '.')) cur++;
                     
                     /* Validate it looks like a number? User just said "numbers". 
                        "24.08" is digits and dots. */
                     add_color_attr(attrs, num_start, cur, COLOR_NUMBER); /* Orange */
                     seg_start = cur;
                     continue;
                }
                
                cur++;
            }
            if (cur >= len && text[cur-1] != '"') {
                if (cur > seg_start) add_color_attr(attrs, seg_start, cur, COLOR_STRING);
            }
            continue;
        }
        
        if (text[cur] == '\'') {
            size_t start_pos = cur;
            cur++;
            add_color_attr(attrs, start_pos, cur, COLOR_STRING);
            
            size_t seg_start = cur;
            while (cur < len) {
                if (text[cur] == '\'' && text[cur-1] != '\\') {
                    if (cur > seg_start) add_color_attr(attrs, seg_start, cur, COLOR_STRING);
                    add_color_attr(attrs, cur, cur+1, COLOR_STRING);
                    cur++;
                    break;
                }
                
                /* Numbers inside String */
                if (g_ascii_isdigit(text[cur])) {
                     if (cur > seg_start) add_color_attr(attrs, seg_start, cur, COLOR_STRING);
                     size_t num_start = cur;
                     while (cur < len && (g_ascii_isdigit(text[cur]) || text[cur] == '.')) cur++;
                     add_color_attr(attrs, num_start, cur, COLOR_NUMBER); /* Orange */
                     seg_start = cur;
                     continue;
                }
                
                cur++;
            }
            if (cur >= len && text[cur-1] != '\'') {
                  if (cur > seg_start) add_color_attr(attrs, seg_start, cur, COLOR_STRING);
            }
            continue;
        }
        
        /* Numbers */
        if (g_ascii_isdigit(text[cur]) || text[cur] == '-' || text[cur] == '.') {
            size_t start_pos = cur;
            size_t probe = cur;
            if (text[probe] == '-') probe++;
            
            gboolean is_number_special = FALSE;
            if (text[probe] == '.' && probe + 3 <= len) {
                if (strncmp(text+probe, ".nan", 4) == 0 || strncmp(text+probe, ".inf", 4) == 0)
                is_number_special = TRUE;
            }
            
            if (is_number_special) {
                while (probe < len && (g_ascii_isalnum(text[probe]) || text[probe] == '.')) probe++;
                add_color_attr(attrs, start_pos, probe, COLOR_NUMBER);
                cur = probe;
                continue;
            }
            
            if (probe < len && g_ascii_isdigit(text[probe])) {
                while (probe < len && (g_ascii_isalnum(text[probe]) || text[probe] == '.')) probe++;
                if (probe == len || g_ascii_isspace(text[probe]) || strchr("#,]}", text[probe])) {
                    add_color_attr(attrs, start_pos, probe, COLOR_NUMBER);
                    cur = probe;
                    continue;
                }
            }
        }
        
        /* Variable Substitution ${...} */
        if (text[cur] == '$' && cur + 1 < len && text[cur+1] == '{') {
            size_t start = cur;
            cur += 2;
            add_color_attr(attrs, start, cur, COLOR_PREPROC); /* ${ -> Purple */
            
            size_t var_start = cur;
            while (cur < len && text[cur] != '}') cur++;
            add_color_attr(attrs, var_start, cur, COLOR_VARIABLE); /* XXXX -> Red */
            
            if (cur < len) {
                add_color_attr(attrs, cur, cur+1, COLOR_PREPROC); /* } -> Purple */
                cur++; 
            }
            continue;
        }
        
        /* Keywords (Scalars) or Keys or Generic Unquoted Strings */
        /* Accept almost anything that isn't a delimiter as start of scalar */
        if (g_ascii_isalnum(text[cur]) || strchr("._-/+@=^%", text[cur])) {
            size_t start_pos = cur;
            
            /* Consume until delimiter */
            while (cur < len) {
                char c = text[cur];
                if (g_ascii_isspace(c)) break;
                if (strchr("[]{},:", c)) { 
                    /* Allow :// in scalars (URLs) */
                    if (c == ':' && cur + 2 < len && text[cur+1] == '/' && text[cur+2] == '/') {
                        cur++; continue;
                    }
                    
                     if (c == ':' || c == ',' || c == '}' || c == ']') break;
                }
                /* Also break on variable start */
                if (c == '$' && cur+1 < len && text[cur+1] == '{') break;
                
                cur++;
            }
            
            size_t word_len = cur - start_pos;
            
            /* Check if Key (followed by :) */
            gboolean is_key = FALSE;
            if (cur < len && text[cur] == ':') is_key = TRUE;
            
            gboolean delim = (cur == len || g_ascii_isspace(text[cur]) || strchr(":#,]}", text[cur]));
            
            if (is_key) {
                 add_color_attr(attrs, start_pos, cur, COLOR_VARIABLE); /* Red */
                 continue;
            }
            
            if (delim) {
                if ((word_len == 4 && strncmp(text+start_pos, "true", 4) == 0) ||
                    (word_len == 4 && strncmp(text+start_pos, "True", 4) == 0) ||
                    (word_len == 5 && strncmp(text+start_pos, "false", 5) == 0) ||
                    (word_len == 5 && strncmp(text+start_pos, "FALSE", 5) == 0) ||
                    (word_len == 4 && strncmp(text+start_pos, "null", 4) == 0) ||
                    (word_len == 1 && strncmp(text+start_pos, "~", 1) == 0)) {
                    add_color_attr(attrs, start_pos, cur, COLOR_NUMBER); /* Orange */
                } else {
                    add_color_attr(attrs, start_pos, cur, COLOR_STRING);
                }
            } else {
                 add_color_attr(attrs, start_pos, cur, COLOR_STRING);
            }
            continue;
        }
        
        /* Punctuation */
        if (text[cur] == ':' || text[cur] == '?' || text[cur] == '-' || text[cur] == ',') {
            add_color_attr(attrs, cur, cur+1, COLOR_PUNCTUATION);
            cur++;
            continue;
        }

        cur++;
    }
    
    set_line_end_state(ctx, line_index, state);
}
