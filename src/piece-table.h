#ifndef PIECE_TABLE_H
#define PIECE_TABLE_H

#include <gtk/gtk.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SOURCE_ORIGINAL,
    SOURCE_ADD
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

/* Large buffer using size_t (64-bit) for sizes - handles multi-GB content */
typedef struct {
    char *data;
    size_t len;
    size_t capacity;
} LargeBuffer;

typedef struct {
    char *orig_data;
    size_t orig_size;
    
    LargeBuffer *add_buffer;  /* Uses size_t for 64-bit sizes */
    
    PieceNode *root;
    
    FileEncoding encoding;
    NewlineType newline_style;
    gboolean has_bom;
    gboolean is_mmapped;
    char *mmap_base;
    size_t mmap_size;
    
    uint64_t change_count;
} PieceTable;

PieceTable *piece_table_new(const char *filename);
void piece_table_free(PieceTable *pt);

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
size_t piece_table_iter_get_next_line_string(PieceTableIter *iter, GString *buf);


/* Insertion/Deletion */
void piece_table_insert(PieceTable *pt, size_t offset, const char *text, size_t len);
void piece_table_delete(PieceTable *pt, size_t offset, size_t len);

/* Optimized bulk replacement - accepts pre-calculated newline count to avoid O(N) scan */
void piece_table_replace_all(PieceTable *pt, const char *new_content, size_t len, size_t lf_count);
/* Zero-RAM replacement from file descriptor (mmap) */
void piece_table_replace_from_fd(PieceTable *pt, int fd, size_t len, size_t lf_count);
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

#endif
