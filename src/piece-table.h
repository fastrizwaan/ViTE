#ifndef PIECE_TABLE_H
#define PIECE_TABLE_H

#include <gtk/gtk.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SOURCE_ORIGINAL,
    SOURCE_ADD,
    SOURCE_EXTERNAL_START = 2
} PieceSource;

typedef struct {
    PieceSource source;
    size_t start;
    size_t length;
    size_t cached_lf;  /* Cached newline count for this piece */
} Piece;

/* Splay Tree Node */
typedef struct _PieceNode PieceNode;
struct _PieceNode {
    Piece piece;
    PieceNode *left, *right, *parent;
    
    /* Cached aggregates for subtree */
    size_t size_subtree; /* Total bytes */
    size_t lf_subtree;   /* Total newlines */
};

typedef enum {
    ENCODING_UTF8,
    ENCODING_UTF16LE,
    ENCODING_UTF16BE,
} FileEncoding;

typedef enum {
    NEWLINE_LF,
    NEWLINE_CRLF,
    NEWLINE_CR
} NewlineType;

/* Disk-backed buffer using mmap for zero-RAM content storage */
typedef struct {
    int fd;              /* File descriptor for temp file */
    char *path;          /* Path for cleanup (NULL if O_TMPFILE) */
    char *mmap_base;     /* mmap base pointer */
    size_t len;          /* Current content length */
    size_t capacity;     /* Current mmap size (page-aligned) */
} DiskBuffer;

typedef struct {
    char *orig_data;
    size_t orig_size;
    
    DiskBuffer *add_buffer;  /* Zero-RAM disk-backed buffer */
    
    PieceNode *root;
    
    FileEncoding encoding;
    NewlineType newline_style;
    gboolean has_bom;
    gboolean is_mmapped;
    char *mmap_base;
    size_t mmap_size;
    
    uint64_t change_count;
    
    GPtrArray *external_sources; /* Array of PieceTableSource* */
} PieceTable;

PieceTable *piece_table_new(const char *filename);
PieceTable *piece_table_new_empty(void);
void piece_table_free(PieceTable *pt);

/* Async Loading */
typedef void (*PieceTableLoadProgressCallback)(double progress, gpointer user_data);

typedef struct {
    PieceTable *pt;
    char *filename;
    GCancellable *cancellable;
    PieceTableLoadProgressCallback progress_cb;
    gpointer progress_data;
} PieceTableLoadData;

void piece_table_load_async(PieceTable *pt, const char *filename, GCancellable *cancellable, 
                            PieceTableLoadProgressCallback progress_cb, gpointer progress_data,
                            GAsyncReadyCallback callback, gpointer user_data);
gboolean piece_table_load_finish(PieceTable *pt, GAsyncResult *res, GError **error);


/* Iterator for fast sequential access */
typedef struct {
    PieceTable *pt;
    PieceNode *current_node;
    size_t offset_in_node;
    size_t current_line_index;
} PieceTableIter;

void piece_table_iter_init(PieceTable *pt, PieceTableIter *iter);
void piece_table_iter_init_at_line(PieceTable *pt, PieceTableIter *iter, size_t line_index);
size_t piece_table_iter_get_next_line(PieceTableIter *iter, char *buf, size_t buf_len);
size_t piece_table_iter_get_next_line(PieceTableIter *iter, char *buf, size_t buf_len);
size_t piece_table_iter_get_next_line_string(PieceTableIter *iter, GString *buf);
const char *piece_table_iter_get_chunk(PieceTableIter *iter, size_t *len);
void piece_table_iter_advance(PieceTableIter *iter, size_t len);


/* Insertion/Deletion */
void piece_table_insert(PieceTable *pt, size_t offset, const char *text, size_t len);
void piece_table_delete(PieceTable *pt, size_t offset, size_t len);

/* Optimized bulk replacement - accepts pre-calculated newline count to avoid O(N) scan */
void piece_table_replace_all(PieceTable *pt, const char *new_content, size_t len, size_t lf_count);
/* Zero-RAM replacement from file descriptor (mmap) */
void piece_table_replace_from_fd(PieceTable *pt, int fd, size_t len, size_t lf_count);

/* Async versions for massive files */
typedef struct _PieceTableReplaceTask PieceTableReplaceTask;

PieceTableReplaceTask *piece_table_replace_async_start(PieceTable *pt, int fd, size_t len);
gboolean piece_table_replace_async_step(PieceTableReplaceTask *task, gint64 budget_us, double *progress_out);
void piece_table_replace_async_finalize(PieceTableReplaceTask *task);
void piece_table_replace_async_cancel(PieceTableReplaceTask *task);

/* Insert content from an external file descriptor (Zero-RAM) */
void piece_table_insert_from_fd_range(PieceTable *pt, size_t offset, int fd, size_t file_offset, size_t len);
char *piece_table_get_text_range(PieceTable *pt, size_t offset, size_t len);
size_t piece_table_get_line_of_offset(PieceTable *pt, size_t offset);
size_t piece_table_get_offset_of_line(PieceTable *pt, size_t line_index);

/* Access */
/* Get text into a buffer. Returns bytes read. Caller must free if we return a copy? 
   Actually, let's just use a functional iterator or copy to buffer. 
   Getting a "line" pointer is hard because a line might be fragmented across pieces.
   We will return a freshly allocated string for the line. */
char *piece_table_get_line(PieceTable *pt, size_t line_index, size_t *out_len);
size_t piece_table_get_line_into(PieceTable *pt, size_t line_index, char *buf, size_t buf_len);
char *piece_table_get_line_truncated(PieceTable *pt, size_t line_index, size_t *out_len, size_t max_len);
size_t piece_table_get_line_length(PieceTable *pt, size_t line_index);
void piece_table_foreach_line(PieceTable *pt, void (*func)(size_t line_len, void *user_data), void *user_data);

/* New: Get line info without allocating if possible? 
   No, since we need to concatenate pieces. 
   For rendering, we might want an iterator that yields chunks.
   But for now, simple string alloc is fine for 1 line. */

size_t piece_table_get_length(PieceTable *pt);
size_t piece_table_get_line_count(PieceTable *pt);

/* Map functionality */
/* For debugging */
void piece_table_print_tree(PieceTable *pt);

void piece_table_set_newline_type(PieceTable *pt, NewlineType type);
NewlineType piece_table_get_newline_type(PieceTable *pt);

void piece_table_set_encoding(PieceTable *pt, FileEncoding enc);
FileEncoding piece_table_get_encoding(PieceTable *pt);

#endif
