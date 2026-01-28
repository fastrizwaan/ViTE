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
            add_attr(attrs, cur, len, &d_comment);
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
                add_attr(attrs, cur, probe, &d_variable);
                cur = probe + 1; /* Skip colon */
                
                /* Highlight the colon itself as Punctuation (Orange) per request */
                add_attr(attrs, probe, probe + 1, &d_punctuation);
                
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
             add_attr(attrs, start, cur, &d_keyword); /* Purple */
             continue;
        }
        
        /* Anchors & Aliases (* &) */
        if (text[cur] == '*' || text[cur] == '&') {
             if (cur+1 < len && !g_ascii_isspace(text[cur+1])) {
                 size_t start = cur;
                 cur++;
                 add_attr(attrs, start, cur, &d_keyword); /* Prefix: Purple */
                 
                 size_t name_start = cur;
                 while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '-' || text[cur] == '_')) cur++;
                 add_attr(attrs, name_start, cur, &d_type); /* Name: Yellow */
                 continue;
             }
        }
        
        /* Block Indicators (| >) */
        if (text[cur] == '|' || text[cur] == '>') {
             size_t start = cur;
             cur++;
             if (cur < len && (text[cur] == '-' || text[cur] == '+')) cur++;
             add_attr(attrs, start, cur, &d_keyword); /* Purple */
             continue;
        }
        
        /* Brackets */
        if (strchr("[]{}()", text[cur])) {
             add_attr(attrs, cur, cur+1, &d_punctuation); /* Orange */
             cur++;
             continue;
        }
        
        /* Strings */
        if (text[cur] == '"') {
            size_t start_pos = cur;
            cur++; /* Skip open */
            add_attr(attrs, start_pos, cur, &d_string); 
            
            size_t seg_start = cur;
            while (cur < len) {
                if (text[cur] == '"') {
                    if (cur > seg_start) add_attr(attrs, seg_start, cur, &d_string);
                    add_attr(attrs, cur, cur+1, &d_string); /* Close quote */
                    cur++;
                    break;
                }
                
                /* Escapes */
                if (text[cur] == '\\') {
                    if (cur > seg_start) add_attr(attrs, seg_start, cur, &d_string);
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
                    add_attr(attrs, cur, esc_end, &d_builtin); /* Cyan */
                    cur = esc_end;
                    seg_start = cur;
                    continue;
                }
                
                /* Numbers inside String */
                if (g_ascii_isdigit(text[cur])) {
                     if (cur > seg_start) add_attr(attrs, seg_start, cur, &d_string);
                     
                     size_t num_start = cur;
                     while (cur < len && (g_ascii_isdigit(text[cur]) || text[cur] == '.')) cur++;
                     
                     /* Validate it looks like a number? User just said "numbers". 
                        "24.08" is digits and dots. */
                     add_attr(attrs, num_start, cur, &d_number); /* Orange */
                     seg_start = cur;
                     continue;
                }
                
                cur++;
            }
            if (cur >= len && text[cur-1] != '"') {
                if (cur > seg_start) add_attr(attrs, seg_start, cur, &d_string);
            }
            continue;
        }
        
        if (text[cur] == '\'') {
            size_t start_pos = cur;
            cur++;
            add_attr(attrs, start_pos, cur, &d_string);
            
            size_t seg_start = cur;
            while (cur < len) {
                if (text[cur] == '\'' && text[cur-1] != '\\') {
                    if (cur > seg_start) add_attr(attrs, seg_start, cur, &d_string);
                    add_attr(attrs, cur, cur+1, &d_string);
                    cur++;
                    break;
                }
                
                /* Numbers inside String */
                if (g_ascii_isdigit(text[cur])) {
                     if (cur > seg_start) add_attr(attrs, seg_start, cur, &d_string);
                     size_t num_start = cur;
                     while (cur < len && (g_ascii_isdigit(text[cur]) || text[cur] == '.')) cur++;
                     add_attr(attrs, num_start, cur, &d_number); /* Orange */
                     seg_start = cur;
                     continue;
                }
                
                cur++;
            }
            if (cur >= len && text[cur-1] != '\'') {
                  if (cur > seg_start) add_attr(attrs, seg_start, cur, &d_string);
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
                if (strncmp(text+probe, ".nan", 4) == 0 || strncmp(text+probe, ".inf", 4) == 0) match:
                is_number_special = TRUE;
            }
            
            if (is_number_special) {
                while (probe < len && (g_ascii_isalnum(text[probe]) || text[probe] == '.')) probe++;
                add_attr(attrs, start_pos, probe, &d_number);
                cur = probe;
                continue;
            }
            
            if (probe < len && g_ascii_isdigit(text[probe])) {
                while (probe < len && (g_ascii_isalnum(text[probe]) || text[probe] == '.')) probe++;
                if (probe == len || g_ascii_isspace(text[probe]) || strchr("#,]}", text[probe])) {
                    add_attr(attrs, start_pos, probe, &d_number);
                    cur = probe;
                    continue;
                }
            }
        }
        
        /* Variable Substitution ${...} */
        if (text[cur] == '$' && cur + 1 < len && text[cur+1] == '{') {
            size_t start = cur;
            cur += 2;
            add_attr(attrs, start, cur, &d_preproc); /* ${ -> Purple */
            
            size_t var_start = cur;
            while (cur < len && text[cur] != '}') cur++;
            add_attr(attrs, var_start, cur, &d_variable); /* XXXX -> Red */
            
            if (cur < len) {
                add_attr(attrs, cur, cur+1, &d_preproc); /* } -> Purple */
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
                 add_attr(attrs, start_pos, cur, &d_variable); /* Red */
                 continue;
            }
            
            if (delim) {
                if ((word_len == 4 && strncmp(text+start_pos, "true", 4) == 0) ||
                    (word_len == 4 && strncmp(text+start_pos, "True", 4) == 0) ||
                    (word_len == 5 && strncmp(text+start_pos, "false", 5) == 0) ||
                    (word_len == 5 && strncmp(text+start_pos, "FALSE", 5) == 0) ||
                    (word_len == 4 && strncmp(text+start_pos, "null", 4) == 0) ||
                    (word_len == 1 && strncmp(text+start_pos, "~", 1) == 0)) {
                    add_attr(attrs, start_pos, cur, &d_number); /* Orange */
                } else {
                    add_attr(attrs, start_pos, cur, &d_string);
                }
            } else {
                 add_attr(attrs, start_pos, cur, &d_string);
            }
            continue;
        }
        
        /* Punctuation */
        if (text[cur] == ':' || text[cur] == '?' || text[cur] == '-' || text[cur] == ',') {
            add_attr(attrs, cur, cur+1, &d_punctuation);
            cur++;
            continue;
        }

        cur++;
    }
    
    set_line_end_state(ctx, line_index, state);
}
