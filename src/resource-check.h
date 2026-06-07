/*
 * resource-check.h - System resource monitoring for safe allocations
 * 
 * Provides utilities to check available RAM/disk before large allocations
 * to prevent crashes under memory pressure and detect integer overflows.
 */

#ifndef RESOURCE_CHECK_H
#define RESOURCE_CHECK_H

#include <stddef.h>
#include <glib.h>

/* Get available RAM in bytes (reads /proc/meminfo) */
size_t resource_get_available_ram(void);

/* Get available disk space in bytes for a given path */
size_t resource_get_available_disk(const char *path);

/* Check if allocation of 'size' bytes is safe (won't exhaust RAM) */
gboolean resource_can_allocate(size_t size);

/* Check if writing 'size' bytes to disk at path is safe */
gboolean resource_can_write_disk(const char *path, size_t size);

/* Validate that size doesn't look like integer overflow */
gboolean resource_size_valid(size_t size);

/* Safe malloc that returns NULL if size is too large or invalid */
void *resource_safe_malloc(size_t size);

/* Safe realloc with overflow checks */
void *resource_safe_realloc(void *ptr, size_t old_size, size_t new_size);

/* Safe GString allocator */
GString *resource_safe_g_string_sized_new(size_t dfl_size);

/* Get safe, disk-backed cache directory for zero-RAM operations */
const char *resource_get_vite_cache_dir(void);
void resource_cleanup_vite_cache(void);
/* Safe malloc0 that returns NULL if size is too large or invalid */
void *resource_safe_malloc0(size_t size);

#endif /* RESOURCE_CHECK_H */
