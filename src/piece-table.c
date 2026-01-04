#include "piece-table.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -- Utils -- */

static void
detect_encoding(const char *data, size_t size, FileEncoding *enc, gboolean *has_bom, size_t *bom_len)
{
    *enc = ENCODING_UTF8;
    *has_bom = FALSE;
    *bom_len = 0;

    if (size >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) {
        *enc = ENCODING_UTF8;
        *has_bom = TRUE;
        *bom_len = 3;
        return;
    } else if (size >= 2 && (unsigned char)data[0] == 0xFF && (unsigned char)data[1] == 0xFE) {
        *enc = ENCODING_UTF16LE;
        *has_bom = TRUE;
        *bom_len = 2;
        return;
    } else if (size >= 2 && (unsigned char)data[0] == 0xFE && (unsigned char)data[1] == 0xFF) {
        *enc = ENCODING_UTF16BE;
        *has_bom = TRUE;
        *bom_len = 2;
        return;
    }

    /* Heuristic: Check for UTF-16 without BOM */
    if (size >= 4) {
        /* Check if it's valid UTF-8 first */
        if (!g_utf8_validate(data, size, NULL)) {
            size_t check_len = (size > 1024) ? 1024 : size;
            size_t le_count = 0;
            size_t be_count = 0;
            
            for (size_t i = 0; i < check_len - 1; i += 2) {
                if (data[i+1] == 0 && data[i] != 0) le_count++;
                if (data[i] == 0 && data[i+1] != 0) be_count++;
            }
            
            if (le_count > check_len / 4) {
                *enc = ENCODING_UTF16LE;
            } else if (be_count > check_len / 4) {
                *enc = ENCODING_UTF16BE;
            }
        }
    }
}

static void
detect_newline_style(const char *data, size_t size, NewlineType *style)
{
    *style = NEWLINE_LF; // Default
    const char *ptr = data;
    const char *end = data + size;
    while (ptr < end) {
        if (*ptr == '\n') {
            *style = NEWLINE_LF;
            return;
        } else if (*ptr == '\r') {
            if (ptr + 1 < end && *(ptr + 1) == '\n') {
                *style = NEWLINE_CRLF;
                return;
            } else {
                *style = NEWLINE_CR;
                return;
            }
        }
        ptr++;
    }
}

/* Robust newline finder that handles \n, \r\n, and \r */
static const char *
find_next_newline(const char *ptr, const char *end, int *nl_len)
{
    while (ptr < end) {
        if (*ptr == '\n') {
            if (nl_len) *nl_len = 1;
            return ptr;
        } else if (*ptr == '\r') {
            if (ptr + 1 < end && *(ptr + 1) == '\n') {
                if (nl_len) *nl_len = 2;
                return ptr;
            } else {
                if (nl_len) *nl_len = 1;
                return ptr;
            }
        }
        ptr++;
    }
    return NULL;
}

static size_t
count_newlines(const char *data, size_t len)
{
    size_t count = 0;
    const char *ptr = data;
    const char *end = data + len;
    int nl_len;
    while ((ptr = find_next_newline(ptr, end, &nl_len))) {
        count++;
        ptr += nl_len;
    }
    return count;
}

static size_t
piece_newlines(PieceTable *pt, Piece *p)
{
    const char *data = (p->source == SOURCE_ORIGINAL) ? pt->orig_data : (char*)pt->add_buffer->data;
    return count_newlines(data + p->start, p->length);
}

/* -- Node logic -- */

static PieceNode*
node_new(Piece piece)
{
    PieceNode *n = malloc(sizeof(PieceNode));
    n->piece = piece;
    n->left = n->right = n->parent = NULL;
    n->size_subtree = piece.length;
    n->lf_subtree = 0; /* Caller must set or update */
    return n;
}

static void
update_node(PieceTable *pt, PieceNode *x)
{
    if (!x) return;
    x->size_subtree = x->piece.length;
    x->lf_subtree = piece_newlines(pt, &x->piece);
    
    if (x->left) {
        x->size_subtree += x->left->size_subtree;
        x->lf_subtree += x->left->lf_subtree;
        x->left->parent = x;
    }
    if (x->right) {
        x->size_subtree += x->right->size_subtree;
        x->lf_subtree += x->right->lf_subtree;
        x->right->parent = x;
    }
}

/* Rotate right */
static void
rotate_right(PieceTable *pt, PieceNode *x)
{
    PieceNode *y = x->left;
    if (!y) return;
    
    x->left = y->right;
    if (y->right) y->right->parent = x;
    
    y->parent = x->parent;
    if (!x->parent) pt->root = y;
    else if (x == x->parent->right) x->parent->right = y;
    else x->parent->left = y;
    
    y->right = x;
    x->parent = y;
    
    update_node(pt, x);
    update_node(pt, y);
}

/* Rotate left */
static void
rotate_left(PieceTable *pt, PieceNode *x)
{
    PieceNode *y = x->right;
    if (!y) return;
    
    x->right = y->left;
    if (y->left) y->left->parent = x;
    
    y->parent = x->parent;
    if (!x->parent) pt->root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    
    y->left = x;
    x->parent = y;
    
    update_node(pt, x);
    update_node(pt, y);
}

static void
splay(PieceTable *pt, PieceNode *x)
{
    while (x->parent) {
        if (!x->parent->parent) {
            if (x->parent->left == x) rotate_right(pt, x->parent);
            else rotate_left(pt, x->parent);
        } else if (x->parent->left == x && x->parent->parent->left == x->parent) {
            rotate_right(pt, x->parent->parent);
            rotate_right(pt, x->parent);
        } else if (x->parent->right == x && x->parent->parent->right == x->parent) {
            rotate_left(pt, x->parent->parent);
            rotate_left(pt, x->parent);
        } else if (x->parent->left == x && x->parent->parent->right == x->parent) {
            rotate_right(pt, x->parent);
            rotate_left(pt, x->parent); /* Note: Parent of x changes after first rotate */
            /* Wait, standard splay double rotation */
            /* If x is left child of right child */
        } else {
             /* x is right child of left child, or left of right */
             // Just doing simple splay steps is easiest
             if (x == x->parent->left) {
                 rotate_right(pt, x->parent);
             } else {
                 rotate_left(pt, x->parent);
             }
        }
    }
}

/* Find node containing offset. Returns cached start_offset of that node too? 
   Nope, we accumulate logic. */
static PieceNode *
find_node_at_offset(PieceTable *pt, size_t offset, size_t *node_start_offset)
{
    PieceNode *curr = pt->root;
    size_t current_start = 0;
    
    while (curr) {
        size_t left_size = curr->left ? curr->left->size_subtree : 0;
        
        if (offset < left_size) {
            curr = curr->left;
        } else if (offset >= left_size + curr->piece.length) {
            offset -= (left_size + curr->piece.length);
            current_start += (left_size + curr->piece.length);
            curr = curr->right;
        } else {
            /* Found */
            if (node_start_offset) *node_start_offset = current_start + left_size;
            splay(pt, curr);
            return curr;
        }
    }
    return NULL;
}

/* Helper: find node by line index.
   Returns the node containing the start of 'line_index'.
   updates 'out_start_byte' to the byte offset of the BEGINNING of this node.
   Wait, identifying which byte starts the line is tricky if a line spans nodes.
   The 'find line' finds the node containing the Nth newline?
   
   If I want "Line 5", I look for 5th newline.
   Actually, "Line 5" starts after 4th newline.
*/
static PieceNode *
find_node_for_line(PieceTable *pt, size_t line_index, size_t *out_node_start_lf, size_t *out_node_start_byte)
{
    PieceNode *curr = pt->root;
    size_t seen_lf = 0;
    size_t seen_byte = 0;

    if (line_index == 0) {
        /* Line 0 starts at the leftmost node */
        curr = pt->root;
        seen_lf = 0;
        seen_byte = 0;
        
        while (curr) {
            if (curr->left) {
                 curr = curr->left;
            } else {
                 if (out_node_start_lf) *out_node_start_lf = seen_lf;
                 if (out_node_start_byte) *out_node_start_byte = seen_byte;
                 splay(pt, curr);
                 return curr;
            }
        }
        return NULL;
    }

    size_t target_lf = line_index - 1;
    size_t target_byte = 0;
    
    curr = pt->root;
    seen_lf = 0;
    seen_byte = 0;
    int found_target = 0;
    
    while (curr) {
        size_t left_lf = curr->left ? curr->left->lf_subtree : 0;
        size_t left_size = curr->left ? curr->left->size_subtree : 0;
        
        if (target_lf < seen_lf + left_lf) {
             curr = curr->left;
        } else {
             size_t node_lf = piece_newlines(pt, &curr->piece);
             if (target_lf < seen_lf + left_lf + node_lf) {
                 const char *data = (curr->piece.source == SOURCE_ORIGINAL) ? pt->orig_data : (char*)pt->add_buffer->data;
                 size_t internal_idx = target_lf - (seen_lf + left_lf);
                 size_t found = 0;
                 const char *ptr = data + curr->piece.start;
                 const char *end = ptr + curr->piece.length;
                 const char *p_ptr = ptr;
                 int nl_len;
                 
                 while (ptr < end && found < internal_idx) {
                     ptr = find_next_newline(ptr, end, &nl_len);
                     ptr += nl_len;
                     found++;
                 }
                 const char *lf_pos = find_next_newline(ptr, end, &nl_len);
                 
                 if (lf_pos) {
                     size_t lf_off = lf_pos - p_ptr;
                     target_byte = seen_byte + left_size + lf_off + nl_len;
                     found_target = 1;
                     break; 
                 }
             }
             seen_lf += left_lf + node_lf;
             seen_byte += left_size + curr->piece.length;
             curr = curr->right;
        }
    }
    
    if (found_target) {
        size_t found_start_byte;
        PieceNode *res = find_node_at_offset(pt, target_byte, &found_start_byte);
        if (res) {
             if (out_node_start_lf) *out_node_start_lf = res->left ? res->left->lf_subtree : 0;
             if (out_node_start_byte) *out_node_start_byte = found_start_byte;
             return res;
        }
    }
    
    return NULL;
}



/* Helper to build balanced tree from array of nodes */
static PieceNode*
build_balanced_tree_recursive(PieceNode **nodes, int start, int end, PieceTable *pt)
{
    if (start > end) return NULL;
    int mid = (start + end) / 2;
    PieceNode *node = nodes[mid];
    
    node->left = build_balanced_tree_recursive(nodes, start, mid - 1, pt);
    if (node->left) node->left->parent = node;
    
    node->right = build_balanced_tree_recursive(nodes, mid + 1, end, pt);
    if (node->right) node->right->parent = node;
    
    update_node(pt, node);
    return node;
}

PieceTable *
piece_table_new(const char *filename)
{
    int fd = open(filename, O_RDONLY);
    char *mmap_ptr = NULL;
    size_t size = 0;
    
    if (fd != -1) {
        struct stat sb;
        fstat(fd, &sb);
        size = sb.st_size;
        if (size > 0)
            mmap_ptr = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
    }

    PieceTable *pt = g_new0(PieceTable, 1);
    pt->mmap_base = mmap_ptr;
    pt->mmap_size = size;
    pt->is_mmapped = TRUE;
    pt->orig_data = mmap_ptr;
    pt->orig_size = size;

    if (mmap_ptr == MAP_FAILED) {
        pt->mmap_base = NULL;
        pt->orig_data = NULL;
        pt->orig_size = 0;
        pt->is_mmapped = FALSE;
    }

    size_t bom_len = 0;
    if (pt->orig_data) {
        detect_encoding(pt->orig_data, pt->orig_size, &pt->encoding, &pt->has_bom, &bom_len);
        
        if (pt->encoding != ENCODING_UTF8) {
            const char *from_codeset = (pt->encoding == ENCODING_UTF16LE) ? "UTF-16LE" : "UTF-16BE";
            gsize bytes_read, bytes_written;
            GError *error = NULL;
            char *utf8_data = g_convert(pt->orig_data + bom_len, pt->orig_size - bom_len, "UTF-8", from_codeset, &bytes_read, &bytes_written, &error);
            if (utf8_data) {
                pt->orig_data = utf8_data;
                pt->orig_size = bytes_written;
                pt->is_mmapped = FALSE;
            } else {
                g_warning("Failed to convert file: %s", error->message);
                g_clear_error(&error);
                pt->orig_data = g_strdup("");
                pt->orig_size = 0;
                pt->is_mmapped = FALSE;
            }
        } else if (bom_len > 0) {
            pt->orig_data += bom_len;
            pt->orig_size -= bom_len;
        }
        
        detect_newline_style(pt->orig_data, pt->orig_size, &pt->newline_style);
    }

    pt->add_buffer = g_byte_array_new();
    
    if (pt->orig_size > 0) {
        /* Chunking strategy */
        size_t chunk_size = 16 * 1024; /* 16KB */
        size_t count = (pt->orig_size + chunk_size - 1) / chunk_size;
        
        PieceNode **nodes = malloc(count * sizeof(PieceNode*));
        
        for (size_t i = 0; i < count; i++) {
            size_t start = i * chunk_size;
            size_t len = chunk_size;
            if (start + len > pt->orig_size) len = pt->orig_size - start;
            
            Piece p = { SOURCE_ORIGINAL, start, len };
            nodes[i] = node_new(p);
        }
        
        pt->root = build_balanced_tree_recursive(nodes, 0, (int)count - 1, pt);
        free(nodes);
    } else {
        pt->root = NULL;
    }

    return pt;
}

void
piece_table_free(PieceTable *pt)
{
    /* Should free tree nodes recursively */
    /* ... skipped for brevity in this step, but needed */
    if (pt->is_mmapped) {
        if (pt->mmap_base && pt->mmap_size > 0) munmap(pt->mmap_base, pt->mmap_size);
    } else {
        g_free(pt->orig_data);
    }
    g_byte_array_unref(pt->add_buffer);
    free(pt);
}

size_t
piece_table_get_length(PieceTable *pt)
{
    return pt->root ? pt->root->size_subtree : 0;
}

size_t
piece_table_get_line_count(PieceTable *pt)
{
    /* Always at least 1 line */
    if (!pt->root) return 1;
    size_t n = pt->root->lf_subtree;
    /* If the last character is a newline, we have N+1 lines?
       Common editor logic: "a\n" is 2 lines. "a" is 1 line.
       count(new lines) = 1 in "a\n".
       So lines = newlines + 1.
    */
    return n + 1;
}

/* Insert logic */
void
piece_table_insert(PieceTable *pt, size_t offset, const char *text, size_t len)
{
    if (len == 0) return;
    
    /* Add text to buffer */
    size_t start_in_add = pt->add_buffer->len;
    g_byte_array_append(pt->add_buffer, (guint8*)text, len);
    
    Piece new_piece = { SOURCE_ADD, start_in_add, len };
    PieceNode *new_node = node_new(new_piece);
    /* Set manual LF */
    new_node->lf_subtree = count_newlines(text, len);

    if (!pt->root) {
        pt->root = new_node;
        update_node(pt, pt->root);
        return;
    }

    /* Split logic */
    size_t node_start_off;
    PieceNode *at_node = find_node_at_offset(pt, offset, &node_start_off);
    
    if (!at_node) {
        /* Append at end */
        /* Splay rightmost? Use simpler append */
        /* Assuming offset == length, find max */
        PieceNode *curr = pt->root;
        while (curr->right) curr = curr->right;
        
        curr->right = new_node;
        new_node->parent = curr;
        splay(pt, new_node); /* Updates everything */
        return;
    }

    /* We split 'at_node' at 'offset - node_start_off' */
    size_t split_point = offset - node_start_off;
    
    /* If split point is 0, we insert before */
    if (split_point == 0) {
        /* at_node is root due to splay. new_node becomes root.
           left of new_node is at_node->left.
           right of new_node is at_node.
           at_node->left = NULL.
        */
        new_node->left = at_node->left;
        if (new_node->left) new_node->left->parent = new_node;
        
        new_node->right = at_node;
        at_node->parent = new_node;
        at_node->left = NULL;
        
        pt->root = new_node;
        update_node(pt, at_node);
        update_node(pt, new_node);
        return;
    } else if (split_point == at_node->piece.length) {
         /* Insert after. new_node becomes root. 
            new_node->right = at_node->right.
            new_node->left = at_node.
            at_node->right = NULL.
         */
         
         /* But wait, generic insertion into BST/Splay:
            Insert and Splay. 
         */
         /* Simplified: Just modifying the tree structure directly */
         new_node->right = at_node->right;
         if (new_node->right) new_node->right->parent = new_node;
         
         new_node->left = at_node;
         at_node->parent = new_node;
         at_node->right = NULL;
         
         pt->root = new_node;
         update_node(pt, at_node);
         update_node(pt, new_node);
         return;
    }
    
    /* Middle split: at_node becomes Left part. new_node is middle. Right part is new node. */
    /* Create wrapper for right part */
    Piece right_piece = at_node->piece;
    right_piece.start += split_point;
    right_piece.length -= split_point;
    PieceNode *right_node = node_new(right_piece);
    /* Calculate LF for right piece carefully */
    right_node->lf_subtree = piece_newlines(pt, &right_piece);

    /* Update left piece (at_node) */
    at_node->piece.length = split_point;
    /* Needs update LF */
    /* Only way is to recount or be smart. Recounting small piece is fast. */
    /* Or we already know total LF. */
    /* Recounting part of it */
    /* optimization: we know total in at_node, we calc right_node, so left is total - right */
    size_t old_total = piece_newlines(pt, &at_node->piece); // This re-reads full, safer
    /* Actually at_node data is valid still */
    at_node->lf_subtree = count_newlines(
        (at_node->piece.source == SOURCE_ORIGINAL ? pt->orig_data : (char*)pt->add_buffer->data) + at_node->piece.start,
        split_point
    );
    at_node->size_subtree = split_point; // Temporary before full update

    /* Stitch:
       Left (at_node) < New < Right
       at_node is root.
       We can make New root. 
       New->left = at_node.
       New->right = Right.
       Right->right = at_node->right.
       What about at_node->left? Stays with at_node.
       at_node->right = NULL. 
    */
    
    /* Correct splay insert logic:
       Root is at_node.
       Detach at_node->right.
    */
    PieceNode *old_right = at_node->right;
    
    /* Construct New Root: */
    pt->root = new_node;
    
    new_node->left = at_node;
    at_node->parent = new_node;
    at_node->right = NULL; /* Cut */
    
    new_node->right = right_node;
    right_node->parent = new_node;
    
    right_node->right = old_right;
    if (old_right) old_right->parent = right_node;
    
    update_node(pt, at_node);
    update_node(pt, right_node);
    update_node(pt, new_node);
}

/* Delete logic */
/* Helper: Split buffer at logical offset. 
   Returns the node that ends exactly at offset (left part), 
   or the node that starts exactly at offset?
   Splay approach: After splay(find(offset)), root contains offset.
   If offset is middle of root, we split root.
   If offset is 0 of root, root is Right, Left is root->left...
*/

static void
ensure_split_at(PieceTable *pt, size_t offset)
{
    /* Make sure there is a boundary at 'offset' */
    size_t node_start;
    PieceNode *node = find_node_at_offset(pt, offset, &node_start);
    if (!node) return; /* End of file or empty */
    
    size_t local_off = offset - node_start;
    if (local_off == 0 || local_off == node->piece.length) return;
    
    /* Split 'node' into A and B */
    /* Reuse logic from insert:
       node becomes A (0..local_off)
       Create B (local_off..len)
       A < B
    */
    Piece right_piece = node->piece;
    right_piece.start += local_off;
    right_piece.length -= local_off;
    
    PieceNode *right_node = node_new(right_piece);
    right_node->lf_subtree = piece_newlines(pt, &right_piece);
    
    /* Update Left (node) */
    node->piece.length = local_off;
    node->lf_subtree = count_newlines(
         (node->piece.source == SOURCE_ORIGINAL ? pt->orig_data : (char*)pt->add_buffer->data) + node->piece.start,
         local_off
    ); // simplified
    node->size_subtree = local_off; // Temp
    
    /* Insert B right of A */
    /* A is root. A->right becomes B->right. A->right is B. */
    
    PieceNode *old_right = node->right;
    
    node->right = right_node;
    right_node->parent = node;
    
    right_node->right = old_right;
    if (old_right) old_right->parent = right_node;
    
    update_node(pt, right_node);
    update_node(pt, node);
}

void
piece_table_delete(PieceTable *pt, size_t offset, size_t len)
{
    if (len == 0 || !pt->root) return;
    
    size_t total = piece_table_get_length(pt);
    if (offset + len > total) len = total - offset;

    /* Strategy:
       Split at offset.
       Split at offset + len.
       Then we have: [LeftPart] [RangeToDelete] [RightPart]
       We can remove [RangeToDelete] subtree.
    */
    
    ensure_split_at(pt, offset);
    ensure_split_at(pt, offset + len);
    
    /* 1. Splay node at 'offset-1' or just 'offset'?
       If we splay 'offset', it brings the node STARTING at offset to root.
       Then everything >= offset is in Root+Right.
    */
    
    size_t start_node_off;
    PieceNode *start_node = find_node_at_offset(pt, offset, &start_node_off);
    /* start_node should now START at offset because of ensure_split_at(offset) 
       Wait, find_node returns the node containing the offset.
       If offset is boundary, it might return the one before or after depending on logic.
       My logic: offset < left_size -> left.
       >= -> right.
       If offset == 0 of node, it returns that node.
       So yes, start_node starts at offset.
    */
    
    if (!start_node) return; /* Should not happen */
    
    /* Splay start_node. Now Root = start_node. 
       Everything < offset is in Root->Left.
       Everything >= offset is Root + Root->Right.
    */
    
    /* Now find end boundary */
    /* We want to cut from Offset to Offset+Len. 
       We already split at Offset+Len.
       So the end of deletion is a boundary.
       The node Starting at Offset+Len should be preserved.
    */
    
    size_t end_node_off;
    PieceNode *end_node = find_node_at_offset(pt, offset + len, &end_node_off);
    
    if (!end_node) {
        /* Deleting until EOF */
        /* Root is start_node. Root->Left is preserved.
           Root and Root->Right are deleted?
           Yes.
        */
        PieceNode *left_preserved = start_node->left;
        if (left_preserved) {
            left_preserved->parent = NULL;
            pt->root = left_preserved;
        } else {
            pt->root = NULL;
        }
        /* TODO: Free deleted nodes (start_node and right) */
        // free_tree(start_node); -- skipping for now, assume leak is acceptable in proto
        return;
    }
    
    /* We have end_node. Splay it.
       Now Root = end_node. (Starts at offset+len).
       Root->Left contains the Stuff to Delete AND the Stuff Before Offset.
    */
    
    /* This splay messes up the first splay structure.
       Standard Splay Delete Range (start, end):
       1. Splay(start-1) -> Root.
       2. Splay(end+1) -> Root->Right.
       3. Root->Right->Left is the range.
    */
    
    /* Case 1: Deleting from 0? */
    PieceNode *left_anchor = NULL;
    if (offset > 0) {
        size_t off;
        left_anchor = find_node_at_offset(pt, offset - 1, &off);
        /* because we split at offset, offset-1 must be in the node Left of boundary. */
    }
    
    if (left_anchor) {
        splay(pt, left_anchor);
        /* Root is left_anchor. Root->right contains range + tail. */
        /* Now splay end_node (starts at offset + len) INSIDE Root->right?
           Splaying normally Bubbles it to Root.
        */
        splay(pt, end_node);
        /* Now Root = end_node. 
           Root->Left contains everything before end.
           Root->Left->Right ... this is getting complicated.
        */
        /* Let's try simpler:
           Splay(left_anchor). Root = left_anchor.
           Expose Right child.
           The node `end_node` is in Right child.
           Splay `end_node`. Root = end_node.
           left_anchor is now Left child of `end_node` (if it's the max of left part).
           Actually left_anchor is < end_node.
           
           If we just splay `end_node`, `left_anchor` is somewhere in `end_node->left`.
           If we then splay `left_anchor` but STOP when parent is `end_node`... (Standard Splay Range method).
           
           Since I don't have `splay_to_root_child`, let's hack:
           
           Splay(end_node).
           Cut end_node->left. Call it L.
           Splay(left_anchor) in L.
             (Hack: Temporarily make L a root, splay, reattach).
           L_new->right is the Range! Delete it!
        */
        
        splay(pt, end_node);
        PieceNode *L = end_node->left;
        if (L) L->parent = NULL;
        
        pt->root = L;
        splay(pt, left_anchor); /* Now left_anchor is new root of L */
        
        /* content to delete is left_anchor->right */
        /* Free left_anchor->right */
        /* PieceNode *deleted = left_anchor->right; - unused variable warning avoidance if not freeing */
        left_anchor->right = NULL;
        update_node(pt, left_anchor);
        
        /* Reattach */
        end_node->left = left_anchor;
        left_anchor->parent = end_node;
        pt->root = end_node;
        update_node(pt, end_node);
        
    } else {
        /* Deleting from 0 */
        /* Splay end_node. Root = end_node.
           Root->Left is the range [0...len].
           Delete Root->Left.
        */
        splay(pt, end_node);
        // free_tree(end_node->left);
        end_node->left = NULL;
        update_node(pt, end_node);
    }
}

/* Helper to get text from range */
char *
piece_table_get_text_range(PieceTable *pt, size_t offset, size_t len)
{
    if (len == 0) return g_strdup("");
    GString *res = g_string_new("");
    
    /* Naive: iterate */
    /* Or split/find nodes. iterating chars is slow. 
       We should iterate pieces. 
    */
    
    size_t cur = offset;
    size_t remaining = len;
    
    while (remaining > 0) {
        size_t node_start;
        PieceNode *n = find_node_at_offset(pt, cur, &node_start);
        if (!n) break;
        
        size_t off_in_node = cur - node_start;
        size_t avail = n->piece.length - off_in_node;
        size_t chunk = (avail < remaining) ? avail : remaining;
        
        const char *data = (n->piece.source == SOURCE_ORIGINAL) ? pt->orig_data : (char*)pt->add_buffer->data;
        g_string_append_len(res, data + n->piece.start + off_in_node, chunk);
        
        cur += chunk;
        remaining -= chunk;
    }
    
    return g_string_free(res, FALSE);
}

/* Walk the tree in-order starting from a node to build the line */
/* This is O(K) where K is number of pieces in the line. */
char *
piece_table_get_line(PieceTable *pt, size_t line_index, size_t *out_len)
{
    size_t start_lf, start_byte;
    PieceNode *node = find_node_for_line(pt, line_index, &start_lf, &start_byte);
    
    if (!node) {
        *out_len = 0;
        return g_strdup("");
    }
    
    size_t relative_lf = line_index - start_lf;
    const char *data = (node->piece.source == SOURCE_ORIGINAL) ? pt->orig_data : (char*)pt->add_buffer->data;
    data += node->piece.start;
    size_t len = node->piece.length;
    
    size_t internal_offset = 0;
    int nl_len;
    if (relative_lf > 0) {
        size_t found = 0;
        const char *ptr = data;
        const char *end = data + len;
        while (ptr < end && found < relative_lf) {
            const char *p = find_next_newline(ptr, end, &nl_len);
            if (!p) break;
            ptr = p + nl_len;
            found++;
        }
        internal_offset = ptr - data;
    }
    
    GString *res = g_string_new("");
    const char *ptr = data + internal_offset;
    const char *node_end = data + len;
    
    const char *eol = find_next_newline(ptr, node_end, &nl_len);
    if (eol) {
        g_string_append_len(res, ptr, eol - ptr + nl_len); 
        *out_len = res->len;
        return g_string_free(res, FALSE);
    }
    
    g_string_append_len(res, ptr, node_end - ptr);
    
    PieceNode *curr = node;
    while (1) {
        if (curr->right) {
            curr = curr->right;
            while (curr->left) curr = curr->left;
        } else {
            PieceNode *p = curr->parent;
            while (p && curr == p->right) {
                curr = p;
                p = p->parent;
            }
            curr = p;
        }
        
        if (!curr) break; 
        
        const char *cdata = (curr->piece.source == SOURCE_ORIGINAL) ? pt->orig_data : (char*)pt->add_buffer->data;
        cdata += curr->piece.start;
        size_t clen = curr->piece.length;
        const char *cend = cdata + clen;
        
        const char *ceol = find_next_newline(cdata, cend, &nl_len);
        if (ceol) {
            g_string_append_len(res, cdata, ceol - cdata + nl_len);
            break;
        } else {
            g_string_append_len(res, cdata, clen);
        }
    }
    
    *out_len = res->len;
    return g_string_free(res, FALSE);
}

size_t
piece_table_get_line_length(PieceTable *pt, size_t line_index)
{
    size_t start_lf, start_byte;
    PieceNode *node = find_node_for_line(pt, line_index, &start_lf, &start_byte);
    
    if (!node) return 0;
    
    size_t relative_lf = line_index - start_lf;
    const char *data = (node->piece.source == SOURCE_ORIGINAL) ? pt->orig_data : (char*)pt->add_buffer->data;
    data += node->piece.start;
    size_t len = node->piece.length;
    
    size_t internal_offset = 0;
    int nl_len;
    if (relative_lf > 0) {
        size_t found = 0;
        const char *ptr = data;
        const char *end = data + len;
        while (ptr < end && found < relative_lf) {
            const char *p = find_next_newline(ptr, end, &nl_len);
            if (!p) break;
            ptr = p + nl_len;
            found++;
        }
        internal_offset = ptr - data;
    }
    
    size_t total_len = 0;
    const char *ptr = data + internal_offset;
    const char *node_end = data + len;
    
    const char *eol = find_next_newline(ptr, node_end, &nl_len);
    if (eol) {
        return (eol - ptr + nl_len);
    }
    total_len += (node_end - ptr);
    
    PieceNode *curr = node;
    while (1) {
        if (curr->right) {
            curr = curr->right;
            while (curr->left) curr = curr->left;
        } else {
            PieceNode *p = curr->parent;
            while (p && curr == p->right) {
                curr = p;
                p = p->parent;
            }
            curr = p;
        }
        
        if (!curr) break;
        
        const char *cdata = (curr->piece.source == SOURCE_ORIGINAL) ? pt->orig_data : (char*)pt->add_buffer->data;
        cdata += curr->piece.start;
        size_t clen = curr->piece.length;
        const char *cend = cdata + clen;
        
        const char *ceol = find_next_newline(cdata, cend, &nl_len);
        if (ceol) {
            total_len += (ceol - cdata + nl_len);
            break;
        } else {
            total_len += clen;
        }
    }
    
    return total_len;
}

size_t
piece_table_get_line_of_offset(PieceTable *pt, size_t offset)
{
    /* Find node containing offset, summing LF of left subtrees + LF inside node up to split */
    size_t node_start;
    PieceNode *node = find_node_at_offset(pt, offset, &node_start);
    if (!node) {
        if (pt->root && offset >= pt->root->size_subtree)
            return pt->root->lf_subtree;
        return 0;
    }
    
    /* We need the path to sum Left subtrees.
       Splay implementation moves node to root.
       So Root->Left contains all preceding content.
       So line index = Root->Left->lf_subtree + LF inside Root before offset.
    */
    size_t lines_before = node->left ? node->left->lf_subtree : 0;
    
    /* Count newlines in node up to (offset - node_start) */
    size_t local_off = offset - node_start;
    const char *data = (node->piece.source == SOURCE_ORIGINAL) ? pt->orig_data : (char*)pt->add_buffer->data;
    size_t local_lf = count_newlines(data + node->piece.start, local_off);
    
    return lines_before + local_lf;
}

size_t
piece_table_get_offset_of_line(PieceTable *pt, size_t line_index)
{
    size_t start_lf, start_byte;
    PieceNode *node = find_node_for_line(pt, line_index, &start_lf, &start_byte);
    if (!node) {
        if (pt->root && line_index >= pt->root->lf_subtree)
            return pt->root->size_subtree;
        return 0;
    }
    
    /* node is root. node->left lines = start_lf.
       We want line_index.
       Lines to skip in node = line_index - start_lf.
       We need to find byte offset of Nth newline.
       If N=0, we are at start of node (start_byte).
       If N=1, we are after 1st newline.
    */
    
    size_t relative_lf = line_index - start_lf;
    size_t internal_offset = 0;
    
    const char *data = (node->piece.source == SOURCE_ORIGINAL) ? pt->orig_data : (char*)pt->add_buffer->data;
    /* Use direct pointer math */
    
    size_t len = node->piece.length;
    const char *p_data = data + node->piece.start;
    
    if (relative_lf > 0) {
        size_t found = 0;
        const char *ptr = p_data;
        const char *end = p_data + len;
        int nl_len;
        while (ptr < end && found < relative_lf) {
            const char *p = find_next_newline(ptr, end, &nl_len);
            if (!p) break;
            ptr = p + nl_len;
            found++;
        }
        internal_offset = ptr - p_data;
    }
    
    return start_byte + internal_offset;
}

/* -- Traversal -- */

static void
traverse_node_for_lines(PieceTable *pt, PieceNode *node, void (*func)(size_t len, void *user_data), void *user_data, size_t *acc_len)
{
    if (!node) return;
    
    traverse_node_for_lines(pt, node->left, func, user_data, acc_len);
    
    /* Process current piece */
    const char *data = (node->piece.source == SOURCE_ORIGINAL) ? pt->orig_data : (char*)pt->add_buffer->data;
    const char *ptr = data + node->piece.start;
    const char *end = ptr + node->piece.length;
    
    const char *scan = ptr;
    int nl_len;
    while (scan < end) {
        const char *nl = find_next_newline(scan, end, &nl_len);
        if (nl) {
            size_t seg_len = nl - scan;
            size_t full_len = *acc_len + seg_len + nl_len;
            
            func(full_len, user_data);
            
            *acc_len = 0;
            scan = nl + nl_len;
        } else {
            *acc_len += (end - scan);
            break;
        }
    }
    
    traverse_node_for_lines(pt, node->right, func, user_data, acc_len);
}

void
piece_table_foreach_line(PieceTable *pt, void (*func)(size_t line_len, void *user_data), void *user_data)
{
    if (!pt || !pt->root) return;
    
    size_t acc_len = 0;
    traverse_node_for_lines(pt, pt->root, func, user_data, &acc_len);
    
    /* If there is leftover text (file doesn't end in newline), emit it as final line */
    if (acc_len > 0) {
        func(acc_len, user_data);
    }
    /* Note: If file ends in newline, we don't emit an empty line here to avoid confusing line counting
       unless the editor logic specifically expects it. 
       Usually "A\n" -> 2 lines. 
       If we iterate, we emit 1 for "A\n". The second line is empty and implicit.
       Our loop in editor_widget expects total_lines.
       If total_lines > emitted_lines, we know the last one is empty. */
}
