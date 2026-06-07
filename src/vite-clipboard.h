/*
 * vite-clipboard.h - Reference-based clipboard for huge file operations
 * 
 * Instead of copying gigabytes of text, we store references to document
 * regions. This makes Copy O(1) and Paste streams data on-demand.
 */

#ifndef VITE_CLIPBOARD_H
#define VITE_CLIPBOARD_H

#include <gtk/gtk.h>
#include "document.h"

/* Virtual Selection - O(1) metadata instead of O(n) copy */
typedef struct {
    size_t start_offset;
    size_t end_offset;
    gboolean is_whole_document;  /* Optimization flag for undo */
    uint64_t document_version;    /* To detect stale selections */
} VirtualSelection;

typedef enum {
    VITE_CLIPBOARD_ENTRY_TEXT,
    VITE_CLIPBOARD_ENTRY_REFERENCE,
    VITE_CLIPBOARD_ENTRY_FILE
} ViteClipboardEntryType;

/* Reference-based Clipboard Entry */
typedef struct {
    ViteClipboardEntryType type;

    /* Source reference (for internal paste - zero-copy) */
    Document *source_doc;         /* Weak reference to source document */
    char *source_file_path;       /* Path to source file (for cross-doc) */
    size_t start_offset;
    size_t end_offset;
    uint64_t source_version;      /* Document version when copied */
    
    /* Cached text (for external paste or if source changed) */
    char *cached_text;            /* NULL until materialized */
    size_t cached_len;

    /* For FILE type (persisted from reference) */
    int persisted_fd;             /* Temp file descriptor containing the content */
    
    /* Flags */
    gboolean is_valid;            /* FALSE if source was modified/closed */
    gboolean is_cut;              /* TRUE if this was a cut operation */
} ViteClipboardEntry;

/* Progress callback for streaming operations */
typedef void (*ViteClipboardProgressCallback)(size_t bytes_done, size_t bytes_total, gpointer user_data);

/* Streaming paste context */
typedef struct _ViteStreamingPaste ViteStreamingPaste;

/* Async copy context */
typedef struct _ViteClipboardCopyTask ViteClipboardCopyTask;

/* Global Clipboard Manager */
typedef struct {
    ViteClipboardEntry *current;
    GdkClipboard *system_clipboard;
    gboolean system_sync_pending;  /* Deferred system clipboard sync */
    uint64_t generation;           /* Bumps whenever internal content is replaced/cleared */
    
    /* Active async copy (if any) */
    ViteClipboardCopyTask *active_copy;

    /* Active streaming paste (if any) */
    ViteStreamingPaste *active_paste;
} ViteClipboard;

/* API */
ViteClipboard *vite_clipboard_get_default(void);
void vite_clipboard_free(ViteClipboard *clip);
void vite_clipboard_clear(ViteClipboard *clip);

/* Set clipboard to reference a document region (O(1) copy) */
void vite_clipboard_set_reference(ViteClipboard *clip, Document *doc, 
                                   size_t start, size_t end, gboolean is_cut);

/* Check if we have internal (reference-based) content */
gboolean vite_clipboard_has_internal_content(ViteClipboard *clip);

/* Async Copy */
ViteClipboardCopyTask *vite_clipboard_copy_async(ViteClipboard *clip, Document *doc, size_t start, size_t end, gboolean is_cut, ViteClipboardProgressCallback cb, gpointer user_data);
void vite_clipboard_copy_async_cancel(ViteClipboardCopyTask *task);

/* Get the size of clipboard content without materializing */
size_t vite_clipboard_get_content_size(ViteClipboard *clip);

/* Streaming paste for large content - returns immediately, pastes in background */
void vite_clipboard_paste_streaming(ViteClipboard *clip, Document *target, 
                                     size_t offset,
                                     ViteClipboardProgressCallback progress_cb,
                                     gpointer user_data);

/* Synchronous paste for small content */
gboolean vite_clipboard_paste_sync(ViteClipboard *clip, Document *target, 
                                    size_t offset, size_t *out_pasted_len);

/* Force sync to system clipboard (for external paste) */
void vite_clipboard_sync_to_system(ViteClipboard *clip);

/* Invalidate entries referencing a document (call when doc is modified/closed) */
void vite_clipboard_invalidate_for_document(ViteClipboard *clip, Document *doc);

/* Cancel any active streaming paste */
void vite_clipboard_cancel_streaming(ViteClipboard *clip);

/* Check if reference is still valid (source not modified) */
gboolean vite_clipboard_is_reference_valid(ViteClipboard *clip);

/* Persist current reference clipboard entry to a file descriptor (used for Cut) */
void vite_clipboard_persist_to_fd(ViteClipboard *clip);

#endif /* VITE_CLIPBOARD_H */
