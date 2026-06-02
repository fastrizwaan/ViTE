#include "editor-internal.h"
#include <string.h>

/* Forward declaration */
static size_t find_match_at_or_after_cursor(EditorWidget *self, size_t cursor_offset);

/* Helper: Update viewport matches from active search around current scroll position */
void
editor_widget_update_search_viewport(EditorWidget *self)
{
    if (!self->active_search) return;
    
    /* Get visible offset range */
    size_t start_offset, end_offset;
    editor_widget_get_visible_offset_range(self, &start_offset, &end_offset);
    
    /* Expand range for smooth experience */
    size_t padding = 50000; /* 50KB buffer */
    start_offset = (start_offset > padding) ? start_offset - padding : 0;
    end_offset += padding;
    
    /* Get viewport matches from search task */
    GArray *viewport_matches = document_search_task_get_viewport_matches(
        self->active_search, start_offset, end_offset);
    
    /* Update editor's viewport matches */
    if (self->search_matches) {
        g_array_unref(self->search_matches);
    }
    self->search_matches = viewport_matches; /* Takes ownership */
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

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
editor_widget_jump_to_current_match(EditorWidget *self)
{
    /* Filter mode navigation - check first since active_search is NULL in filter mode */
    if (!self->active_search && self->filtered_lines) {
        size_t total = compact_matches_count(self->filtered_lines);
        if (total == 0) return;
        
        size_t m_line;
        if (compact_matches_get(self->filtered_lines, self->global_match_idx, &m_line, NULL)) {
            size_t offset = document_get_offset_of_line(self->doc, m_line);
            editor_widget_clear_cursors(self);
            EditorCursor *c = editor_widget_get_primary_cursor(self);
            if (c) {
                c->cursor_offset = offset;
                c->selection_anchor = offset;
                c->target_x = -1;
                scroll_to_cursor_centered(self);
            }
        }
        gtk_widget_queue_draw(GTK_WIDGET(self));
        g_signal_emit_by_name(self, "caret-moved");
        size_t line, col;
        editor_widget_get_cursor_position(self, &line, &col);
        g_signal_emit_by_name(self, "cursor-moved", (guint)line, (guint)col);
        return;
    }
    
    /* Fallback: use viewport matches if no active search */
    if (!self->active_search) {
        if (!self->search_matches || self->search_matches->len == 0) return;
        
        SearchMatch m = g_array_index(self->search_matches, SearchMatch, self->current_match_idx);
        editor_widget_clear_cursors(self);
        EditorCursor *c = editor_widget_get_primary_cursor(self);
        if (c) {
            c->cursor_offset = m.end;
            c->selection_anchor = m.start;
            c->target_x = -1;
            scroll_to_cursor_centered(self);
        }
        gtk_widget_queue_draw(GTK_WIDGET(self));
        g_signal_emit_by_name(self, "caret-moved");
        size_t line, col;
        editor_widget_get_cursor_position(self, &line, &col);
        g_signal_emit_by_name(self, "cursor-moved", (guint)line, (guint)col);
        return;
    }
    
    size_t total = document_search_task_get_match_count(self->active_search);
    if (total == 0) return;
    
    SearchMatch m;
    if (!document_search_task_get_match_at(self->active_search, self->global_match_idx, &m)) {
        return;
    }
    
    /* Update cursor and selection */
    self->current_match_offset = m.start;
    editor_widget_clear_cursors(self);
    EditorCursor *c = editor_widget_get_primary_cursor(self);
    if (c) {
        c->cursor_offset = m.end;
        c->selection_anchor = m.start;
        c->target_x = -1;
    }
    
    /* Scroll to make match visible */
    scroll_to_cursor_centered(self);
    
    /* Update viewport matches to include new position */
    editor_widget_update_search_viewport(self);

    g_signal_emit_by_name(self, "caret-moved");
    size_t line, col;
    editor_widget_get_cursor_position(self, &line, &col);
    g_signal_emit_by_name(self, "cursor-moved", (guint)line, (guint)col);
}

void 
editor_widget_next_match(EditorWidget *self) 
{
    /* Filter mode navigation - check first since active_search is NULL in filter mode */
    if (!self->active_search && self->filtered_lines) {
        size_t total = compact_matches_count(self->filtered_lines);
        if (total == 0) return;
        
        self->global_match_idx++;
        if (self->global_match_idx >= total) self->global_match_idx = 0;
        
        editor_widget_jump_to_current_match(self);
        return;
    }
    
    /* Fallback: use viewport matches if no active search */
    if (!self->active_search) {
        if (!self->search_matches || self->search_matches->len == 0) return;
        
        self->current_match_idx++;
        if (self->current_match_idx >= (int)self->search_matches->len) {
            self->current_match_idx = 0;
        }
        editor_widget_jump_to_current_match(self);
        return;
    }
    
    size_t total = document_search_task_get_match_count(self->active_search);
    if (total == 0) return;
    
    /* Simply go to the next match by index */
    self->global_match_idx++;
    if (self->global_match_idx >= total) self->global_match_idx = 0;
    
    editor_widget_jump_to_current_match(self);
}

void 
editor_widget_prev_match(EditorWidget *self) 
{
    /* Filter mode navigation - check first since active_search is NULL in filter mode */
    if (!self->active_search && self->filtered_lines) {
        size_t total = compact_matches_count(self->filtered_lines);
        if (total == 0) return;
        
        if (self->global_match_idx == 0) self->global_match_idx = total - 1;
        else self->global_match_idx--;
        
        editor_widget_jump_to_current_match(self);
        return;
    }
    
    /* Fallback: use viewport matches if no active search */
    if (!self->active_search) {
        if (!self->search_matches || self->search_matches->len == 0) return;
        
        self->current_match_idx--;
        if (self->current_match_idx < 0) {
            self->current_match_idx = (int)self->search_matches->len - 1;
        }
        editor_widget_jump_to_current_match(self);
        return;
    }
    
    size_t total = document_search_task_get_match_count(self->active_search);
    if (total == 0) return;
    
    /* Simply go to the previous match by index */
    if (self->global_match_idx == 0) self->global_match_idx = total - 1;
    else self->global_match_idx--;
    
    editor_widget_jump_to_current_match(self);
}

void
editor_widget_set_active_search(EditorWidget *self, SearchTask *task)
{
    g_return_if_fail(EDITOR_IS_WIDGET(self));
    
    /* Note: We don't own the SearchTask reference, find-replace-bar manages it */
    self->active_search = task;
    
    if (task) {
        /* Reset global index based on cursor position */
        EditorCursor *c = editor_widget_get_primary_cursor(self);
        size_t cursor_pos = c ? c->cursor_offset : 0;
        size_t total = document_search_task_get_match_count(task);
        
        /* Find first match at or after cursor */
        self->global_match_idx = 0;
        for (size_t i = 0; i < total; i++) {
            SearchMatch m;
            if (document_search_task_get_match_at(task, i, &m)) {
                if (m.start >= cursor_pos) {
                    self->global_match_idx = i;
                    break;
                }
                /* If we pass cursor, use this match */
                self->global_match_idx = i;
            }
        }
    } else {
        self->global_match_idx = 0;
    }
}

/* Clear all search state - called when user starts typing */
void
editor_widget_clear_search(EditorWidget *self)
{
    g_return_if_fail(EDITOR_IS_WIDGET(self));
    
    /* Early exit if nothing to clear - avoids redraw on every keystroke */
    if (!self->search_matches && !self->active_search) {
        return;
    }
    
    if (self->search_matches) {
        g_array_unref(self->search_matches);
        self->search_matches = NULL;
    }
    self->active_search = NULL;
    self->global_match_idx = 0;
    self->current_match_idx = -1;
    self->current_match_offset = (size_t)-1;
    
    gtk_widget_queue_draw(GTK_WIDGET(self));
}

/* Helper: Binary search to find first match at or after cursor_offset */
static size_t
find_match_at_or_after_cursor(EditorWidget *self, size_t cursor_offset)
{
    if (!self->active_search) return 0;
    
    size_t total = document_search_task_get_match_count(self->active_search);
    if (total == 0) return 0;
    
    /* Binary search for first match.start >= cursor_offset */
    size_t low = 0, high = total;
    while (low < high) {
        size_t mid = (low + high) / 2;
        SearchMatch m;
        if (document_search_task_get_match_at(self->active_search, mid, &m)) {
            if (m.start < cursor_offset) {
                low = mid + 1;
            } else {
                high = mid;
            }
        } else {
            low = mid + 1;
        }
    }
    
    /* If past end, wrap to beginning */
    if (low >= total) low = 0;
    return low;
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
    // EditorCursor *c = editor_widget_get_primary_cursor(self);
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
    if (self->active_search) {
        EditorCursor *c = editor_widget_get_primary_cursor(self);
        size_t cursor_pos = c ? c->cursor_offset : 0;
        
        size_t total = document_search_task_get_match_count(self->active_search);
        if (total == 0) return -1;
        
        /* Find first match >= cursor_pos */
        size_t idx = find_match_at_or_after_cursor(self, cursor_pos);
        
        /* Check if cursor is actually inside the PREVIOUS match */
        if (idx == 0) {
             SearchMatch prev;
             if (document_search_task_get_match_at(self->active_search, total - 1, &prev)) {
                  if (cursor_pos >= prev.start && cursor_pos <= prev.end) {
                      self->global_match_idx = total - 1;
                      return (int)self->global_match_idx;
                  }
             }
        } else {
             SearchMatch prev;
             if (document_search_task_get_match_at(self->active_search, idx - 1, &prev)) {
                  if (cursor_pos >= prev.start && cursor_pos <= prev.end) {
                      self->global_match_idx = idx - 1;
                      return (int)self->global_match_idx;
                  }
             }
        }
        
        /* Check if cursor is exactly at current match start */
        SearchMatch curr;
        if (document_search_task_get_match_at(self->active_search, idx, &curr)) {
             if (cursor_pos == curr.start) {
                  self->global_match_idx = idx;
                  return (int)self->global_match_idx;
             }
        }

        self->global_match_idx = idx;
        return (int)self->global_match_idx;
    }
    
    if (self->filtered_lines) {
        size_t count = compact_matches_count(self->filtered_lines);
        if (count == 0) return -1;
        
        EditorCursor *c = editor_widget_get_primary_cursor(self);
        size_t cursor_pos = c ? c->cursor_offset : 0;
        size_t cursor_line = document_get_line_of_offset(self->doc, cursor_pos);
        
        size_t first_idx;
        
        if (self->global_match_idx < count) {
            size_t m_line;
            if (compact_matches_get(self->filtered_lines, self->global_match_idx, &m_line, NULL)) {
                if (cursor_line == m_line) {
                    return (int)self->global_match_idx;
                }
            }
        }
        
        compact_matches_find_range(self->filtered_lines, cursor_line, SIZE_MAX, &first_idx, NULL);
        
        /* If we are exactly on the line, we want that index */
        if (first_idx < count) {
            size_t m_line;
            if (compact_matches_get(self->filtered_lines, first_idx, &m_line, NULL)) {
                if (cursor_line == m_line) {
                    self->global_match_idx = first_idx;
                    return (int)first_idx;
                }
            }
        }
        
        /* Check if previous match is the one */
        if (first_idx > 0) {
            size_t m_line;
            if (compact_matches_get(self->filtered_lines, first_idx - 1, &m_line, NULL)) {
                if (cursor_line == m_line) {
                    return (int)(first_idx - 1);
                }
            }
        }
        
        /* If we are past the last one, wrap to 0 */
        if (first_idx >= count) {
            return 0;
        }
        
        return (int)first_idx;
    }

    return self->current_match_idx;
}


void
editor_widget_set_filtered_lines(EditorWidget *self, CompactMatches *matches, const char *pattern, gboolean regex, gboolean case_sensitive)
{
    g_return_if_fail(EDITOR_IS_WIDGET(self));
    
    if (self->filtered_lines) {
        compact_matches_free(self->filtered_lines);
        self->filtered_lines = NULL;
    }
    
    /* Take ownership of matches */
    self->filtered_lines = matches;
    
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

    editor_widget_rebuild_visible_lines(self);
    
    /* Force Layout update because line count effectively changes */
    if (self->line_y_offsets) {
        g_array_set_size(self->line_y_offsets, 0);
    }
    editor_widget_update_adjustments(self, -1, -1);
    gtk_widget_queue_draw(GTK_WIDGET(self));
}
