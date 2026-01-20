#include "editor-internal.h"
#include <string.h>

void 
editor_widget_set_search_results(EditorWidget *self, GArray *matches) 
{
    /* Free old matches */
    if (self->search_matches) {
         g_array_unref(self->search_matches);
        self->search_matches = NULL;
    }
    
    /* Take a reference to new matches (caller keeps their own reference) */
    if (matches) {
        self->search_matches = g_array_ref(matches);
    } else {
        self->search_matches = NULL;
    }
    self->current_match_idx = -1;
    
    /* Loop to find first match after cursor? Or first match? */
    /* If cursor is at offset X, find match where start >= X. */
    if (self->search_matches && self->search_matches->len > 0) {
        EditorCursor *c = editor_widget_get_primary_cursor(self);
        size_t cursor = c ? c->cursor_offset : 0;
        
        /* Find closest match */
        int best_idx = 0;
        for (guint i = 0; i < self->search_matches->len; i++) {
            SearchMatch m = g_array_index(self->search_matches, SearchMatch, i);
            if (m.start >= cursor) {
                best_idx = (int)i;
                break;
            }
        }
        self->current_match_idx = best_idx;
    }
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void 
editor_widget_next_match(EditorWidget *self) 
{
    if (!self->search_matches || self->search_matches->len == 0) return;
    
    self->current_match_idx++;
    if (self->current_match_idx >= (int)self->search_matches->len) {
        self->current_match_idx = 0; /* Loop */
    }
    
    SearchMatch m = g_array_index(self->search_matches, SearchMatch, self->current_match_idx);
    
    /* Select the match */
    editor_widget_clear_cursors(self);
    
    /* Update primary cursor (since clear_cursors keeps one) */
    EditorCursor *c = editor_widget_get_primary_cursor(self);
    if (c) {
        c->cursor_offset = m.end;
        c->selection_anchor = m.start;
        c->target_x = -1;
        scroll_to_cursor(self); 
    }
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void 
editor_widget_prev_match(EditorWidget *self) 
{
    if (!self->search_matches || self->search_matches->len == 0) return;
    
    self->current_match_idx--;
    if (self->current_match_idx < 0) {
        self->current_match_idx = (int)self->search_matches->len - 1; /* Loop */
    }
    
    SearchMatch m = g_array_index(self->search_matches, SearchMatch, self->current_match_idx);
    
    editor_widget_clear_cursors(self);
    
    EditorCursor *c = editor_widget_get_primary_cursor(self);
    if (c) {
        c->cursor_offset = m.end;
        c->selection_anchor = m.start;
        c->target_x = -1;
        scroll_to_cursor(self);
    }
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

void 
editor_widget_replace_current(EditorWidget *self, const char *replacement, gboolean regex, const char *regex_text)
{
    if (self->current_match_idx == -1 || !self->search_matches) return;
    if (self->current_match_idx >= (int)self->search_matches->len) return;
    
    SearchMatch m = g_array_index(self->search_matches, SearchMatch, self->current_match_idx);
    
    char *final_replacement = NULL;
    
    if (regex && regex_text && *regex_text) {
        GError *err = NULL;
        GRegex *pattern = g_regex_new(regex_text, G_REGEX_OPTIMIZE | G_REGEX_CASELESS, 0, &err); 
        
        if (pattern) {
             /* Need context for Lookarounds/Anchors! */
             size_t len = 0;
             size_t line_idx = document_get_line_of_offset(self->doc, m.start);
             char *line_text = document_get_line(self->doc, line_idx, &len);
             size_t line_start = document_get_offset_of_line(self->doc, line_idx);
             size_t offset_in_line = m.start - line_start;
             
             if (line_text) {
                 GMatchInfo *info = NULL;
                 /* N.B. We use match_full to find the specific instance at offset */
                 /* This requires the pattern to match exactly at offset? 
                    Or we scan line and find the one intersecting offset?
                    Since we know `m` comes from a search, it should match.
                 */
                 
                 /* Normalize replacement */
                 char *norm = normalize_replacement_string(replacement, TRUE);
                 
                 if (g_regex_match_full(pattern, line_text, len, offset_in_line, 0, &info, NULL)) {
                     gint s, e;
                     g_match_info_fetch_pos(info, 0, &s, &e);
                     /* Verify it is the correct match (starts at offset_in_line) 
                        Note: g_regex_match_full finds *first* match after start_position.
                        If multiple matches, we might get one *after* offset?
                        We need checks.
                     */
                     if ((size_t)s == offset_in_line) {
                         final_replacement = g_match_info_expand_references(info, norm, NULL);
                     }
                 }
                 g_match_info_free(info);
                 g_free(norm);
                 g_free(line_text);
             }
             g_regex_unref(pattern);
        }
        if (err) g_error_free(err);
    } 
    
    if (!final_replacement) {
        /* Literal or fallback */
        if (regex) {
            /* If regex failed (e.g. compile error), use literal normalized? 
               Or just use raw replacement?
               If regex, we probably expect expanding.
               But if expanding failed, use literal to avoid data loss?
            */
            char *norm = normalize_replacement_string(replacement, TRUE);
            final_replacement = norm ? norm : g_strdup(replacement);
        } else {
            /* Literal mode: normalize handles standard escapes \n \t if we want?
               Usually literal replace supports \n? 
               Yes, normalize(..., FALSE) handles \n \t but not $1.
            */
            final_replacement = normalize_replacement_string(replacement, FALSE);
        }
    }

    /* Check if current selection matches the match (safety) */
    EditorCursor *c = editor_widget_get_primary_cursor(self);
    /* Strict check: cursor must wrap the match */
    /* Relaxed check: just replace at location */
    
    size_t new_pos = document_replace(self->doc, m, final_replacement);
    g_free(final_replacement);
    
    /* Update cursor to end of replacement */
    editor_widget_clear_cursors(self);
    editor_widget_add_cursor(self, new_pos);
    
    /* We must now REFRESH the search because offsets shifted! 
       The caller (FindBar) is responsible for re-triggering search. 
    */
}

int
editor_widget_get_current_match_index(EditorWidget *self)
{
    g_return_val_if_fail(EDITOR_IS_WIDGET(self), -1);
    return self->current_match_idx;
}

void
editor_widget_set_filtered_lines(EditorWidget *self, size_t *lines, size_t count, const char *pattern, gboolean regex, gboolean case_sensitive)
{
    g_return_if_fail(EDITOR_IS_WIDGET(self));
    
    if (self->filtered_lines) {
        g_array_unref(self->filtered_lines);
        self->filtered_lines = NULL;
    }
    
    if (lines && count > 0) {
        self->filtered_lines = g_array_sized_new(FALSE, FALSE, sizeof(size_t), count);
        g_array_append_vals(self->filtered_lines, lines, count);
    }
    
    /* Store filter settings */
    g_free(self->filter_pattern);
    self->filter_pattern = g_strdup(pattern);
    
    if (self->filter_regex_pattern) {
        g_regex_unref(self->filter_regex_pattern);
        self->filter_regex_pattern = NULL;
    }
    
    self->filter_is_regex = regex;
    self->filter_case_sensitive = case_sensitive;
    
    if (regex && pattern && *pattern) {
        GError *err = NULL;
        self->filter_regex_pattern = g_regex_new(pattern, 
            G_REGEX_OPTIMIZE | (case_sensitive ? 0 : G_REGEX_CASELESS), 
            0, &err);
        if (err) {
            /* If regex invalid, maybe fallback or just ignore? User responsible for validation usually. */
            g_error_free(err);
        }
    }
    
    /* Force Layout update because line count effectively changes */
    if (self->line_y_offsets) {
        g_array_set_size(self->line_y_offsets, 0);
    }
    editor_widget_update_adjustments(self, -1, -1);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
