#include "syntax-internal.h"
#include <string.h>

void
syntax_highlight_markdown(SyntaxContext *ctx,
                          PangoAttrList *attrs,
                          const char *text,
                          size_t len,
                          SyntaxState state,
                          size_t line_index)
{
    /* Simple Markdown Highlighter */
    /* State: 
       STATE_MD_CODE_BLOCK: Inside ``` block
       STATE_MD_BLOCK_QUOTE: Inside > block (not strictly stateful per line usually, but for consistency)
    */
    
    size_t cur = 0;
    
    /* Check previous state */
    gboolean in_code_block = (state == STATE_MD_CODE_BLOCK);

    /* Check for Code Block Start/End at line start */
    /* Only if it starts with ``` */
    if (len >= 3 && strncmp(text, "```", 3) == 0) {
        add_color_attr(attrs, 0, len, COLOR_STRING); /* Color fence green */
        if (in_code_block) {
            /* End of block */
            state = STATE_ROOT;
        } else {
            /* Start of block */
            state = STATE_MD_CODE_BLOCK;
        }
        set_line_end_state(ctx, line_index, state);
        return;
    }
    
    if (in_code_block) {
        add_color_attr(attrs, 0, len, COLOR_STRING); /* All content index code block is string color */
        set_line_end_state(ctx, line_index, STATE_MD_CODE_BLOCK);
        return;
    }
    
    /* Normal Line Processing */
    
    /* Headers: #, ##, ... */
    if (text[0] == '#') {
        size_t i = 1;
        while (i < len && text[i] == '#') i++;
        if (i <= 6 && (i == len || g_ascii_isspace(text[i]))) {
            /* It is a header */
            add_color_attr(attrs, 0, len, COLOR_KEYWORD); /* Header color */
            set_line_end_state(ctx, line_index, STATE_ROOT);
            return;
        }
    }
    
    /* Blockquotes: > */
    if (text[0] == '>') {
        /* Treat as comment color */
        add_color_attr(attrs, 0, len, COLOR_COMMENT);
        set_line_end_state(ctx, line_index, STATE_ROOT);
        return;
    }
    
    /* Horizontal Rule: --- or *** or ___ */
    /* check for only these chars */
    /* Simplification: if starts with --- and only contains - or space */
    if (len >= 3 && (strncmp(text, "---", 3) == 0 || strncmp(text, "***", 3) == 0 || strncmp(text, "___", 3) == 0)) {
       gboolean is_hr = TRUE;
       for (size_t k = 0; k < len; k++) {
           if (!g_ascii_isspace(text[k]) && text[k] != '-' && text[k] != '*' && text[k] != '_') {
               is_hr = FALSE; break;
           }
       }
       if (is_hr) {
           add_color_attr(attrs, 0, len, COLOR_COMMENT);
           set_line_end_state(ctx, line_index, STATE_ROOT);
           return;
       }
    }

    /* Inline scanning */
    while (cur < len) {
        /* Bold/Italic: ** or __ or * or _ */
        /* Link: [text](url) */
        /* Code: `...` */
        /* Image: ![...](...) */
        
        if (text[cur] == '`') {
            size_t start = cur;
            cur++;
            while (cur < len && text[cur] != '`') cur++;
            if (cur < len) {
                cur++; /* consume closing ` */
                add_color_attr(attrs, start, cur, COLOR_STRING);
            }
            continue;
        }
        
        if (text[cur] == '[') {
            size_t link_text_start = cur;
            cur++;
            while (cur < len && text[cur] != ']') cur++;
            if (cur < len) {
                /* Found closing bracket */
                cur++;
                add_color_attr(attrs, link_text_start, cur, COLOR_FUNCTION); /* Link Text Blue */
                
                if (cur < len && text[cur] == '(') {
                    size_t url_start = cur;
                    cur++;
                    while (cur < len && text[cur] != ')') cur++;
                    if (cur < len) {
                        cur++;
                        add_color_attr(attrs, url_start, cur, COLOR_COMMENT); /* URL Grey */
                    }
                }
            }
            continue;
        }
        
        /* List markers: - * + at start (already handled mostly by default color if not specialized) 
           But let's color them operator color if at start of line (allowing whitespace)
        */
        if ((text[cur] == '-' || text[cur] == '*' || text[cur] == '+') && 
            (cur == 0 || g_ascii_isspace(text[cur-1])) &&
            (cur + 1 < len && g_ascii_isspace(text[cur+1]))) {
            add_color_attr(attrs, cur, cur+1, COLOR_OPERATOR);
            cur++;
            continue;
        }
        
        /* Digits followed by dot at start: 1. */
        if (g_ascii_isdigit(text[cur]) && (cur == 0 || g_ascii_isspace(text[cur-1]))) {
             size_t num_start = cur;
             while (cur < len && g_ascii_isdigit(text[cur])) cur++;
             if (cur < len && text[cur] == '.') {
                 add_color_attr(attrs, num_start, cur+1, COLOR_OPERATOR);
                 cur++;
             }
             continue;
        }
        
        /* Bold/Italic markers (Basic) */
        if (strncmp(text + cur, "**", 2) == 0 || strncmp(text + cur, "__", 2) == 0) {
            add_color_attr(attrs, cur, cur+2, COLOR_TYPE); /* Gold/Yellow for bold markers */
            cur += 2;
            continue;
        }
        /* Italic markers can be tricky due to _ in words. 
           Let's just highlight the stars/underscores themselves for now if they act as markers.
        */

        cur++;
    }
    
    set_line_end_state(ctx, line_index, STATE_ROOT);
}
