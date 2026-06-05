/*
 * vite-clipboard.c - Reference-based clipboard implementation
 * 
 * Key optimizations:
 * 1. Copy is O(1) - just stores document reference + offsets
 * 2. Paste streams data in chunks to keep UI responsive
 * 3. System clipboard sync is deferred until external paste
 * 4. Version tracking detects stale references
 */

#include "vite-clipboard.h"
#include "resource-check.h"
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>

/* Chunk size for streaming paste (1MB) */
#define PASTE_CHUNK_SIZE (1024 * 1024)

/* Maximum size to sync to system clipboard (100MB) */
#define MAX_SYSTEM_CLIPBOARD_SIZE (100 * 1024 * 1024)

/* Streaming paste context */
struct _ViteStreamingPaste {
    ViteClipboard *clip;
    Document *target;
    size_t target_offset;
    size_t bytes_pasted;
    size_t total_bytes;
    
    /* For FILE based paste */
    int source_fd;
    
    ViteClipboardProgressCallback progress_cb;
    gpointer user_data;
    
    guint idle_id;
    gboolean cancelled;
    gboolean in_undo_group;
};

static ViteClipboard *global_clipboard = NULL;

ViteClipboard *
vite_clipboard_get_default(void)
{
    if (!global_clipboard) {
        global_clipboard = g_new0(ViteClipboard, 1);
    }
    return global_clipboard;
}

static void
free_entry(ViteClipboardEntry *entry)
{
    if (!entry) return;
    g_free(entry->cached_text);
    g_free(entry->source_file_path);
    if (entry->persisted_file_path) {
        unlink(entry->persisted_file_path);
        g_free(entry->persisted_file_path);
    }
    g_free(entry);
}

void
vite_clipboard_free(ViteClipboard *clip)
{
    if (!clip) return;
    
    vite_clipboard_cancel_streaming(clip);
    free_entry(clip->current);
    
    if (clip == global_clipboard) {
        global_clipboard = NULL;
    }
    
    g_free(clip);
}

void
vite_clipboard_set_reference(ViteClipboard *clip, Document *doc,
                              size_t start, size_t end, gboolean is_cut)
{
    if (!clip || !doc || start >= end) return;
    
    /* Free old entry */
    free_entry(clip->current);
    
    ViteClipboardEntry *entry = g_new0(ViteClipboardEntry, 1);
    entry->type = VITE_CLIPBOARD_ENTRY_REFERENCE;
    entry->source_doc = doc;
    entry->source_file_path = g_strdup(document_get_file_path(doc));
    entry->start_offset = start;
    entry->end_offset = end;
    entry->source_version = document_get_version(doc);
    entry->is_valid = TRUE;
    entry->is_cut = is_cut;
    entry->cached_text = NULL;
    entry->cached_len = 0;
    
    clip->current = entry;
    clip->system_sync_pending = TRUE;
    
    g_debug("vite_clipboard: Set reference [%zu, %zu) from doc version %lu",
            start, end, (unsigned long)entry->source_version);
}

void
vite_clipboard_persist_to_file(ViteClipboard *clip)
{
    if (!clip || !clip->current) return;
    
    ViteClipboardEntry *entry = clip->current;
    
    /* Only valid for REFERENCE type that is currently valid */
    if (entry->type != VITE_CLIPBOARD_ENTRY_REFERENCE || !entry->is_valid || !entry->source_doc) {
        return;
    }
    
    size_t len = entry->end_offset - entry->start_offset;
    if (len == 0) return;
    
    /* Create temp file */
    char *fname = g_build_filename(resource_get_vite_cache_dir(), "vite-clipboard-XXXXXX", NULL);
    int fd = mkstemp(fname);
    if (fd == -1) {
        g_warning("Failed to create persistence file: %s", strerror(errno));
        g_free(fname);
        return;
    }
    
    g_debug("vite_clipboard: Persisting %zu bytes to %s", len, fname);
    
    if (!resource_can_write_disk(resource_get_vite_cache_dir(), len)) {
        g_warning("vite_clipboard: Insufficient disk space to persist clipboard");
        close(fd);
        unlink(fname);
        g_free(fname);
        return;
    }
    
    size_t written = 0;
    size_t chunk_size = 1024 * 1024;
    gboolean success = TRUE;
    
    while (written < len) {
        size_t to_write = len - written;
        if (to_write > chunk_size) to_write = chunk_size;
        
        char *chunk = document_get_text_range(entry->source_doc, entry->start_offset + written, to_write);
        if (!chunk) {
            success = FALSE;
            break;
        }
        
        ssize_t res = write(fd, chunk, to_write);
        g_free(chunk);
        
        if (res != (ssize_t)to_write) {
            success = FALSE;
            break;
        }
        written += to_write;
    }
    
    close(fd);
    
    if (success) {
        /* Update entry to FILE type */
        entry->type = VITE_CLIPBOARD_ENTRY_FILE;
        entry->persisted_file_path = fname;
        /* Clear reference */
        entry->source_doc = NULL;
        /* is_valid remains TRUE, as file is static */
        g_debug("vite_clipboard: Persistence successful, switched to FILE mode");
        g_warning("vite_clipboard: Persistence failed");
        unlink(fname);
        g_free(fname);
    }
}

/* Async Copy Implementation */
struct _ViteClipboardCopyTask {
    ViteClipboard *clip;
    Document *doc;
    size_t start;
    size_t end;
    gboolean is_cut;
    ViteClipboardProgressCallback cb;
    gpointer user_data;
    guint idle_id;
    int fd;
    char *path;
    size_t written;
};

static gboolean
vite_clipboard_copy_idle_step(gpointer user_data)
{
    ViteClipboardCopyTask *task = user_data;
    size_t len = task->end - task->start;
    
    gint64 start_time = g_get_monotonic_time();
    size_t chunk_size = 256 * 1024; /* 256KB chunks */
    
    while (task->written < len) {
        size_t to_write = len - task->written;
        if (to_write > chunk_size) to_write = chunk_size;
        
        char *chunk = document_get_text_range(task->doc, task->start + task->written, to_write);
        if (!chunk) {
            g_warning("vite_clipboard_copy_idle_step: allocation failed");
            break; /* Error out */
        }
        
        ssize_t res = write(task->fd, chunk, to_write);
        g_free(chunk);
        
        if (res != (ssize_t)to_write) {
            g_warning("vite_clipboard_copy_idle_step: write failed");
            break;
        }
        
        task->written += to_write;
        
        if (g_get_monotonic_time() - start_time >= 10000) {
            if (task->cb) task->cb(task->written, len, task->user_data);
            return G_SOURCE_CONTINUE;
        }
    }
    
    close(task->fd);
    
    if (task->written == len) {
        if (task->clip->current) {
            free_entry(task->clip->current);
        }
        
        ViteClipboardEntry *entry = resource_safe_malloc0(sizeof(ViteClipboardEntry));
        entry->type = VITE_CLIPBOARD_ENTRY_FILE;
        entry->persisted_file_path = task->path; /* takes ownership */
        entry->start_offset = 0;
        entry->end_offset = len;
        entry->is_valid = TRUE;
        entry->is_cut = task->is_cut;
        
        task->clip->current = entry;
    } else {
        unlink(task->path);
        g_free(task->path);
    }
    
    if (task->cb) task->cb(len, len, task->user_data);
    
    task->idle_id = 0;
    g_free(task);
    return G_SOURCE_REMOVE;
}

ViteClipboardCopyTask *
vite_clipboard_copy_async(ViteClipboard *clip, Document *doc, size_t start, size_t end, gboolean is_cut, ViteClipboardProgressCallback cb, gpointer user_data)
{
    if (!clip || !doc || start >= end) return NULL;
    
    size_t len = end - start;
    if (!resource_can_write_disk(resource_get_vite_cache_dir(), len)) {
        g_warning("vite_clipboard_copy_async: Insufficient disk space");
        return NULL;
    }
    
    char template[] = "vite-clip-XXXXXX";
    char *temp_dir = g_build_filename(resource_get_vite_cache_dir(), NULL);
    g_mkdir_with_parents(temp_dir, 0700);
    char *full_template = g_build_filename(temp_dir, template, NULL);
    int fd = mkstemp(full_template);
    g_free(temp_dir);
    
    if (fd == -1) {
        g_warning("vite_clipboard_copy_async: mkstemp failed");
        g_free(full_template);
        return NULL;
    }
    
    ViteClipboardCopyTask *task = g_new0(ViteClipboardCopyTask, 1);
    task->clip = clip;
    task->doc = doc;
    task->start = start;
    task->end = end;
    task->is_cut = is_cut;
    task->cb = cb;
    task->user_data = user_data;
    task->fd = fd;
    task->path = full_template;
    
    task->idle_id = g_idle_add(vite_clipboard_copy_idle_step, task);
    return task;
}

void
vite_clipboard_copy_async_cancel(ViteClipboardCopyTask *task)
{
    if (!task) return;
    if (task->idle_id) g_source_remove(task->idle_id);
    close(task->fd);
    unlink(task->path);
    g_free(task->path);
    g_free(task);
}

gboolean
vite_clipboard_has_internal_content(ViteClipboard *clip)
{
    if (!clip || !clip->current) return FALSE;
    
    if (clip->current->type == VITE_CLIPBOARD_ENTRY_REFERENCE) {
        return vite_clipboard_is_reference_valid(clip);
    }
    
    return clip->current->is_valid;
}

size_t
vite_clipboard_get_content_size(ViteClipboard *clip)
{
    if (!clip || !clip->current) return 0;
    return clip->current->end_offset - clip->current->start_offset;
}

gboolean
vite_clipboard_is_reference_valid(ViteClipboard *clip)
{
    if (!clip || !clip->current || !clip->current->is_valid) return FALSE;
    
    ViteClipboardEntry *entry = clip->current;
    
    if (entry->type == VITE_CLIPBOARD_ENTRY_FILE) {
        return TRUE; /* Files persist */
    }
    
    if (entry->type == VITE_CLIPBOARD_ENTRY_REFERENCE) {
        /* Check if source document still exists and hasn't changed */
        if (!entry->source_doc) return FALSE;
        
        uint64_t current_version = document_get_version(entry->source_doc);
        if (current_version != entry->source_version) {
            g_debug("vite_clipboard: Reference invalidated (version %lu -> %lu)",
                    (unsigned long)entry->source_version, (unsigned long)current_version);
            entry->is_valid = FALSE;
            return FALSE;
        }
        return TRUE;
    }
    
    return TRUE;
}

/* Streaming paste idle callback */
static gboolean
streaming_paste_chunk(gpointer user_data)
{
    ViteStreamingPaste *paste = user_data;
    
    if (paste->cancelled) {
        if (paste->in_undo_group) {
            document_end_undo_group(paste->target);
        }
        if (paste->source_fd >= 0) close(paste->source_fd);
        g_free(paste);
        return G_SOURCE_REMOVE;
    }
    
    ViteClipboardEntry *entry = paste->clip->current;
    
    if (!entry || !entry->is_valid || paste->bytes_pasted >= paste->total_bytes) {
        /* Done - finalize */
        if (paste->in_undo_group) {
            document_end_undo_group(paste->target);
        }
        
        if (paste->progress_cb) {
            paste->progress_cb(paste->total_bytes, paste->total_bytes, paste->user_data);
        }
        
        if (paste->source_fd >= 0) close(paste->source_fd);
        paste->clip->active_paste = NULL;
        g_free(paste);
        return G_SOURCE_REMOVE;
    }
    
    /* Calculate chunk to paste */
    size_t remaining = paste->total_bytes - paste->bytes_pasted;
    size_t chunk_size = MIN(remaining, PASTE_CHUNK_SIZE);
    
    char *chunk = NULL;
    
    if (entry->type == VITE_CLIPBOARD_ENTRY_FILE) {
        /* Read from file */
        if (paste->source_fd >= 0) {
            /* Resource check before allocation */
            if (!resource_can_allocate(chunk_size)) {
                g_warning("vite_clipboard: Low memory during paste, reducing chunk size");
                chunk_size = MIN(chunk_size, 256 * 1024); /* Reduce to 256KB */
                if (!resource_can_allocate(chunk_size)) {
                    g_warning("vite_clipboard: Cannot allocate even reduced chunk");
                    if (paste->in_undo_group) {
                        document_end_undo_group(paste->target);
                    }
                    if (paste->source_fd >= 0) close(paste->source_fd);
                    paste->clip->active_paste = NULL;
                    g_free(paste);
                    return G_SOURCE_REMOVE;
                }
            }
            chunk = g_try_malloc(chunk_size);
            if (!chunk) {
                g_warning("vite_clipboard: Failed to allocate chunk");
                if (paste->in_undo_group) {
                    document_end_undo_group(paste->target);
                }
                if (paste->source_fd >= 0) close(paste->source_fd);
                paste->clip->active_paste = NULL;
                g_free(paste);
                return G_SOURCE_REMOVE;
            }
            ssize_t r = read(paste->source_fd, chunk, chunk_size);
            if (r != (ssize_t)chunk_size) {
                g_free(chunk);
                chunk = NULL; /* Error or EOF */
            }
        }
    } else if (entry->type == VITE_CLIPBOARD_ENTRY_REFERENCE) {
        /* Read from doc */
        size_t src_offset = entry->start_offset + paste->bytes_pasted;
        chunk = document_get_text_range(entry->source_doc, src_offset, chunk_size);
    }
    
    if (chunk) {
        document_insert(paste->target, 
                        paste->target_offset + paste->bytes_pasted, 
                        chunk, chunk_size);
        g_free(chunk);
        paste->bytes_pasted += chunk_size;
        
        /* Report progress */
        if (paste->progress_cb) {
            paste->progress_cb(paste->bytes_pasted, paste->total_bytes, paste->user_data);
        }
    } else {
        /* Error reading source - abort */
        g_warning("vite_clipboard: Failed to read chunk");
        if (paste->in_undo_group) {
            document_end_undo_group(paste->target);
        }
        if (paste->source_fd >= 0) close(paste->source_fd);
        paste->clip->active_paste = NULL;
        g_free(paste);
        return G_SOURCE_REMOVE;
    }
    
    /* Continue if more to paste */
    return G_SOURCE_CONTINUE;
}

struct _PasteAsyncData {
    Document *target;
    ViteClipboardProgressCallback cb;
    gpointer user_data;
    size_t total;
    int fd;
};

static void
on_insert_from_fd_async_progress(double progress, gboolean finished, gpointer user_data)
{
    struct _PasteAsyncData *pad = user_data;
    if (pad->cb) {
        pad->cb(progress * pad->total, pad->total, pad->user_data);
    }
    if (finished) {
        document_end_undo_group(pad->target);
        close(pad->fd);
        g_free(pad);
    }
}

void
vite_clipboard_paste_streaming(ViteClipboard *clip, Document *target,
                                size_t offset,
                                ViteClipboardProgressCallback progress_cb,
                                gpointer user_data)
{
    if (!clip || !target) return;
    
    /* Cancel any existing streaming paste */
    vite_clipboard_cancel_streaming(clip);
    
    if (!vite_clipboard_has_internal_content(clip)) {
        g_debug("vite_clipboard: Content not valid for streaming paste");
        return;
    }
    
    ViteClipboardEntry *entry = clip->current;
    size_t total = entry->end_offset - entry->start_offset;
    
    if (total == 0) return;
    
    /* Zero-RAM Strategy for Large Pastes (> 50MB) */
    /* If content is huge, we avoid loading it into RAM (add_buffer).
       Instead, we ensure it's backed by a file (persisted), and then map it.
    */
    size_t ZERO_RAM_THRESHOLD = 50 * 1024 * 1024;
    
    if (total >= ZERO_RAM_THRESHOLD) {
        g_debug("vite_clipboard: Using Zero-RAM paste strategy for %zu bytes", total);
        
        /* 1. Ensure file backing */
        if (entry->type == VITE_CLIPBOARD_ENTRY_REFERENCE) {
            /* This might take a moment (synchronous write), but prevents OOM */
             vite_clipboard_persist_to_file(clip);
        }
        
        if (entry->type == VITE_CLIPBOARD_ENTRY_FILE && entry->persisted_file_path) {
            int fd = open(entry->persisted_file_path, O_RDONLY);
            if (fd >= 0) {
                 /* 2. Insert using async chunking */
                 document_begin_undo_group(target);
                 struct _PasteAsyncData *pad = g_new0(struct _PasteAsyncData, 1);
                 pad->target = target;
                 pad->cb = progress_cb;
                 pad->user_data = user_data;
                 pad->total = total;
                 pad->fd = fd;
                 document_insert_from_fd_async(target, offset, fd, total, on_insert_from_fd_async_progress, pad);
                 return;
            } else {
                 g_warning("vite_clipboard: Failed to open persisted file for Zero-RAM paste");
                 /* Fallback to streaming if open failed */
            }
        }
    }
    
    /* Standard Streaming Paste (RAM buffer) */
    
    /* Create streaming paste context */
    ViteStreamingPaste *paste = g_new0(ViteStreamingPaste, 1);
    paste->clip = clip;
    paste->target = target;
    paste->target_offset = offset;
    paste->bytes_pasted = 0;
    paste->total_bytes = total;
    paste->progress_cb = progress_cb;
    paste->user_data = user_data;
    paste->cancelled = FALSE;
    paste->in_undo_group = TRUE;
    paste->source_fd = -1;
    
    if (entry->type == VITE_CLIPBOARD_ENTRY_FILE) {
        paste->source_fd = open(entry->persisted_file_path, O_RDONLY);
        if (paste->source_fd < 0) {
            g_warning("Failed to open clipboard file: %s", entry->persisted_file_path);
            g_free(paste);
            return;
        }
    }
    
    /* Begin undo group for entire paste */
    document_begin_undo_group(target);
    
    clip->active_paste = paste;
    
    /* Schedule idle handler for chunked paste */
    paste->idle_id = g_idle_add(streaming_paste_chunk, paste);
}

gboolean
vite_clipboard_paste_sync(ViteClipboard *clip, Document *target,
                           size_t offset, size_t *out_pasted_len)
{
    if (out_pasted_len) *out_pasted_len = 0;
    if (!clip || !target) return FALSE;
    
    if (!vite_clipboard_has_internal_content(clip)) {
        return FALSE;
    }
    
    ViteClipboardEntry *entry = clip->current;
    
    /* Overflow/underflow check on size calculation */
    if (entry->end_offset < entry->start_offset) {
        g_warning("vite_clipboard: Invalid offset range [%zu, %zu) - likely overflow", 
                  entry->start_offset, entry->end_offset);
        return FALSE;
    }
    
    size_t len = entry->end_offset - entry->start_offset;
    
    if (len == 0) return TRUE;
    
    /* Validate size is not corrupted */
    if (!resource_size_valid(len)) {
        g_warning("vite_clipboard: Invalid size %zu (likely overflow)", len);
        return FALSE;
    }
    
    /* Zero-RAM Strategy: Always persist to file first, then use mmap insert */
    
    /* Ensure content is file-backed */
    if (entry->type == VITE_CLIPBOARD_ENTRY_REFERENCE) {
        vite_clipboard_persist_to_file(clip);
        /* Re-check after persist */
        if (entry->type != VITE_CLIPBOARD_ENTRY_FILE || !entry->persisted_file_path) {
            g_warning("vite_clipboard: Failed to persist for zero-RAM paste");
            return FALSE;
        }
    }
    
    /* Now we have file-backed content - use Zero-RAM insert */
    if (entry->type == VITE_CLIPBOARD_ENTRY_FILE && entry->persisted_file_path) {
        int fd = open(entry->persisted_file_path, O_RDONLY);
        if (fd >= 0) {
            document_insert_from_fd(target, offset, fd, len);
            close(fd);
            if (out_pasted_len) *out_pasted_len = len;
            return TRUE;
        }
        g_warning("vite_clipboard: Failed to open persisted file: %s", entry->persisted_file_path);
    }
    
    /* Fallback for edge cases (should not normally reach here) */
    g_warning("vite_clipboard: Falling back to RAM-based paste");
    char *text = document_get_text_range(entry->source_doc, entry->start_offset, len);
    if (!text) return FALSE;
    
    document_insert(target, offset, text, len);
    g_free(text);
    
    if (out_pasted_len) *out_pasted_len = len;
    return TRUE;
}

void
vite_clipboard_sync_to_system(ViteClipboard *clip)
{
    if (!clip || !clip->system_sync_pending) return;
    
    if (!clip->current || !clip->current->is_valid) {
        clip->system_sync_pending = FALSE;
        return;
    }
    
    ViteClipboardEntry *entry = clip->current;
    size_t len = entry->end_offset - entry->start_offset;
    
    /* Guard against invalid sizes (underflow/overflow) */
    if (!resource_size_valid(len)) {
        g_warning("vite_clipboard: Invalid content size %zu", len);
        clip->system_sync_pending = FALSE;
        return;
    }
    
    /* For huge content, use file-backed clipboard scheme */
    if (len > MAX_SYSTEM_CLIPBOARD_SIZE) {
        /* If we are already FILE backed, use that path directly? */
        /* Or copy to new temp file? */
        /* Let's be safe and copy to a specific temp file for system exchange if needed,
           or just reuse our internal file if we want to expose it.
           Safety: if we expose internal file path, other apps might read it.
           We'll stick to the existing "vite-temp://" protocol.
        */
        
        char *path = NULL;
        gboolean create_temp = TRUE;
        
        if (entry->type == VITE_CLIPBOARD_ENTRY_FILE) {
            /* We already have a file. Reuse it? 
               We should probably not pass the internal persistence file ownership.
               But for now, sticking to logic:
            */
             path = g_strdup(entry->persisted_file_path);
             create_temp = FALSE; 
        }
        
        if (create_temp) {
            /* Create new temp file ... existing logic ... */
             /* We can reuse persist logic if we implemented it generically.
                But let's keep the dedicated export logic or 
                just call persist_to_file if not already persisted?
             */
             if (entry->type == VITE_CLIPBOARD_ENTRY_REFERENCE) {
                 vite_clipboard_persist_to_file(clip);
                 /* Now we are FILE backed */
                 path = g_strdup(entry->persisted_file_path);
             }
        }
        
        if (path) {
            char *uri = g_strdup_printf("vite-temp://%s", path);
            if (clip->system_clipboard) {
                gdk_clipboard_set_text(clip->system_clipboard, uri);
                g_debug("vite_clipboard: Set huge content pointer: %s", uri);
            }
            g_free(uri);
            g_free(path);
        }
        
        clip->system_sync_pending = FALSE;
        return;
    }
    
    /* Small content - materialize */
    if (!entry->cached_text) {
        if (entry->type == VITE_CLIPBOARD_ENTRY_FILE) {
             int fd = open(entry->persisted_file_path, O_RDONLY);
             if (fd >= 0) {
                 entry->cached_text = resource_safe_malloc(len + 1);
                 if (entry->cached_text) {
                     ssize_t r = read(fd, entry->cached_text, len);
                     if (r < 0 || (size_t)r != len) {
                         g_free(entry->cached_text);
                         entry->cached_text = NULL;
                         entry->cached_len = 0;
                     } else {
                         entry->cached_text[len] = '\0';
                         entry->cached_len = len;
                     }
                 }
                 close(fd);
             }
        } else if (entry->type == VITE_CLIPBOARD_ENTRY_REFERENCE && entry->source_doc) {
             entry->cached_text = document_get_text_range(entry->source_doc,
                                                          entry->start_offset, len);
             entry->cached_len = len;
        }
    }
    
    if (clip->system_clipboard && entry->cached_text) {
        gdk_clipboard_set_text(clip->system_clipboard, entry->cached_text);
        g_debug("vite_clipboard: Synced %zu bytes to system clipboard", len);
    }
    
    clip->system_sync_pending = FALSE;
}

void
vite_clipboard_invalidate_for_document(ViteClipboard *clip, Document *doc)
{
    if (!clip || !clip->current || !doc) return;
    
    /* Only invalidate REFERENCE types. FILE types are strictly independent. */
    if (clip->current->type == VITE_CLIPBOARD_ENTRY_REFERENCE && 
        clip->current->source_doc == doc) {
        clip->current->is_valid = FALSE;
        g_debug("vite_clipboard: Invalidated reference for document");
    }
}

void
vite_clipboard_cancel_streaming(ViteClipboard *clip)
{
    if (!clip || !clip->active_paste) return;
    
    clip->active_paste->cancelled = TRUE;
    /* The idle handler will clean up */
    clip->active_paste = NULL;
}
