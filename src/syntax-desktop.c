#include "syntax-internal.h"
#include <string.h>

void 
syntax_highlight_desktop(SyntaxContext *ctx, PangoAttrList *attrs, const char *text, size_t len, SyntaxState state, size_t line_index)
{
    GMatchInfo *mi;
    if (g_regex_match(ctx->desktop_comment, text, 0, &mi)) {
        while (g_match_info_matches(mi)) {
            int s, e;
            g_match_info_fetch_pos(mi, 0, &s, &e);
            add_attr(attrs, s, e, &d_comment);
            g_match_info_next(mi, NULL);
        }
    }
    g_match_info_free(mi);
    
    if (g_regex_match(ctx->desktop_section, text, 0, &mi)) {
        while (g_match_info_matches(mi)) {
            int s, e;
            g_match_info_fetch_pos(mi, 0, &s, &e);
            add_attr(attrs, s, e, &d_keyword); /* Keywords color for Section */
            g_match_info_next(mi, NULL);
        }
    }
    g_match_info_free(mi);
    
    if (g_regex_match(ctx->desktop_key, text, 0, &mi)) {
        while (g_match_info_matches(mi)) {
            int ks, ke;
            int full_s, full_e;
            g_match_info_fetch_pos(mi, 0, &full_s, &full_e);
            g_match_info_fetch_pos(mi, 1, &ks, &ke); /* Group 1 key */
            add_attr(attrs, ks, ke, &d_tag); /* Key -> Red */
            
            /* Highlight everything after the key match (=) as string (Green) */
            /* The regex matches "Key=", so full_e is after = */
            add_attr(attrs, full_e, len, &d_string); 
            
            g_match_info_next(mi, NULL);
        }
    }
    g_match_info_free(mi);
    
    if (g_regex_match(ctx->desktop_arg, text, 0, &mi)) {
        while (g_match_info_matches(mi)) {
            int s, e;
            g_match_info_fetch_pos(mi, 0, &s, &e);
            add_attr(attrs, s, e, &d_param); /* Argument -> Orange */
            g_match_info_next(mi, NULL);
        }
    }
    g_match_info_free(mi);
    
    /* Strings */
    if (g_regex_match(ctx->desktop_string_dq, text, 0, &mi)) {
        while (g_match_info_matches(mi)) {
            int s, e;
            g_match_info_fetch_pos(mi, 0, &s, &e);
            add_attr(attrs, s, e, &d_string);
            g_match_info_next(mi, NULL);
        }
    }
    g_match_info_free(mi);
    
    if (g_regex_match(ctx->desktop_string_sq, text, 0, &mi)) {
        while (g_match_info_matches(mi)) {
            int s, e;
            g_match_info_fetch_pos(mi, 0, &s, &e);
            add_attr(attrs, s, e, &d_string);
            g_match_info_next(mi, NULL);
        }
    }
    g_match_info_free(mi);

    set_line_end_state(ctx, line_index, state);
}
