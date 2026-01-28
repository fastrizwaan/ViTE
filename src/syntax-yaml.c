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
                    add_attr(attrs, seg_start, cur, &d_string);
                    add_attr(attrs, cur, cur+1, &d_string);
                    cur++;
                    break;
                }
                if (text[cur] == '\\') {
                    add_attr(attrs, seg_start, cur, &d_string);
                    size_t esc_end = cur + 1;
                    if (esc_end < len) {
                        char type = text[esc_end];
                        esc_end++; /* Consume type char */
                        
                        /* Unicode / Hex Check */
                        if (type == 'u') {
                            /* \uXXXX */
                            int digits = 0;
                            while (esc_end < len && g_ascii_isxdigit(text[esc_end]) && digits < 4) {
                                esc_end++; digits++;
                            }
                        } else if (type == 'U') {
                            /* \UXXXXXXXX */
                            int digits = 0;
                            while (esc_end < len && g_ascii_isxdigit(text[esc_end]) && digits < 8) {
                                esc_end++; digits++;
                            }
                        } else if (type == 'x') {
                            /* \xXX */
                            int digits = 0;
                            while (esc_end < len && g_ascii_isxdigit(text[esc_end]) && digits < 2) {
                                esc_end++; digits++;
                            }
                        }
                    }
                    add_attr(attrs, cur, esc_end, &d_builtin); /* Cyan */
                    cur = esc_end;
                    seg_start = cur;
                    continue;
                }
                cur++;
            }
            if (cur >= len && text[cur-1] != '"') {
                add_attr(attrs, seg_start, cur, &d_string);
            }
            
            /* Check if this quoted string is a Key (followed by :) */
            size_t p = cur;
            while (p < len && g_ascii_isspace(text[p])) p++;
            if (p < len && text[p] == ':') {
                 /* Treat quoted identifier followed by colon as a Key (Red) if requested. 
                    User asked "a, b, c (XXX: YYY) XXX should be red". 
                    This usually implies unquoted identifiers in inline maps {a: 1}.
                    But let's leave quoted strings as Green for now unless user clarifies. */
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
                cur++;
            }
            add_attr(attrs, start_pos, cur, &d_string);
            continue;
        }
        
        /* Numbers */
        if (g_ascii_isdigit(text[cur]) || text[cur] == '-' || text[cur] == '.') {
            size_t start_pos = cur;
            size_t probe = cur;
            
            /* Check for leading sign */
            if (text[probe] == '-') probe++;
            
            /* Special check: ".nan", ".inf" */
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
                
                /* Delimiter check to confirm it's a value and not part of text */
                if (probe == len || g_ascii_isspace(text[probe]) || strchr("#,]}", text[probe])) {
                    add_attr(attrs, start_pos, probe, &d_number);
                    cur = probe;
                    continue;
                }
            }
        }
        
        /* Keywords (Scalars) or Keys */
        if (g_ascii_isalpha(text[cur]) || text[cur] == '~') {
            size_t start_pos = cur;
            if (text[cur] == '~') {
                 cur++;
            } else {
                 while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_' || text[cur] == '-')) cur++;
            }
            size_t word_len = cur - start_pos;
            
            /* Check if Key (followed by :) */
            size_t p = cur;
            /* Allow spaces before colon? "key : value" */
            /* In inline maps {key: value}, space is allowed. */
            /* Check if next significant char is ':' */
            /* Note: We shouldn't scan past newline or comments, but this loop is per line. */
             
            /* Check for colon */
            gboolean is_key = FALSE;
            if (p < len && text[p] == ':') {
                 is_key = TRUE;
            }
            
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
