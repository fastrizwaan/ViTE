/*
 * Compact Match Storage - Memory-mapped file-backed storage
 * 
 * Instead of storing 140M offsets in RAM (1.1GB), we:
 * 1. Create a temporary file
 * 2. mmap it for reading/writing
 * 3. Let OS handle paging to/from disk
 * 
 * This keeps matches on disk, using only what's needed in RAM.
 */

#ifndef COMPACT_MATCHES_H
#define COMPACT_MATCHES_H

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int fd;                 /* File descriptor for temp file */
    size_t *data;           /* mmap'd data - sorted match offsets */
    size_t count;           /* Number of matches */
    size_t capacity;        /* Allocated capacity (in elements) */
    size_t match_length;    /* Length of each match (for end offset) */
    char *filepath;         /* Path to temp file */
} CompactMatches;

/* Create/destroy */
CompactMatches *compact_matches_new(size_t match_length);
void compact_matches_free(CompactMatches *cm);

/* Adding matches (must be in sorted order!) */
void compact_matches_append(CompactMatches *cm, size_t start);

/* Query */
size_t compact_matches_count(CompactMatches *cm);
size_t compact_matches_memory_usage(CompactMatches *cm);

/* Binary search: find indices of matches in range [start_offset, end_offset) */
void compact_matches_find_range(CompactMatches *cm, size_t start_offset, size_t end_offset,
                                 size_t *out_first_idx, size_t *out_last_idx);

/* Get match at index */
bool compact_matches_get(CompactMatches *cm, size_t index, size_t *start, size_t *end);

/* Convert range to GArray for UI (only converts visible portion!) */
GArray *compact_matches_range_to_array(CompactMatches *cm, size_t first_idx, size_t last_idx);

/* Convert ALL to GArray (expensive! only use when needed) */
GArray *compact_matches_to_array(CompactMatches *cm);

#endif /* COMPACT_MATCHES_H */
