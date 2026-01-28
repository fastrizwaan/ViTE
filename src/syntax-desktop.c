#include "syntax-internal.h"
#include <string.h>

void 
syntax_highlight_desktop(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index)
{
    size_t cur = 0;
    
    while (cur < len) {
        /* Whitespace */
        if (g_ascii_isspace(text[cur])) {
            cur++;
            continue;
        }
        
        /* Comment # */
        if (text[cur] == '#') {
            add_attr(attrs, cur, len, &d_comment);
            cur = len;
            continue;
        }
        
        /* Section Header [Section] */
        if (text[cur] == '[') {
            /* Check if it's a section line (starts with [) - usually matches regex ^\[...\] */
            /* We can enforce start of line logic if needed, but simplistic is fine */
            size_t start = cur;
            add_attr(attrs, cur, cur+1, &d_keyword); /* [ -> Purple */
            cur++;
            
            size_t name_start = cur;
            while (cur < len && text[cur] != ']') cur++;
            
            if (cur > name_start) {
                add_attr(attrs, name_start, cur, &d_builtin); /* Name -> Cyan */
            }
            
            if (cur < len && text[cur] == ']') {
                add_attr(attrs, cur, cur+1, &d_keyword); /* ] -> Purple */
                cur++;
            }
            continue;
        }
        
        /* Assigment Key=Val */
        /* Scan for = */
        size_t p = cur;
        while (p < len && text[p] != '=' && text[p] != '#' && text[p] != '[') p++;
        
        if (p < len && text[p] == '=') {
            /* Found key */
            add_attr(attrs, cur, p, &d_tag); /* Key -> Red (or Tag color) */
            
            /* Highlight = as Cyan */
            add_attr(attrs, p, p+1, &d_builtin); 
            
            cur = p + 1;
            
            /* Highlight Value */
            /* Value runs to end of line, but handle ; as Orange */
            size_t val_start = cur;
            while (cur < len) {
                if (text[cur] == ';') {
                    if (cur > val_start) add_attr(attrs, val_start, cur, &d_string);
                    add_attr(attrs, cur, cur+1, &d_punctuation); /* ; -> Orange */
                    cur++;
                    val_start = cur;
                } else {
                    cur++;
                }
            }
            if (cur > val_start) add_attr(attrs, val_start, cur, &d_string);
            continue;
        }
        
        /* Fallback */
        cur++;
    }

    set_line_end_state(ctx, line_index, state);
}
