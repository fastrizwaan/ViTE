/*
 * resource-check.c - System resource monitoring for safe allocations
 * 
 * Provides utilities to check available RAM/disk before large allocations
 * to prevent crashes under memory pressure and detect integer overflows.
 */

#include "resource-check.h"
#include <sys/statvfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Maximum fraction of available RAM we'll allocate in one go (50%) */
/* Maximum fraction of available RAM we'll allocate in one go (90%) */
#define MAX_ALLOC_FRACTION 0.9

/* Absolute maximum single allocation removed - trusting system stats */
/* #define ABSOLUTE_MAX_ALLOC (2ULL * 1024 * 1024 * 1024) */

/* Threshold for "looks like integer overflow" - anything > 2^62 is suspicious */
#define OVERFLOW_THRESHOLD (1ULL << 62)

/* Minimum RAM to leave free (100MB) */
#define MIN_FREE_RAM (100ULL * 1024 * 1024)

/* Minimum disk space to leave free (500MB) */
#define MIN_FREE_DISK (500ULL * 1024 * 1024)

size_t
resource_get_available_ram(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    
    char line[256];
    size_t available = 0;
    
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemAvailable:", 13) == 0) {
            unsigned long kb = 0;
            if (sscanf(line + 13, "%lu", &kb) == 1) {
                available = (size_t)kb * 1024; /* Convert KB to bytes */
            }
            break;
        }
    }
    
    fclose(f);
    return available;
}

size_t
resource_get_available_disk(const char *path)
{
    if (!path) path = "/tmp";
    
    struct statvfs stat;
    if (statvfs(path, &stat) != 0) {
        return 0;
    }
    
    /* f_bavail = blocks available to non-root, f_frsize = fragment size */
    return (size_t)stat.f_bavail * (size_t)stat.f_frsize;
}

gboolean
resource_size_valid(size_t size)
{
    /* Detect obvious integer overflow - any value > 2^62 is suspicious */
    return size < OVERFLOW_THRESHOLD;
}

gboolean
resource_can_allocate(size_t size)
{
    /* First check for overflow */
    if (!resource_size_valid(size)) {
        g_debug("resource_can_allocate: Size %zu looks like overflow", size);
        return FALSE;
    }
    
    /* Check against absolute maximum */
    
    /* Check against absolute maximum - DISABLED */
    /* if (size > ABSOLUTE_MAX_ALLOC) {
        g_debug("resource_can_allocate: Size %zu exceeds absolute max", size);
        return FALSE;
    } */
    
    /* Check available RAM */
    size_t available = resource_get_available_ram();
    if (available == 0) {
        /* Can't read /proc/meminfo - optimistically allow but log */
        g_debug("resource_can_allocate: Cannot read available RAM, allowing");
        return TRUE;
    }
    
    /* Don't use more than MAX_ALLOC_FRACTION of available RAM */
    size_t max_alloc = (size_t)(available * MAX_ALLOC_FRACTION);
    
    /* Also ensure we leave MIN_FREE_RAM after allocation */
    if (size > available - MIN_FREE_RAM) {
        g_debug("resource_can_allocate: Would leave too little free RAM");
        return FALSE;
    }
    
    if (size > max_alloc) {
        g_debug("resource_can_allocate: Size %zu exceeds safe allocation limit %zu", size, max_alloc);
        return FALSE;
    }
    
    return TRUE;
}

gboolean
resource_can_write_disk(const char *path, size_t size)
{
    if (!resource_size_valid(size)) {
        return FALSE;
    }
    
    size_t available = resource_get_available_disk(path);
    if (available == 0) {
        /* Can't check - optimistically allow */
        return TRUE;
    }
    
    /* Ensure we leave MIN_FREE_DISK after write */
    if (size > available - MIN_FREE_DISK) {
        g_debug("resource_can_write_disk: Would leave too little free disk space");
        return FALSE;
    }
    
    return TRUE;
}

void *
resource_safe_malloc(size_t size)
{
    if (!resource_can_allocate(size)) {
        g_warning("resource_safe_malloc: Refusing to allocate %zu bytes", size);
        return NULL;
    }
    
    void *ptr = g_try_malloc(size);
    if (!ptr && size > 0) {
        g_warning("resource_safe_malloc: g_try_malloc failed for %zu bytes", size);
    }
    
    return ptr;
}

void *
resource_safe_realloc(void *ptr, size_t old_size, size_t new_size)
{
    (void)old_size; /* May be used for additional checks in future */
    
    if (!resource_can_allocate(new_size)) {
        g_warning("resource_safe_realloc: Refusing to reallocate to %zu bytes", new_size);
        return NULL;
    }
    
    void *new_ptr = g_try_realloc(ptr, new_size);
    if (!new_ptr && new_size > 0) {
        g_warning("resource_safe_realloc: g_try_realloc failed for %zu bytes", new_size);
    }
    
    return new_ptr;
}
