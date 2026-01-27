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
            
            while (probe < len) {
                if (text[probe] == '\\') {
                    probe += 2; continue;
                }
                if (text[probe] == '"' && !in_sq) in_dq = !in_dq;
                if (text[probe] == '\'' && !in_dq) in_sq = !in_sq;
                if (text[probe] == '#' && !in_dq && !in_sq) break; /* Comment start, stop scanning */
                if (text[probe] == ':' && !in_dq && !in_sq) {
                    found_colon = TRUE;
                    break;
                }
                probe++;
            }
            
            if (found_colon) {
                /* Highlight everything up to colon as Key (Red) */
                add_attr(attrs, cur, probe, &d_variable);
                cur = probe + 1; /* Skip colon */
                key_scanned = TRUE;
                continue;
            } else {
                /* No colon found, treated as value item (e.g. list item) */
                key_scanned = TRUE;
                /* Fall through to value parsing */
            }
        }
        
        /* Value Parsing */
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
        
        /* Numbers */
        if (g_ascii_isdigit(text[cur]) || text[cur] == '-') {
            size_t start_pos = cur;
            /* Check if it's a number */
            size_t probe = cur;
            if (text[probe] == '-') probe++;
            gboolean is_num = FALSE;
            if (probe < len && g_ascii_isdigit(text[probe])) {
                while (probe < len && (g_ascii_isalnum(text[probe]) || text[probe] == '.')) probe++;
                /* If followed by space or end or comment */
                if (probe == len || g_ascii_isspace(text[probe]) || text[probe] == '#' || text[probe] == ',' || text[probe] == ']' || text[probe] == '}') {
                    is_num = TRUE;
                    add_attr(attrs, start_pos, probe, &d_number);
                    cur = probe;
                    continue;
                }
            }
        }
        
        /* Keywords (Scalars) */
        if (g_ascii_isalpha(text[cur])) {
            size_t start_pos = cur;
            while (cur < len && g_ascii_isalpha(text[cur])) cur++;
            size_t word_len = cur - start_pos;
            if ((word_len == 4 && strncmp(text+start_pos, "true", 4) == 0) ||
                (word_len == 5 && strncmp(text+start_pos, "false", 5) == 0) ||
                (word_len == 4 && strncmp(text+start_pos, "null", 4) == 0) ||
                (word_len == 3 && strncmp(text+start_pos, "yes", 3) == 0) ||
                (word_len == 2 && strncmp(text+start_pos, "no", 2) == 0)) {
                add_attr(attrs, start_pos, cur, &d_number);
            } else {
                 /* Plain value text - could be green (string) */
                 add_attr(attrs, start_pos, cur, &d_string);
            }
            continue;
        }
        
        cur++;
    }
    
    set_line_end_state(ctx, line_index, state);
}
