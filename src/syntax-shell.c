#include "syntax-internal.h"
#include <string.h>

static const char *bash_keywords[] = {
    "if", "then", "else", "elif", "fi",
    "case", "esac",
    "for", "while", "until", "do", "done",
    "select", "in",
    "function",
    "time", "coproc",
    "break", "continue", "return",
    "export", "readonly", "declare", "local", "typeset",
    NULL
};

static const char *bash_builtins[] = {
    "cd", "pwd", "exit", "read", "printf", "echo",
    "test", "[", "]",
    "exec", "type", "hash", "help",
    "jobs", "fg", "bg", "disown", "wait",
    "unset", "set", "shopt", "basename", "realpath", "which",
    "zenity", "wget", "tar", "grep", "sed", "awk", "find", "ls", "rm", "cp", "mv",
    NULL
};

static const char *bash_booleans[] = {
    "true", "false", "TRUE", "FALSE", NULL
};

#if 0
static const char *bash_test_ops[] = {
    "-e", "-f", "-d", "-r", "-w", "-x",
    "-s", "-L", "-c", "-b",
    "-z", "-n",
    "-eq", "-ne", "-lt", "-le", "-gt", "-ge",
    NULL
};
#endif

static gboolean
match_word(const char *text, size_t start, size_t len, const char **list)
{
    for (int i = 0; list[i]; i++) {
        size_t w = strlen(list[i]);
        if (start + w <= len &&
            strncmp(text + start, list[i], w) == 0 &&
            (start + w == len ||
             (!g_ascii_isalnum(text[start + w]) && text[start + w] != '_')))
            return TRUE;
    }
    return FALSE;
}

#define BASH_PUSH(s) state = ((state & 0x0FFFFFFF) << 4) | (s)
#define BASH_POP()   state >>= 4
#define BASH_CUR     ((int)state & 0xF)

void
syntax_highlight_bash(SyntaxContext *ctx,
                      PangoAttrList *attrs,
                      const char *text,
                      size_t len,
                      SyntaxState state,
                      size_t line_index)
{
    size_t cur = 0;
    
    /* Persistent context from previous line. 
       We use a bitwise stack to handle nesting $(...) inside "" etc. 
       4 bits per level.
    */
    gboolean was_continued = (BASH_CUR == STATE_BASH_CONTINUATION);
    if (was_continued) BASH_POP();

    /* Check if we are at the start of a command */
    gboolean is_cmd_start = (BASH_CUR == STATE_ROOT || BASH_CUR == STATE_BASH_CMD_SUBST || BASH_CUR == STATE_SH_BACKTICK || BASH_CUR == STATE_BASH_CASE_BODY);
    if (was_continued) is_cmd_start = FALSE;

    gboolean in_case_patterns = (BASH_CUR == STATE_BASH_CASE);
    gboolean in_export_context = FALSE;
    int paren_depth = 0;

    while (cur < len) {
        int s = BASH_CUR;
        
        if (g_ascii_isspace(text[cur])) {
            cur++;
            continue;
        }

        if (s == STATE_IN_DOUBLE_QUOTE || s == STATE_IN_SINGLE_QUOTE) {
            char q = (s == STATE_IN_DOUBLE_QUOTE) ? '"' : '\'';
            while (cur < len) {
                if (text[cur] == '\\' && s == STATE_IN_DOUBLE_QUOTE) {
                    add_color_attr(ctx, attrs, cur, cur + 1, COLOR_LOGICAL);
                    cur++;
                    if (cur < len) cur++;
                    continue;
                }
                if (text[cur] == q) {
                    add_color_attr(ctx, attrs, cur, cur + 1, COLOR_STRING);
                    cur++;
                    BASH_POP();
                    is_cmd_start = FALSE;
                    goto next_loop;
                }
                if (s == STATE_IN_DOUBLE_QUOTE && text[cur] == '$' && cur + 1 < len) {
                    if (text[cur+1] == '(') {
                        add_color_attr(ctx, attrs, cur, cur + 2, COLOR_STRING); /* Green delimeter */
                        cur += 2;
                        BASH_PUSH(STATE_BASH_CMD_SUBST);
                        is_cmd_start = TRUE;
                        goto next_loop;
                    }
                    /* Simple variable interpolation */
                    size_t v_start = cur++;
                    if (text[cur] == '{') {
                        add_color_attr(ctx, attrs, v_start, v_start + 2, COLOR_STRING); /* ${ in Green */
                        cur++;
                        size_t var_content_start = cur;
                        while (cur < len && text[cur] != '}') cur++;
                        if (cur > var_content_start) add_color_attr(ctx, attrs, var_content_start, cur, COLOR_VARIABLE); /* XXX in Red */
                        if (cur < len) {
                            add_color_attr(ctx, attrs, cur, cur + 1, COLOR_STRING); /* } in Green */
                            cur++;
                        }
                    } else if (g_ascii_isdigit(text[cur]) || strchr("?#@$!*", text[cur])) {
                        cur++;
                        add_color_attr(ctx, attrs, v_start, cur, COLOR_TYPE);
                    } else if (g_ascii_isalpha(text[cur]) || text[cur] == '_') {
                        while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                        add_color_attr(ctx, attrs, v_start, cur, COLOR_VARIABLE);
                    }
                    continue;
                }
                if (s == STATE_IN_DOUBLE_QUOTE && text[cur] == '`') {
                    add_color_attr(ctx, attrs, cur, cur + 1, COLOR_STRING); /* Green delimeter */
                    cur++;
                    BASH_PUSH(STATE_SH_BACKTICK);
                    is_cmd_start = TRUE;
                    goto next_loop;
                }
                
                add_color_attr(ctx, attrs, cur, cur + 1, COLOR_STRING);
                cur++;
            }
            continue;
        }

        if (g_ascii_isspace(text[cur])) {
            cur++;
            continue;
        }

        if (text[cur] == '#') {
            add_color_attr(ctx, attrs, cur, len, COLOR_COMMENT);
            in_export_context = FALSE;
            break;
        }

        if (text[cur] == '\\') {
            add_color_attr(ctx, attrs, cur, cur + 1, COLOR_LOGICAL);
            cur++;
            gboolean is_eol_cont = TRUE;
            for (size_t i = cur; i < len; i++) if (!g_ascii_isspace(text[i])) { is_eol_cont = FALSE; break; }
            if (is_eol_cont) BASH_PUSH(STATE_BASH_CONTINUATION);
            continue;
        }

        /* End of command substitution */
        if (text[cur] == ')' && s == STATE_BASH_CMD_SUBST) {
            add_color_attr(ctx, attrs, cur, cur + 1, COLOR_STRING); /* Green delimeter */
            cur++;
            BASH_POP();
            is_cmd_start = FALSE;
            goto next_loop;
        }
        if (text[cur] == '`' && s == STATE_SH_BACKTICK) {
            add_color_attr(ctx, attrs, cur, cur + 1, COLOR_STRING); /* Green delimeter */
            cur++;
            BASH_POP();
            is_cmd_start = FALSE;
            goto next_loop;
        }

        /* Delimiters and operators */
        if (strchr(";&|()<>[]{}", text[cur])) {
            in_export_context = FALSE;
            if (cur + 1 < len && ((text[cur] == '&' && text[cur+1] == '&') || (text[cur] == '|' && text[cur+1] == '|'))) {
                add_color_attr(ctx, attrs, cur, cur + 2, COLOR_VARIABLE_C);
                cur += 2;
                is_cmd_start = TRUE;
            } else if (text[cur] == ';' && cur + 1 < len && text[cur+1] == ';') {
                add_color_attr(ctx, attrs, cur, cur + 2, COLOR_PUNCTUATION);
                cur += 2;
                if (BASH_CUR == STATE_BASH_CASE_BODY) {
                     BASH_POP(); /* Pop BODY, return to CASE (pattern) */
                }
                is_cmd_start = TRUE;
                in_case_patterns = TRUE;
            /* Rainbow Brackets: ( ) [ ] { } */
            } else if (strchr("()[]{}", text[cur])) {
                ViteColorSlot bracket_color = COLOR_PUNCTUATION;
                if (strchr("([{", text[cur])) {
                   int depth_mod = paren_depth % 3;
                   if (depth_mod == 0) bracket_color = COLOR_PUNCTUATION;      /* Orange */
                   else if (depth_mod == 1) bracket_color = COLOR_KEYWORD;     /* Purple */
                   else bracket_color = COLOR_LOGICAL;                         /* Cyan */
                   paren_depth++;
                } else {
                   if (paren_depth > 0) paren_depth--;
                   int depth_mod = paren_depth % 3;
                   if (depth_mod == 0) bracket_color = COLOR_PUNCTUATION;      /* Orange */
                   else if (depth_mod == 1) bracket_color = COLOR_KEYWORD;     /* Purple */
                   else bracket_color = COLOR_LOGICAL;                         /* Cyan */
                }
                
                add_color_attr(ctx, attrs, cur, cur + 1, bracket_color);
                
                /* Case statement `)` handling */
                if (text[cur] == ')' && BASH_CUR == STATE_BASH_CASE) {
                    BASH_POP();
                    BASH_PUSH(STATE_BASH_CASE_BODY); 
                    in_case_patterns = FALSE;
                    is_cmd_start = TRUE;
                }
                cur++;
                continue;
            } else {
                add_color_attr(ctx, attrs, cur, cur + 1, COLOR_VARIABLE_C);
                cur++;
                is_cmd_start = TRUE;
            }
            continue;
        }

        /* String start */
        if (text[cur] == '"' || text[cur] == '\'') {
            add_color_attr(ctx, attrs, cur, cur + 1, COLOR_STRING);
            BASH_PUSH((text[cur] == '"') ? STATE_IN_DOUBLE_QUOTE : STATE_IN_SINGLE_QUOTE);
            cur++;
            continue;
        }

        /* Words / Identifiers */
        if (g_ascii_isalnum(text[cur]) || text[cur] == '_' || text[cur] == '*' || text[cur] == '.' || text[cur] == '-' || text[cur] == '/' || text[cur] == '$') {
            size_t start = cur;

            /* Subshell start check first inside word-start block */
            if (text[cur] == '$' && cur + 1 < len && text[cur+1] == '(') {
                add_color_attr(ctx, attrs, cur, cur + 2, COLOR_STRING); /* Green delimeter */
                cur += 2;
                BASH_PUSH(STATE_BASH_CMD_SUBST);
                is_cmd_start = TRUE;
                goto next_loop;
            }

            if (text[cur] == '$') {
                cur++;
                if (cur < len && text[cur] == '{') {
                    add_color_attr(ctx, attrs, start, start + 2, COLOR_PUNCTUATION); /* ${ delimeter */
                    cur++;
                    size_t var_content_start = cur;
                    while (cur < len && text[cur] != '}') cur++;
                    if (cur > var_content_start) add_color_attr(ctx, attrs, var_content_start, cur, COLOR_VARIABLE); /* XXX in Red */
                    if (cur < len) {
                        add_color_attr(ctx, attrs, cur, cur + 1, COLOR_PUNCTUATION); /* } delimeter */
                        cur++;
                    }
                } else if (cur < len && (g_ascii_isdigit(text[cur]) || strchr("?#@$!*", text[cur]))) {
                    cur++;
                    add_color_attr(ctx, attrs, start, cur, COLOR_TYPE);
                } else {
                    while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                    add_color_attr(ctx, attrs, start, cur, COLOR_VARIABLE);
                }
                is_cmd_start = FALSE;
                continue;
            }

            while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_' || text[cur] == '*' || 
                                 text[cur] == '-' || text[cur] == '.' || text[cur] == '/' || text[cur] == '+')) {
                cur++;
            }
            size_t w_len = cur - start;

            if (text[start] == '-') {
                add_color_attr(ctx, attrs, start, cur, COLOR_NUMBER); /* Flags Orange */
                is_cmd_start = FALSE;
                continue;
            }

            if (in_case_patterns && text[start] != ';') {
                add_color_attr(ctx, attrs, start, cur, COLOR_VARIABLE);
                continue;
            }
            
            /* Assignment X=... or X+=... highlight X red */
            if (cur < len && text[cur] == '=') {
                size_t var_end = cur;
                if (var_end > start && text[var_end-1] == '+') var_end--;
                add_color_attr(ctx, attrs, start, var_end, COLOR_VARIABLE);
                add_color_attr(ctx, attrs, var_end, cur + 1, COLOR_LOGICAL);
                cur++;
                
                if (in_export_context) {
                    /* Highlight unquoted value in red */
                    if (cur < len && text[cur] != '"' && text[cur] != '\'') {
                        size_t val_start = cur;
                        while (cur < len && !g_ascii_isspace(text[cur]) && !strchr(";&|()<>", text[cur])) cur++;
                        if (cur > val_start) add_color_attr(ctx, attrs, val_start, cur, COLOR_VARIABLE);
                    }
                }
                
                is_cmd_start = FALSE;
                continue;
            }

            if (is_word_in_list(text + start, w_len, (const char*[]){"if", "then", "else", "elif", "fi", "for", "while", "do", "done", "case", "esac", "in", "function", NULL})) {
                add_color_attr(ctx, attrs, start, cur, COLOR_KEYWORD);
                if (strncmp(text + start, "in", 2) == 0) in_case_patterns = TRUE;
                if (strncmp(text + start, "case", 4) == 0) {
                     BASH_PUSH(STATE_BASH_CASE);
                }
                if (strncmp(text + start, "esac", 4) == 0) {
                     if (BASH_CUR == STATE_BASH_CASE || BASH_CUR == STATE_BASH_CASE_BODY) BASH_POP();
                }
                is_cmd_start = strchr("if then else elif do in", text[start]) != NULL;
                in_export_context = FALSE;
            } else if (match_word(text, start, len, bash_keywords)) {
                add_color_attr(ctx, attrs, start, cur, COLOR_KEYWORD);
                if (is_word_in_list(text + start, w_len, (const char*[]){"export", "local", "declare", "readonly", "typeset", NULL})) {
                    in_export_context = TRUE;
                }
                is_cmd_start = FALSE;
            } else if (match_word(text, start, len, bash_booleans)) {
                add_color_attr(ctx, attrs, start, cur, COLOR_NUMBER); /* Booleans stay Orange */
                is_cmd_start = FALSE;
            } else if (in_export_context) {
                add_color_attr(ctx, attrs, start, cur, COLOR_VARIABLE); /* Variables in export are Red */
                is_cmd_start = FALSE;
            } else if (is_cmd_start) {
                if (match_word(text, start, len, bash_builtins)) {
                    add_color_attr(ctx, attrs, start, cur, COLOR_LOGICAL); /* 1st word Builtin -> Cyan */
                } else {
                    add_color_attr(ctx, attrs, start, cur, COLOR_FUNCTION); /* 1st word command -> Blue */
                }
                is_cmd_start = FALSE;
            } else if (g_ascii_isdigit(text[start])) {
                add_color_attr(ctx, attrs, start, cur, COLOR_NUMBER);
                is_cmd_start = FALSE;
            } else {
                add_color_attr(ctx, attrs, start, cur, COLOR_STRING); /* Arguments -> Green */
            }
            continue;
        }

        if (text[cur] == '`') {
            add_color_attr(ctx, attrs, cur, cur + 1, COLOR_STRING);
            cur++;
            BASH_PUSH(STATE_SH_BACKTICK);
            is_cmd_start = TRUE;
            goto next_loop;
        }

        /* Fallback */
        add_color_attr(ctx, attrs, cur, cur + 1, COLOR_PUNCTUATION);
        cur++;

    next_loop: ;
    }

    set_line_end_state(ctx, line_index, (SyntaxState)state);
}
