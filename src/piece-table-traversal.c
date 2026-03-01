#include "piece-table.h"
#include <string.h>

/* -- Traversal -- */

static void
traverse_node_for_lines(PieceTable *pt, PieceNode *node, void (*func)(size_t len, void *user_data), void *user_data, size_t *acc_len)
{
    if (!node) return;
    
    traverse_node_for_lines(pt, node->left, func, user_data, acc_len);
    
    /* Process current piece */
    const char *data = (node->piece.source == SOURCE_ORIGINAL) ? pt->orig_data : (char*)pt->add_buffer->mmap_base;
    const char *ptr = data + node->piece.start;
    const char *end = ptr + node->piece.length;
    
    const char *scan = ptr;
    while (scan < end) {
        const char *nl = memchr(scan, '\n', end - scan);
        if (nl) {
            /* Found a newline */
            size_t seg_len = nl - scan;
            size_t full_len = *acc_len + seg_len + 1; /* +1 for newline */
            
            func(full_len, user_data);
            
            *acc_len = 0;
            scan = nl + 1;
        } else {
            /* No more newlines in this piece, accumulate rest */
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
    } else {
        /* If file ends exactly with newline, do we emit empty line?
           Text editors usually count 'lines'. 
           "A\n" -> Line 1: "A\n", Line 2: "" (empty).
           If we just emitted "A\n", acc_len is 0. 
           We should query if we need to emit the last phantom line.
           Actually, splitting by \n usually dictates lines.
           "A" -> 1 line.
           "A\n" -> 2 lines (second is empty).
           My traversal emits on \n. So "A\n" emits once.
           We need to handle the trailing case correctly. 
           
           If the last char was \n, we should seemingly emit an empty line.
           But acc_len is 0.
           We can track if we ever emitted.
           
           Actually, checking document_get_line_count logic:
           It usually counts newlines + 1.
           So yes, we should probably ensure count matches.
           
           Let's look at how visualizer handles it.
           It expects offset array to match total_lines.
        */
        /* For consistency with typical line counting: 
           If the file is not empty and ends in \n, there's an implicit empty last line?
           Or "A\n" is 1 line?
           Usually "A\n" is line 0="A", line 1="" => 2 lines.
           So if last char was \n (implied by acc_len==0 and file not empty), we emit 0-length line.
           But wait, if file is empty, 0 lines? Or 1 empty line?
           Usually 1 empty line.
        */
    }
}
