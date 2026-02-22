#include "syntax-internal.h"
#include <string.h>

void 
syntax_highlight_xml(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index)
{
    GMatchInfo *mi;
    /* Comments */
    if (g_regex_match(ctx->xml_comment_start, text, 0, &mi)) {
         while (g_match_info_matches(mi)) {
             int s, e;
             g_match_info_fetch_pos(mi, 0, &s, &e);
             add_color_attr(attrs, s, len, COLOR_COMMENT); /* Assume single line or until end */
             /* TODO: Proper state machine for XML comments */
             state = STATE_IN_ML_COMMENT;
             g_match_info_next(mi, NULL);
         }
    }
    g_match_info_free(mi);
    
    /* Tags */
    if (state == STATE_ROOT) {
        if (g_regex_match(ctx->xml_tag_open, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_color_attr(attrs, s, e, COLOR_TAG); /* Red tags */
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        if (g_regex_match(ctx->xml_tag_close, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_color_attr(attrs, s, e, COLOR_TAG);
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        /* Attributes */
        if (g_regex_match(ctx->xml_attr, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 1, &s, &e); /* Group 1 name */
                add_color_attr(attrs, s, e, COLOR_ATTRIBUTE); /* Orange attributes */
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        
        /* Strings (values) - Simple regex */
        GRegex *dq = g_regex_new("\"[^\"]*\"", G_REGEX_OPTIMIZE, 0, NULL);
        if (g_regex_match(dq, text, 0, &mi)) {
            while (g_match_info_matches(mi)) {
                int s, e;
                g_match_info_fetch_pos(mi, 0, &s, &e);
                add_color_attr(attrs, s, e, COLOR_STRING);
                g_match_info_next(mi, NULL);
            }
        }
        g_match_info_free(mi);
        g_regex_unref(dq);
    }
    
    set_line_end_state(ctx, line_index, state);
}
