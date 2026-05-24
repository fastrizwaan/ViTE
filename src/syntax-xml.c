#include "syntax-internal.h"
#include <string.h>

void 
syntax_highlight_xml(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index)
{
    /* Fast Path: State computation only (no attributes) */
    if (!attrs) {
        size_t cur = 0;
        while (cur < len) {
            if (state == STATE_IN_ML_COMMENT) {
                const char *found = strstr(text + cur, "-->");
                if (found) {
                    cur = (found - text) + 3;
                    state = STATE_ROOT;
                } else {
                    cur = len;
                }
                continue;
            }
            if (state == STATE_XML_CDATA) {
                const char *found = strstr(text + cur, "]]>");
                if (found) {
                    cur = (found - text) + 3;
                    state = STATE_ROOT;
                } else {
                    cur = len;
                }
                continue;
            }
            if (state == STATE_XML_TAG) {
                while (cur < len && text[cur] != '>') cur++;
                if (cur < len) {
                    state = STATE_ROOT;
                    cur++;
                }
                continue;
            }
            if (state == STATE_ROOT) {
                if (text[cur] == '<') {
                    if (cur + 3 < len && text[cur+1] == '!' && text[cur+2] == '-' && text[cur+3] == '-') {
                        state = STATE_IN_ML_COMMENT;
                        cur += 4;
                        continue;
                    }
                    if (cur + 8 < len && strncmp(text + cur, "<![CDATA[", 9) == 0) {
                        state = STATE_XML_CDATA;
                        cur += 9;
                        continue;
                    }
                    state = STATE_XML_TAG;
                    cur++;
                    continue;
                }
            }
            cur++;
        }
        set_line_end_state(ctx, line_index, state);
        return;
    }

    size_t cur = 0;
    while (cur < len) {
        if (state == STATE_IN_ML_COMMENT) {
            size_t start_pos = cur;
            while (cur + 2 < len) {
                if (text[cur] == '-' && text[cur+1] == '-' && text[cur+2] == '>') {
                    cur += 3;
                    state = STATE_ROOT;
                    break;
                }
                cur++;
            }
            if (state == STATE_IN_ML_COMMENT) cur = len;
            add_color_attr(ctx, attrs, start_pos, cur, COLOR_COMMENT);
            continue;
        }

        if (state == STATE_XML_CDATA) {
            size_t start_pos = cur;
            while (cur + 2 < len) {
                if (text[cur] == ']' && text[cur+1] == ']' && text[cur+2] == '>') {
                    cur += 3;
                    state = STATE_ROOT;
                    break;
                }
                cur++;
            }
            if (state == STATE_XML_CDATA) cur = len;
            add_color_attr(ctx, attrs, start_pos, cur, COLOR_STRING);
            continue;
        }

        if (state == STATE_XML_TAG) {
            /* Attributes and strings inside tag */
            while (cur < len && text[cur] != '>') {
                if (g_ascii_isspace(text[cur])) {
                    cur++;
                    continue;
                }
                if (text[cur] == '/') {
                    add_color_attr(ctx, attrs, cur, cur + 1, COLOR_TAG);
                    cur++;
                    continue;
                }
                if (text[cur] == '"' || text[cur] == '\'') {
                    char q = text[cur];
                    size_t str_start = cur;
                    cur++;
                    while (cur < len && text[cur] != q) cur++;
                    if (cur < len) cur++;
                    add_color_attr(ctx, attrs, str_start, cur, COLOR_STRING);
                    continue;
                }
                if (text[cur] == '=') {
                    add_color_attr(ctx, attrs, cur, cur + 1, COLOR_OPERATOR);
                    cur++;
                    continue;
                }
                if (g_ascii_isalpha(text[cur]) || text[cur] == '_' || text[cur] == ':') {
                    size_t attr_start = cur;
                    while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == ':' || text[cur] == '-' || text[cur] == '.' || text[cur] == '_')) {
                        cur++;
                    }
                    add_color_attr(ctx, attrs, attr_start, cur, COLOR_ATTRIBUTE);
                    continue;
                }
                cur++;
            }
            if (cur < len && text[cur] == '>') {
                add_color_attr(ctx, attrs, cur, cur + 1, COLOR_TAG);
                cur++;
                state = STATE_ROOT;
            }
            continue;
        }

        if (state == STATE_ROOT) {
            if (g_ascii_isspace(text[cur])) {
                cur++;
                continue;
            }
            if (text[cur] == '<' && cur + 3 < len && text[cur+1] == '!' && text[cur+2] == '-' && text[cur+3] == '-') {
                state = STATE_IN_ML_COMMENT;
                size_t start_pos = cur;
                cur += 4;
                while (cur + 2 < len) {
                    if (text[cur] == '-' && text[cur+1] == '-' && text[cur+2] == '>') {
                        cur += 3;
                        state = STATE_ROOT;
                        break;
                    }
                    cur++;
                }
                if (state == STATE_IN_ML_COMMENT) cur = len;
                add_color_attr(ctx, attrs, start_pos, cur, COLOR_COMMENT);
                continue;
            }

            if (text[cur] == '<' && cur + 8 < len && strncmp(text + cur, "<![CDATA[", 9) == 0) {
                state = STATE_XML_CDATA;
                size_t start_pos = cur;
                cur += 9;
                while (cur + 2 < len) {
                    if (text[cur] == ']' && text[cur+1] == ']' && text[cur+2] == '>') {
                        cur += 3;
                        state = STATE_ROOT;
                        break;
                    }
                    cur++;
                }
                if (state == STATE_XML_CDATA) cur = len;
                add_color_attr(ctx, attrs, start_pos, cur, COLOR_STRING);
                continue;
            }

            if (text[cur] == '<') {
                size_t tag_start = cur;
                cur++;
                if (cur < len && text[cur] == '/') cur++; /* </ */
                
                while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == ':' || text[cur] == '-' || text[cur] == '.' || text[cur] == '_')) {
                    cur++;
                }
                add_color_attr(ctx, attrs, tag_start, cur, COLOR_TAG);
                state = STATE_XML_TAG;
                continue;
            }

            cur++;
        }
    }
    
    set_line_end_state(ctx, line_index, state);
}
