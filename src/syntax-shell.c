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
                    add_attr(attrs, cur, cur + 1, &d_logical);
                    cur++;
                    if (cur < len) cur++;
                    continue;
                }
                if (text[cur] == q) {
                    add_attr(attrs, cur, cur + 1, &d_string);
                    cur++;
                    BASH_POP();
                    is_cmd_start = FALSE;
                    goto next_loop;
                }
                if (s == STATE_IN_DOUBLE_QUOTE && text[cur] == '$' && cur + 1 < len) {
                    if (text[cur+1] == '(') {
                        add_attr(attrs, cur, cur + 2, &d_string); /* Green delimeter */
                        cur += 2;
                        BASH_PUSH(STATE_BASH_CMD_SUBST);
                        is_cmd_start = TRUE;
                        goto next_loop;
                    }
                    /* Simple variable interpolation */
                    size_t v_start = cur++;
                    if (text[cur] == '{') {
                        add_attr(attrs, v_start, v_start + 2, &d_string); /* ${ in Green */
                        cur++;
                        size_t var_content_start = cur;
                        while (cur < len && text[cur] != '}') cur++;
                        if (cur > var_content_start) add_attr(attrs, var_content_start, cur, &d_variable); /* XXX in Red */
                        if (cur < len) {
                            add_attr(attrs, cur, cur + 1, &d_string); /* } in Green */
                            cur++;
                        }
                    } else if (g_ascii_isdigit(text[cur]) || strchr("?#@$!*", text[cur])) {
                        cur++;
                        add_attr(attrs, v_start, cur, &d_type);
                    } else if (g_ascii_isalpha(text[cur]) || text[cur] == '_') {
                        while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                        add_attr(attrs, v_start, cur, &d_variable);
                    }
                    continue;
                }
                if (s == STATE_IN_DOUBLE_QUOTE && text[cur] == '`') {
                    add_attr(attrs, cur, cur + 1, &d_string); /* Green delimeter */
                    cur++;
                    BASH_PUSH(STATE_SH_BACKTICK);
                    is_cmd_start = TRUE;
                    goto next_loop;
                }
                
                add_attr(attrs, cur, cur + 1, &d_string);
                cur++;
            }
            continue;
        }

        if (g_ascii_isspace(text[cur])) {
            cur++;
            continue;
        }

        if (text[cur] == '#') {
            add_attr(attrs, cur, len, &d_comment);
            in_export_context = FALSE;
            break;
        }

        if (text[cur] == '\\') {
            add_attr(attrs, cur, cur + 1, &d_logical);
            cur++;
            gboolean is_eol_cont = TRUE;
            for (size_t i = cur; i < len; i++) if (!g_ascii_isspace(text[i])) { is_eol_cont = FALSE; break; }
            if (is_eol_cont) BASH_PUSH(STATE_BASH_CONTINUATION);
            continue;
        }

        /* End of command substitution */
        if (text[cur] == ')' && s == STATE_BASH_CMD_SUBST) {
            add_attr(attrs, cur, cur + 1, &d_string); /* Green delimeter */
            cur++;
            BASH_POP();
            is_cmd_start = FALSE;
            goto next_loop;
        }
        if (text[cur] == '`' && s == STATE_SH_BACKTICK) {
            add_attr(attrs, cur, cur + 1, &d_string); /* Green delimeter */
            cur++;
            BASH_POP();
            is_cmd_start = FALSE;
            goto next_loop;
        }

        /* Delimiters and operators */
        if (strchr(";&|()<>[]{}", text[cur])) {
            in_export_context = FALSE;
            if (cur + 1 < len && ((text[cur] == '&' && text[cur+1] == '&') || (text[cur] == '|' && text[cur+1] == '|'))) {
                add_attr(attrs, cur, cur + 2, &d_variable_c);
                cur += 2;
                is_cmd_start = TRUE;
            } else if (text[cur] == ';' && cur + 1 < len && text[cur+1] == ';') {
                add_attr(attrs, cur, cur + 2, &d_punctuation);
                cur += 2;
                if (BASH_CUR == STATE_BASH_CASE_BODY) {
                     BASH_POP(); /* Pop BODY, return to CASE (pattern) */
                }
                is_cmd_start = TRUE;
                in_case_patterns = TRUE;
            /* Rainbow Brackets: ( ) [ ] { } */
            } else if (strchr("()[]{}", text[cur])) {
                const PangoColor *bracket_color = &d_punctuation;
                if (strchr("([{", text[cur])) {
                   int depth_mod = paren_depth % 3;
                   if (depth_mod == 0) bracket_color = &d_punctuation;      /* Orange */
                   else if (depth_mod == 1) bracket_color = &d_keyword;     /* Purple */
                   else bracket_color = &d_logical;                         /* Cyan */
                   paren_depth++;
                } else {
                   if (paren_depth > 0) paren_depth--;
                   int depth_mod = paren_depth % 3;
                   if (depth_mod == 0) bracket_color = &d_punctuation;      /* Orange */
                   else if (depth_mod == 1) bracket_color = &d_keyword;     /* Purple */
                   else bracket_color = &d_logical;                         /* Cyan */
                }
                
                add_attr(attrs, cur, cur + 1, bracket_color);
                
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
                add_attr(attrs, cur, cur + 1, &d_variable_c);
                cur++;
                is_cmd_start = TRUE;
            }
            continue;
        }

        /* String start */
        if (text[cur] == '"' || text[cur] == '\'') {
            add_attr(attrs, cur, cur + 1, &d_string);
            BASH_PUSH((text[cur] == '"') ? STATE_IN_DOUBLE_QUOTE : STATE_IN_SINGLE_QUOTE);
            cur++;
            continue;
        }

        /* Words / Identifiers */
        if (g_ascii_isalnum(text[cur]) || text[cur] == '_' || text[cur] == '*' || text[cur] == '.' || text[cur] == '-' || text[cur] == '/' || text[cur] == '$') {
            size_t start = cur;

            /* Subshell start check first inside word-start block */
            if (text[cur] == '$' && cur + 1 < len && text[cur+1] == '(') {
                add_attr(attrs, cur, cur + 2, &d_string); /* Green delimeter */
                cur += 2;
                BASH_PUSH(STATE_BASH_CMD_SUBST);
                is_cmd_start = TRUE;
                goto next_loop;
            }

            if (text[cur] == '$') {
                cur++;
                if (cur < len && text[cur] == '{') {
                    add_attr(attrs, start, start + 2, &d_punctuation); /* ${ delimeter */
                    cur++;
                    size_t var_content_start = cur;
                    while (cur < len && text[cur] != '}') cur++;
                    if (cur > var_content_start) add_attr(attrs, var_content_start, cur, &d_variable); /* XXX in Red */
                    if (cur < len) {
                        add_attr(attrs, cur, cur + 1, &d_punctuation); /* } delimeter */
                        cur++;
                    }
                } else if (cur < len && (g_ascii_isdigit(text[cur]) || strchr("?#@$!*", text[cur]))) {
                    cur++;
                    add_attr(attrs, start, cur, &d_type);
                } else {
                    while (cur < len && (g_ascii_isalnum(text[cur]) || text[cur] == '_')) cur++;
                    add_attr(attrs, start, cur, &d_variable);
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
                add_attr(attrs, start, cur, &d_number); /* Flags Orange */
                is_cmd_start = FALSE;
                continue;
            }

            if (in_case_patterns && text[start] != ';') {
                add_attr(attrs, start, cur, &d_variable);
                continue;
            }
            
            /* Assignment X=... or X+=... highlight X red */
            if (cur < len && text[cur] == '=') {
                size_t var_end = cur;
                if (var_end > start && text[var_end-1] == '+') var_end--;
                add_attr(attrs, start, var_end, &d_variable);
                add_attr(attrs, var_end, cur + 1, &d_logical);
                cur++;
                
                if (in_export_context) {
                    /* Highlight unquoted value in red */
                    if (cur < len && text[cur] != '"' && text[cur] != '\'') {
                        size_t val_start = cur;
                        while (cur < len && !g_ascii_isspace(text[cur]) && !strchr(";&|()<>", text[cur])) cur++;
                        if (cur > val_start) add_attr(attrs, val_start, cur, &d_variable);
                    }
                }
                
                is_cmd_start = FALSE;
                continue;
            }

            if (is_word_in_list(text + start, w_len, (const char*[]){"if", "then", "else", "elif", "fi", "for", "while", "do", "done", "case", "esac", "in", "function", NULL})) {
                add_attr(attrs, start, cur, &d_keyword);
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
                add_attr(attrs, start, cur, &d_keyword);
                if (is_word_in_list(text + start, w_len, (const char*[]){"export", "local", "declare", "readonly", "typeset", NULL})) {
                    in_export_context = TRUE;
                }
                is_cmd_start = FALSE;
            } else if (match_word(text, start, len, bash_booleans)) {
                add_attr(attrs, start, cur, &d_number); /* Booleans stay Orange */
                is_cmd_start = FALSE;
            } else if (in_export_context) {
                add_attr(attrs, start, cur, &d_variable); /* Variables in export are Red */
                is_cmd_start = FALSE;
            } else if (is_cmd_start) {
                if (match_word(text, start, len, bash_builtins)) {
                    add_attr(attrs, start, cur, &d_logical); /* 1st word Builtin -> Cyan */
                } else {
                    add_attr(attrs, start, cur, &d_function); /* 1st word command -> Blue */
                }
                is_cmd_start = FALSE;
            } else if (g_ascii_isdigit(text[start])) {
                add_attr(attrs, start, cur, &d_number);
                is_cmd_start = FALSE;
            } else {
                add_attr(attrs, start, cur, &d_string); /* Arguments -> Green */
            }
            continue;
        }

        if (text[cur] == '`') {
            add_attr(attrs, cur, cur + 1, &d_string);
            cur++;
            BASH_PUSH(STATE_SH_BACKTICK);
            is_cmd_start = TRUE;
            goto next_loop;
        }

        /* Fallback */
        add_attr(attrs, cur, cur + 1, &d_punctuation);
        cur++;

    next_loop: ;
    }

    set_line_end_state(ctx, line_index, (SyntaxState)state);
}
