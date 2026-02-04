/*
 * Compact Match Storage - Memory-mapped file-backed storage
 * 
 * Uses mmap to store matches on disk instead of RAM.
 * OS handles paging - only actively used pages stay in RAM.
 * 
 * For 140M matches:
 * - File size: 1.1GB (on disk)
 * - RAM usage: only pages being accessed (~few MB)
 */

#include "compact-matches.h"
#include "document.h"
#include <string.h>
#include <stdio.h>
#include "resource-check.h"
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>

#define INITIAL_CAPACITY (64 * 1024)  /* 64K entries = 512KB initial file */
#define GROWTH_FACTOR 2

CompactMatches *compact_matches_new(size_t match_length) {
    CompactMatches *cm = g_new0(CompactMatches, 1);
    cm->match_length = match_length;
    cm->count = 0;
    cm->capacity = INITIAL_CAPACITY;
    cm->data = NULL;
    cm->fd = -1;
    cm->filepath = NULL;
    
    /* Create temporary file */
    char *path = g_strdup("/tmp/vite_matches_XXXXXX");
    cm->fd = mkstemp(path);
    if (cm->fd < 0) {
        g_warning("Failed to create temp file for matches: %s", strerror(errno));
        g_free(path);
        g_free(cm);
        return NULL;
    }
    
    /* Unlink immediately for auto-cleanup */
    unlink(path);
    g_free(path);
    cm->filepath = NULL; /* No longer needed */
    
    /* Pre-allocate file size */
    size_t file_size = cm->capacity * sizeof(size_t);
    
    if (!resource_can_write_disk("/tmp", file_size)) {
        g_warning("compact_matches_new: Insufficient disk space for matches");
        close(cm->fd);
        g_free(cm);
        return NULL;
    }
    
    if (ftruncate(cm->fd, file_size) < 0) {
        g_warning("Failed to resize temp file: %s", strerror(errno));
        close(cm->fd);
        g_free(cm);
        return NULL;
    }
    
    /* mmap the file */
    cm->data = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, cm->fd, 0);
    if (cm->data == MAP_FAILED) {
        g_warning("Failed to mmap temp file: %s", strerror(errno));
        cm->data = NULL;
        close(cm->fd);
        g_free(cm);
        return NULL;
    }
    
    /* Advise kernel about sequential access pattern */
    madvise(cm->data, file_size, MADV_SEQUENTIAL);
    
    return cm;
}

void compact_matches_free(CompactMatches *cm) {
    if (!cm) return;
    
    if (cm->data && cm->data != MAP_FAILED) {
        munmap(cm->data, cm->capacity * sizeof(size_t));
        cm->data = NULL;
    }
    
    if (cm->fd >= 0) {
        close(cm->fd);
        cm->fd = -1;
    }
    
    /* File already unlinked */
    if (cm->filepath) {
        g_free(cm->filepath);
        cm->filepath = NULL;
    }
    
    g_free(cm);
}

static gboolean compact_matches_grow(CompactMatches *cm) {
    if (!cm || !cm->data || cm->fd < 0) return FALSE;
    
    size_t old_size = cm->capacity * sizeof(size_t);
    size_t new_capacity = cm->capacity * GROWTH_FACTOR;
    size_t new_size = new_capacity * sizeof(size_t);
    
    /* Check disk space before growing */
    if (!resource_can_write_disk("/tmp", new_size - old_size)) {
        g_warning("compact_matches_grow: Insufficient disk space");
        return FALSE;
    }

    /* Resize file */
    if (ftruncate(cm->fd, new_size) < 0) {
        g_warning("ftruncate failed: %s", strerror(errno));
        return FALSE;
    }
    
    /* Re-mmap with new size (keep old mapping until new one is ready) */
    void *new_map = mmap(NULL, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, cm->fd, 0);
    if (new_map == MAP_FAILED) {
        g_warning("mmap failed after grow: %s", strerror(errno));
        /* Best-effort rollback to old size; keep old mapping intact */
        ftruncate(cm->fd, old_size);
        return FALSE;
    }
    
    /* Swap mappings */
    if (munmap(cm->data, old_size) < 0) {
        g_warning("munmap failed after grow: %s", strerror(errno));
        /* Keep new mapping anyway to avoid invalid state */
    }
    cm->data = new_map;
    cm->capacity = new_capacity;
    return TRUE;
}

void compact_matches_append(CompactMatches *cm, size_t start) {
    if (!cm || !cm->data) return;
    
    /* Grow if needed */
    if (cm->count >= cm->capacity) {
        if (!compact_matches_grow(cm)) {
            return;  /* Can't grow, drop the match */
        }
    }
    
    cm->data[cm->count++] = start;
}

size_t compact_matches_count(CompactMatches *cm) {
    return cm ? cm->count : 0;
}

size_t compact_matches_memory_usage(CompactMatches *cm) {
    /* Return actual RAM usage - minimal since mmap'd! */
    if (!cm) return 0;
    return sizeof(CompactMatches) + 4096;  /* Just struct + ~1 page overhead */
}

/* Binary search: find first index where data[i] >= target */
static size_t lower_bound(size_t *data, size_t count, size_t target) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (data[mid] < target) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

void compact_matches_find_range(CompactMatches *cm, size_t start_offset, size_t end_offset,
                                 size_t *out_first_idx, size_t *out_last_idx) {
    if (!cm || cm->count == 0 || !cm->data) {
        if (out_first_idx) *out_first_idx = 0;
        if (out_last_idx) *out_last_idx = 0;
        return;
    }
    
    /* Advise kernel we're about to do random access for binary search */
    madvise(cm->data, cm->count * sizeof(size_t), MADV_RANDOM);
    
    /* Find first match where start >= start_offset */
    size_t first = lower_bound(cm->data, cm->count, start_offset);
    
    /* Find last match where start < end_offset */
    size_t last = lower_bound(cm->data, cm->count, end_offset);
    
    if (out_first_idx) *out_first_idx = first;
    if (out_last_idx) *out_last_idx = last;  /* exclusive end */
}

bool compact_matches_get(CompactMatches *cm, size_t index, size_t *start, size_t *end) {
    if (!cm || !cm->data || index >= cm->count) return false;
    
    if (start) *start = cm->data[index];
    if (end) *end = cm->data[index] + cm->match_length;
    return true;
}

GArray *compact_matches_range_to_array(CompactMatches *cm, size_t first_idx, size_t last_idx) {
    if (!cm || !cm->data || first_idx >= last_idx || first_idx >= cm->count) return NULL;
    
    if (last_idx > cm->count) last_idx = cm->count;
    size_t range_count = last_idx - first_idx;
    
    GArray *arr = g_array_sized_new(FALSE, FALSE, sizeof(SearchMatch), range_count);
    
    for (size_t i = first_idx; i < last_idx; i++) {
        SearchMatch m = { cm->data[i], cm->data[i] + cm->match_length };
        g_array_append_val(arr, m);
    }
    
    return arr;
}

GArray *compact_matches_to_array(CompactMatches *cm) {
    if (!cm || cm->count == 0) return NULL;
    return compact_matches_range_to_array(cm, 0, cm->count);
}
