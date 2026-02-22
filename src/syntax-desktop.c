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
            add_color_attr(attrs, cur, len, COLOR_COMMENT);
            cur = len;
            continue;
        }
        
        /* Section Header [Section] */
        if (text[cur] == '[') {
            /* Check if it's a section line (starts with [) - usually matches regex ^\[...\] */
            /* We can enforce start of line logic if needed, but simplistic is fine */
            add_color_attr(attrs, cur, cur+1, COLOR_NUMBER); /* [ -> orange */
            cur++;
            
            size_t name_start = cur;
            while (cur < len && text[cur] != ']') cur++;
            
            if (cur > name_start) {
                add_color_attr(attrs, name_start, cur, COLOR_FUNCTION); /* Name -> Cyan */
            }
            
            if (cur < len && text[cur] == ']') {
                add_color_attr(attrs, cur, cur+1, COLOR_NUMBER); /* ] -> orange */
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
            add_color_attr(attrs, cur, p, COLOR_TAG); /* Key -> Red (or Tag color) */
            
            /* Highlight = as Cyan */
            add_color_attr(attrs, p, p+1, COLOR_BUILTIN); 
            
            cur = p + 1;
            
            /* Highlight Value */
            /* Value runs to end of line, but handle ; as Orange */
            size_t val_start = cur;
            while (cur < len) {
                if (text[cur] == ';') {
                    if (cur > val_start) add_color_attr(attrs, val_start, cur, COLOR_STRING);
                    add_color_attr(attrs, cur, cur+1, COLOR_PUNCTUATION); /* ; -> Orange */
                    cur++;
                    val_start = cur;
                } else {
                    cur++;
                }
            }
            if (cur > val_start) add_color_attr(attrs, val_start, cur, COLOR_STRING);
            continue;
        }
        
        /* Fallback */
        cur++;
    }

    set_line_end_state(ctx, line_index, state);
}
