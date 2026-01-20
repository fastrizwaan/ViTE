#ifndef DOCUMENT_H
#define DOCUMENT_H

#include <gtk/gtk.h>
#include "piece-table.h"
#include "undo.h"

typedef struct _Document Document;

void document_suspend_callbacks(Document *doc);
void document_resume_callbacks(Document *doc);

Document *document_new(const char *filename);
Document *document_new_empty(void);
void document_load_file_async(Document *doc, const char *filename, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
gboolean document_load_file_finish(Document *doc, GAsyncResult *res, GError **error);

typedef void (*DocumentProgressCallback)(double progress, void *user_data);
void document_set_progress_callback(Document *doc, DocumentProgressCallback callback, void *user_data);

void document_free(Document *doc);
const char *document_get_file_path(Document *doc);
void document_set_file_path(Document *doc, const char *path);

/* State Tracking */
/* State Tracking */
gboolean document_is_modified(Document *doc);
void document_mark_saved(Document *doc);
void document_set_newline_type(Document *doc, NewlineType type);
NewlineType document_get_newline_type(Document *doc);
void document_set_encoding(Document *doc, FileEncoding enc);
FileEncoding document_get_encoding(Document *doc);
void document_add_modification_callback(Document *doc, void (*func)(Document *doc, gboolean modified, void *user_data), void *user_data);
void document_remove_modification_callback(Document *doc, void (*func)(Document *doc, gboolean modified, void *user_data), void *user_data);

/* Content Observation */
typedef void (*DocumentContentCallback)(Document *doc, void *user_data);
void document_add_content_callback(Document *doc, DocumentContentCallback callback, void *user_data);
void document_remove_content_callback(Document *doc, DocumentContentCallback callback, void *user_data);

/* Content Access */
char *document_get_line(Document *doc, size_t line_index, size_t *len);
char *document_get_line(Document *doc, size_t line_index, size_t *len);
char *document_get_line_truncated(Document *doc, size_t line_index, size_t *out_len, size_t max_len);
size_t document_get_line_length(Document *doc, size_t line_index);
void document_foreach_line(Document *doc, void (*func)(size_t line_len, void *user_data), void *user_data);
size_t document_get_line_count(Document *doc);
size_t document_get_length(Document *doc);
char *document_get_text_range(Document *doc, size_t offset, size_t len);
size_t document_get_line_of_offset(Document *doc, size_t offset);
size_t document_get_offset_of_line(Document *doc, size_t line_index);

/* Editing */
void document_insert(Document *doc, size_t offset, const char *text, size_t len);
void document_delete(Document *doc, size_t offset, size_t len);

/* Undo/Redo */
UndoInfo document_undo(Document *doc);
UndoInfo document_redo(Document *doc);
void document_begin_undo_group(Document *doc);
void document_end_undo_group(Document *doc);

void document_set_undo_group_selection(Document *doc, size_t start, size_t end);
void document_set_redo_group_selection(Document *doc, size_t start, size_t end);


typedef struct {
    size_t start;
    size_t end;
} SearchMatch;

/* Async Search API */
typedef struct _SearchTask SearchTask;
typedef struct _ReplaceTask ReplaceTask;
typedef void (*SearchCallback)(GArray *matches, gboolean finished, void *user_data);
typedef void (*ReplaceProgressCallback)(int processed, int total, gboolean finished, void *user_data);

SearchTask *document_search_async_start(Document *doc, const char *raw_query, gboolean regex, gboolean case_sensitive, gboolean whole_word, SearchCallback callback, void *user_data);
void document_search_async_cancel(SearchTask *task);

ReplaceTask *document_replace_async_start(Document *doc, GArray *matches, const char *replacement, gboolean regex, GRegex *cached_regex, ReplaceProgressCallback callback, void *user_data);
void document_replace_async_cancel(ReplaceTask *task);

/* Streaming Replace All - doesn't store matches, scans and replaces in one pass.
 * Use this for huge files to avoid memory explosion from storing millions of matches. */
typedef struct _StreamingReplaceTask StreamingReplaceTask;
StreamingReplaceTask *document_replace_streaming_start(Document *doc, const char *query, const char *replacement, 
                                                        gboolean regex, gboolean case_sensitive, gboolean whole_word,
                                                        ReplaceProgressCallback callback, void *user_data);
void document_replace_streaming_cancel(StreamingReplaceTask *task);
GArray *document_search_task_get_matches(SearchTask *task);
GRegex *document_search_task_get_pattern(SearchTask *task);
size_t document_search_task_get_total_lines(SearchTask *task);
size_t document_search_task_get_lines_searched(SearchTask *task);
size_t document_search_task_get_match_count(SearchTask *task);
GArray *document_search_task_get_viewport_matches(SearchTask *task, size_t start_offset, size_t end_offset);

GArray *document_search(Document *doc, const char *raw_query, gboolean regex, gboolean case_sensitive, gboolean whole_word);
gboolean document_find_next(Document *doc, SearchMatch *result, size_t start_pos, const char *raw_query, gboolean regex, gboolean case_sensitive, gboolean whole_word);
gboolean document_find_prev(Document *doc, SearchMatch *result, size_t start_pos, const char *raw_query, gboolean regex, gboolean case_sensitive, gboolean whole_word);

/* Viewport-only search for huge files - searches only within specified line range */
GArray *document_search_viewport(Document *doc, const char *raw_query, 
                                  gboolean regex, gboolean case_sensitive, 
                                  gboolean whole_word,
                                  size_t start_line, size_t end_line);

/* Targeted replace - only processes specified lines for efficiency */
int document_replace_targeted_lines(Document *doc, GArray *target_lines,
                                     const char *query, const char *replacement,
                                     gboolean regex, gboolean case_sensitive);

/* Replace APIs */
/* Replace single match. Returns new cursor position (end of replacement). */
size_t document_replace(Document *doc, SearchMatch match, const char *replacement);
/* Replace all matches. Returns number of replacements. */
/* Replace all matches. Returns number of replacements. */
int document_replace_all(Document *doc, const char *raw_query, const char *replacement, gboolean regex, gboolean case_sensitive, gboolean whole_word);

/* Efficiently replace pre-calculated matches (skips re-search). matches must be valid. */
int document_replace_known_matches(Document *doc, GArray *matches, const char *replacement, gboolean regex, GRegex *cached_regex);

const char *document_search_task_get_query(SearchTask *task);
gboolean document_search_task_get_regex(SearchTask *task);
gboolean document_search_task_get_case_sensitive(SearchTask *task);
gboolean document_search_task_get_whole_word(SearchTask *task);

char *normalize_replacement_string(const char *replacement, gboolean for_regex);


#include "compact-matches.h"

/* Filter API */
typedef struct {
    CompactMatches *matches; /* MMap storage of physical line indices */
    size_t count;
} FilterResult;

void filter_result_free(FilterResult *res);
FilterResult *document_filter_lines(Document *doc, const char *pattern, gboolean regex, gboolean case_sensitive);

/* Async Filtering */
typedef struct _DocumentFilterTask DocumentFilterTask;

DocumentFilterTask *document_filter_async_start(Document *doc, const char *pattern, gboolean regex, gboolean case_sensitive);
/* Returns TRUE if finished, FALSE if should continue yielding */
gboolean document_filter_async_step(DocumentFilterTask *task, gint64 time_budget_us);
FilterResult *document_filter_async_finish(DocumentFilterTask *task);
void document_filter_async_cancel(DocumentFilterTask *task);


#endif
