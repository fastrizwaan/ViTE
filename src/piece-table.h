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

typedef struct {
    char *orig_data;
    size_t orig_size;
    
    GByteArray *add_buffer;
    
    PieceNode *root;
    
    /* Cache for iteration/access */
    // PieceNode *cached_node; // Optimization for sequential access
} PieceTable;

PieceTable *piece_table_new(const char *filename);
void piece_table_free(PieceTable *pt);

/* Insertion/Deletion */
void piece_table_insert(PieceTable *pt, size_t offset, const char *text, size_t len);
void piece_table_delete(PieceTable *pt, size_t offset, size_t len);
char *piece_table_get_text_range(PieceTable *pt, size_t offset, size_t len);
size_t piece_table_get_line_of_offset(PieceTable *pt, size_t offset);
size_t piece_table_get_offset_of_line(PieceTable *pt, size_t line_index);

/* Access */
/* Get text into a buffer. Returns bytes read. Caller must free if we return a copy? 
   Actually, let's just use a functional iterator or copy to buffer. 
   Getting a "line" pointer is hard because a line might be fragmented across pieces.
   We will return a freshly allocated string for the line. */
char *piece_table_get_line(PieceTable *pt, size_t line_index, size_t *out_len);
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
