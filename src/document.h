#ifndef DOCUMENT_H
#define DOCUMENT_H

#include <gtk/gtk.h>
#include "piece-table.h"
#include "undo.h"

typedef struct _Document Document;

void document_suspend_callbacks(Document *doc);
void document_resume_callbacks(Document *doc);

typedef char (*CharTransformFunc)(char c);

typedef enum {
    CHANGE_CASE_LOWER = 0,
    CHANGE_CASE_UPPER = 1,
    CHANGE_CASE_TITLE = 2,
    CHANGE_CASE_INVERT = 3
} ChangeCaseType;

Document *document_new(const char *filename);
Document *document_new_empty(void);
Document *document_ref(Document *doc);
void document_load_file_async(Document *doc, const char *filename, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
gboolean document_load_file_finish(Document *doc, GAsyncResult *res, GError **error);

typedef void (*DocumentProgressCallback)(double progress, FileEncoding encoding, NewlineType newline, void *user_data);
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

/* ============================================================================
 * Save API - File saving with atomic writes
 * ============================================================================ */

/* Save document to its current file path (returns FALSE if no path set) */
gboolean document_save(Document *doc, GError **error);

/* Save document to a new path (Save As) - Updates document's file_path on success */
gboolean document_save_as(Document *doc, const char *path, GError **error);

/* Pre-save encoding compatibility probe (sync, read-only, no disk writes).
   Returns TRUE if saving to the current encoding would lose characters. */
gboolean document_check_encoding_lossy(Document *doc);

/* Async save with progress for huge files (>100MB) */
typedef struct _DocumentSaveTask DocumentSaveTask;
typedef void (*DocumentSaveProgressCallback)(double progress, gboolean finished, gpointer user_data);

DocumentSaveTask *document_save_async_start(Document *doc, const char *path);
gboolean document_save_async_step(DocumentSaveTask *task, gint64 budget_us, double *progress_out);
void document_save_async_finish(DocumentSaveTask *task, GError **error);
gboolean document_save_async_had_lossy(DocumentSaveTask *task);
void document_save_async_cancel(DocumentSaveTask *task);

/* Content Observation Listeners */
typedef void (*DocumentContentCallback)(Document *doc, void *user_data);
/* start_line: where change began. line_delta: change in total lines (can be negative). */
typedef void (*DocumentUpdateCallback)(Document *doc, size_t start_line, int line_delta, void *user_data);

typedef void (*DocumentExternalChangeCallback)(Document *doc, void *user_data);

void document_add_content_callback(Document *doc, DocumentContentCallback callback, void *user_data);
void document_remove_content_callback(Document *doc, DocumentContentCallback callback, void *user_data);

void document_add_update_callback(Document *doc, DocumentUpdateCallback callback, void *user_data);
void document_remove_update_callback(Document *doc, DocumentUpdateCallback callback, void *user_data);

void document_add_external_change_callback(Document *doc, DocumentExternalChangeCallback callback, void *user_data);
void document_remove_external_change_callback(Document *doc, DocumentExternalChangeCallback callback, void *user_data);

/* Precise Content Editing (Byte-level) */
/* delta_len is positive for insertion, negative for deletion. offset is where the change happened. */
typedef void (*DocumentEditCallback)(Document *doc, size_t offset, int64_t delta_len, void *user_data);
void document_add_edit_callback(Document *doc, DocumentEditCallback callback, void *user_data);
void document_remove_edit_callback(Document *doc, DocumentEditCallback callback, void *user_data);

/* Content Access */
char *document_get_line(Document *doc, size_t line_index, size_t *len);
size_t document_get_line_into(Document *doc, size_t line_index, char *buf, size_t buf_len);
char *document_get_line_truncated(Document *doc, size_t line_index, size_t *out_len, size_t max_len, size_t *out_full_len);
size_t document_get_line_length(Document *doc, size_t line_index);
void document_foreach_line(Document *doc, void (*func)(size_t line_len, void *user_data), void *user_data);
size_t document_get_line_count(Document *doc);
size_t document_get_length(Document *doc);
char *document_get_text_range(Document *doc, size_t offset, size_t len);
size_t document_get_line_of_offset(Document *doc, size_t offset);
size_t document_get_line_of_offset(Document *doc, size_t offset);
size_t document_get_offset_of_line(Document *doc, size_t line_index);

uint64_t document_get_version(Document *doc);

/* Editing */
void document_insert(Document *doc, size_t offset, const char *text, size_t len);
void document_insert_from_fd(Document *doc, size_t offset, int fd, size_t len);

/* Async chunk-copying for huge Zero-RAM Paste */
typedef struct _DocumentInsertFromFdTask DocumentInsertFromFdTask;
typedef void (*DocumentInsertFromFdProgressCallback)(double progress, gboolean finished, gpointer user_data);
DocumentInsertFromFdTask *document_insert_from_fd_async(Document *doc, size_t offset, int fd, size_t len, DocumentInsertFromFdProgressCallback cb, gpointer user_data);
void document_insert_from_fd_cancel(DocumentInsertFromFdTask *task);

void document_transfer_range(Document *dest, Document *src, size_t src_offset, size_t len, size_t dest_offset);

/* Iterator for fast sequential access */
typedef struct {
    PieceTableIter iter;
} DocumentIter;

void document_iter_init(Document *doc, DocumentIter *iter, size_t line_index);
size_t document_iter_next_line(DocumentIter *iter, char *buf, size_t buf_len);
void document_delete(Document *doc, size_t offset, size_t len);
/* Optimized: snapshot-based delete for entire document (Select All + Delete/Cut) */
void document_delete_entire(Document *doc);

/* Async chunk-copying for huge Zero-RAM Cut */
typedef struct _DocumentDeleteEntireTask DocumentDeleteEntireTask;
typedef void (*DocumentDeleteEntireProgressCallback)(double progress, gboolean finished, gpointer user_data);
DocumentDeleteEntireTask *document_delete_entire_async(Document *doc, DocumentDeleteEntireProgressCallback cb, gpointer user_data);
void document_delete_entire_cancel(DocumentDeleteEntireTask *task);

/* Undo/Redo */
UndoInfo document_undo(Document *doc);
UndoInfo document_redo(Document *doc);
gboolean document_can_undo(Document *doc);
gboolean document_can_redo(Document *doc);
void document_begin_undo_group(Document *doc);
void document_end_undo_group(Document *doc);

void document_set_undo_group_selection(Document *doc, size_t start, size_t end);
void document_set_redo_group_selection(Document *doc, size_t start, size_t end);

void document_push_custom_undo(Document *doc, void *data, void (*exec_func)(void *, gboolean), void (*free_func)(void *));

void document_clear_undo_redo(Document *doc);

/* Async Undo/Redo with Progress */
typedef void (*UndoRedoProgressCallback)(double progress, gboolean finished, UndoInfo *info, gpointer user_data);
typedef struct _UndoRedoTask UndoRedoTask;
typedef struct _StreamingChangeCaseTask StreamingChangeCaseTask;

UndoRedoTask *document_undo_async(Document *doc, UndoRedoProgressCallback callback, gpointer user_data);
UndoRedoTask *document_redo_async(Document *doc, UndoRedoProgressCallback callback, gpointer user_data);
void document_undo_redo_cancel(UndoRedoTask *task);

void document_reload_from_disk(Document *doc);

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
gboolean document_search_task_get_match_at(SearchTask *task, size_t idx, SearchMatch *out);
void document_search_task_apply_edit(SearchTask *task, size_t offset, int64_t delta_len);

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

StreamingChangeCaseTask *document_change_case_streaming_start(Document *doc, 
                                     size_t start, size_t end,
                                     CharTransformFunc simple_func,
                                     int type,
                                     ReplaceProgressCallback callback, void *user_data);
void document_change_case_streaming_cancel(StreamingChangeCaseTask *task);

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

/* Filter Progress API */
size_t document_filter_task_get_processed(DocumentFilterTask *task);
size_t document_filter_task_get_total(DocumentFilterTask *task);
size_t document_filter_task_get_match_count(DocumentFilterTask *task);


#endif
