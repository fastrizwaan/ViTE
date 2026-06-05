#include "editor-internal.h"
#include <string.h>
#include <adwaita.h>
#include "resource-check.h"
#include "vite-clipboard.h"
#include "editor-widget.h" /* For compare_cursors definitions */

/* Forward declare compare */
static int compare_cursors(gconstpointer a, gconstpointer b) {
    const EditorCursor *ca = a;
    const EditorCursor *cb = b;
    if (ca->cursor_offset < cb->cursor_offset) return -1;
    if (ca->cursor_offset > cb->cursor_offset) return 1;
    return 0;
}

/* For Zero-RAM system clipboard paste */
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

/* Threshold for Zero-RAM system clipboard paste (1MB) 
 * Below this, RAM-based paste is fine for responsiveness */
#define SYSTEM_PASTE_ZERO_RAM_THRESHOLD (1024 * 1024)

static void
large_copy_response_cb(AdwAlertDialog *dialog, gchar *response, EditorWidget *self);

/**
 * Write content to a temp file for Zero-RAM paste.
 * Returns fd on success (caller must close), or -1 on failure.
 */
static int
write_to_temp_file(const char *data, size_t len)
{
    int fd = -1;
    char *path = NULL;
    
#ifdef O_TMPFILE
    fd = open(resource_get_vite_cache_dir(), O_TMPFILE | O_RDWR | O_EXCL, 0600);
    if (fd != -1) {
        /* O_TMPFILE succeeded - write directly */
    } else
#endif
    {
        /* Fallback to mkstemp */
        path = g_build_filename(resource_get_vite_cache_dir(), "vite_syspaste_XXXXXX", NULL);
        fd = mkstemp(path);
        if (fd == -1) {
            g_warning("write_to_temp_file: mkstemp failed: %s", strerror(errno));
            g_free(path);
            return -1;
        }
        /* Unlink immediately so file is deleted when fd closes */
        unlink(path);
        g_free(path);
    }
    
    /* Check disk space */
    if (!resource_can_write_disk(resource_get_vite_cache_dir(), len)) {
        g_warning("write_to_temp_file: Insufficient disk space for %zu bytes", len);
        close(fd);
        /* If named file, unlink it */
        if (path) {
            unlink(path);
            g_free(path);
        }
        return -1;
    }
    
    /* Write content to temp file */
    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, data + written, len - written);
        if (w <= 0) {
            g_warning("write_to_temp_file: write failed: %s", strerror(errno));
            close(fd);
            return -1;
        }
        written += w;
    }
    
    /* Seek back to start for reading */
    lseek(fd, 0, SEEK_SET);
    
    return fd;
}

static void
show_allocation_error_dialog(EditorWidget *self)
{
    GtkWindow *root = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
    
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        "Unable to Copy to System",
        "The selected text is too large to copy to the system clipboard.\n\nYou can use 'Internal Copy' to move text between ViTE tabs without using memory."
    ));
    
    adw_alert_dialog_add_response(dialog, "close", "Close");
    adw_alert_dialog_add_response(dialog, "internal", "Internal Copy (Zero RAM)");
    
    adw_alert_dialog_set_response_appearance(dialog, "internal", ADW_RESPONSE_SUGGESTED);
    
    adw_alert_dialog_set_default_response(dialog, "internal");
    adw_alert_dialog_set_close_response(dialog, "close");
    
    g_signal_connect_object(dialog, "response", G_CALLBACK(large_copy_response_cb), self, 0);
    
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(root));
}

static gboolean
filtered_lines_active(EditorWidget *self)
{
    return self->filtered_lines && self->filtered_lines->data && compact_matches_count(self->filtered_lines) > 0;
}

static gboolean
get_filtered_bounds(EditorWidget *self, size_t start_line, size_t end_line, size_t *out_start_idx, size_t *out_end_idx)
{
    size_t count = compact_matches_count(self->filtered_lines);
    size_t *data = self->filtered_lines->data;

    size_t low = 0, high = count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (data[mid] < start_line) low = mid + 1;
        else high = mid;
    }
    size_t lb = low;

    low = 0; high = count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (data[mid] <= end_line) low = mid + 1;
        else high = mid;
    }
    if (low == 0) return FALSE;
    size_t ub = low - 1;

    if (lb > ub) return FALSE;
    if (out_start_idx) *out_start_idx = lb;
    if (out_end_idx) *out_end_idx = ub;
    return TRUE;
}

static void
get_line_bounds(Document *doc, size_t line_idx, size_t *line_start, size_t *line_end)
{
    *line_start = document_get_offset_of_line(doc, line_idx);
    size_t len = 0;
    char *text = document_get_line(doc, line_idx, &len);
    if (text && len > 0 && text[len - 1] == '\n') {
        len--;
    }
    g_free(text);
    *line_end = *line_start + len;
}

static size_t
filtered_selection_size(EditorWidget *self, size_t start, size_t end)
{
    if (!filtered_lines_active(self) || start >= end) return 0;

    size_t start_line = document_get_line_of_offset(self->doc, start);
    size_t end_line = document_get_line_of_offset(self->doc, end);

    size_t start_idx = 0, end_idx = 0;
    if (!get_filtered_bounds(self, start_line, end_line, &start_idx, &end_idx)) return 0;

    size_t total = 0;
    size_t lines = 0;
    size_t *data = self->filtered_lines->data;
    for (size_t i = start_idx; i <= end_idx; i++) {
        size_t phys_line = data[i];
        size_t line_start = 0, line_end = 0;
        get_line_bounds(self->doc, phys_line, &line_start, &line_end);

        size_t seg_start = line_start;
        size_t seg_end = line_end;
        if (line_end > line_start) {
            seg_start = MAX(line_start, start);
            seg_end = MIN(line_end, end);
        } else {
            if (!(start <= line_start && line_start < end)) continue;
        }

        if (seg_end <= seg_start && line_end > line_start) continue;

        total += (seg_end - seg_start);
        lines++;
    }

    if (lines > 1) total += (lines - 1);
    return total;
}

static gboolean
append_filtered_selection(EditorWidget *self, size_t start, size_t end, GString *out)
{
    if (!filtered_lines_active(self) || start >= end) return FALSE;

    size_t start_line = document_get_line_of_offset(self->doc, start);
    size_t end_line = document_get_line_of_offset(self->doc, end);

    size_t start_idx = 0, end_idx = 0;
    if (!get_filtered_bounds(self, start_line, end_line, &start_idx, &end_idx)) return FALSE;

    gboolean first_line = TRUE;
    gboolean appended = FALSE;
    size_t *data = self->filtered_lines->data;
    for (size_t i = start_idx; i <= end_idx; i++) {
        size_t phys_line = data[i];
        size_t line_start = 0, line_end = 0;
        get_line_bounds(self->doc, phys_line, &line_start, &line_end);

        size_t seg_start = line_start;
        size_t seg_end = line_end;
        if (line_end > line_start) {
            seg_start = MAX(line_start, start);
            seg_end = MIN(line_end, end);
        } else {
            if (!(start <= line_start && line_start < end)) continue;
        }

        if (seg_end <= seg_start && line_end > line_start) continue;

        if (!first_line) g_string_append_c(out, '\n');
        first_line = FALSE;
        appended = TRUE;

        if (seg_end > seg_start) {
            char *text = document_get_text_range(self->doc, seg_start, seg_end - seg_start);
            if (text) {
                g_string_append(out, text);
                g_free(text);
            }
        }
    }
    return appended;
}

size_t
editor_widget_delete_selection(EditorWidget *self)
{
    /* This function handles bulk deletion of all selections */
    if (!self->cursors || self->cursors->len == 0) return 0;
    
    /* Optimization: If single cursor selects entire document, use snapshot-based delete */
    if (self->cursors->len == 1) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, 0);
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        size_t total = document_get_length(self->doc);
        
        if (start == 0 && end == total && total > 0) {
            document_delete_entire(self->doc);
            cur->cursor_offset = 0;
            cur->selection_anchor = 0;
            return total;
        }
    }
    
    g_array_sort(self->cursors, compare_cursors_desc);
    
    document_begin_undo_group(self->doc);
    
    size_t total_deleted = 0;
    for (guint c = 0; c < self->cursors->len; c++) {
        EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
        size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
        size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
        
        if (start != end) {
            size_t len = end - start;
            document_delete(self->doc, start, len);
            cur->cursor_offset = start;
            cur->selection_anchor = start;
            total_deleted += len;
        }
    }
    
    document_end_undo_group(self->doc);
    return total_deleted;
}

static void
perform_copy_internal(EditorWidget *self, size_t total_size)
{
    /* Use mmap-backed file instead of RAM for massive copies */
    char *template_path = g_build_filename(resource_get_vite_cache_dir(), "vite-copy-XXXXXX", NULL);
    int fd = mkstemp(template_path);
    if (fd == -1) {
        g_free(template_path);
        show_allocation_error_dialog(self);
        return;
    }
    
    /* Use posix_fallocate to reserve physical disk blocks immediately instead of creating a sparse file.
     * This prevents a SIGBUS when faulting in mmap pages if the disk/quota gets exhausted. */
    if (posix_fallocate(fd, 0, total_size + 1) != 0) {
        close(fd);
        unlink(template_path);
        g_free(template_path);
        show_allocation_error_dialog(self);
        return;
    }
    
    char *clip_text = mmap(NULL, total_size + 1, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (clip_text == MAP_FAILED) {
        close(fd);
        unlink(template_path);
        g_free(template_path);
        show_allocation_error_dialog(self);
        return;
    }
    
    /* Create a temp array to sort ascending */
    GArray *sorted = g_array_sized_new(FALSE, FALSE, sizeof(EditorCursor), self->cursors->len);
    g_array_append_vals(sorted, self->cursors->data, self->cursors->len);
    g_array_sort(sorted, compare_cursors); /* Ascending sort for clipboard order */
    
    size_t current_offset = 0;
    
    for (guint c = 0; c < self->cursors->len; c++) {
         EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
         size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
         size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
         if (start == end) continue;

         if (filtered_lines_active(self)) {
             GString *tmp = g_string_new("");
             gboolean appended = append_filtered_selection(self, start, end, tmp);
             if (appended && tmp->len > 0) {
                 if (current_offset > 0 && current_offset < total_size) {
                     clip_text[current_offset++] = '\n';
                 }
                 if (current_offset + tmp->len <= total_size) {
                     memcpy(clip_text + current_offset, tmp->str, tmp->len);
                     current_offset += tmp->len;
                 }
             }
             g_string_free(tmp, TRUE);
         } else {
             if (current_offset > 0 && current_offset < total_size) {
                 clip_text[current_offset++] = '\n';
             }
             
             size_t len = end - start;
             size_t written = 0;
             while (written < len) {
                 size_t chunk = MIN(len - written, 1024 * 1024);
                 char *text = document_get_text_range(self->doc, start + written, chunk);
                 if (text) {
                     if (current_offset + chunk <= total_size) {
                         memcpy(clip_text + current_offset, text, chunk);
                         current_offset += chunk;
                     }
                     g_free(text);
                 } else {
                     /* Allocation error mid-copy */
                     g_warning("perform_copy_internal: Failed to read text chunk.");
                     break;
                 }
                 written += chunk;
             }
         }
    }
    
    if (current_offset <= total_size) {
        clip_text[current_offset] = '\0';
    } else {
        clip_text[total_size] = '\0';
    }
    
    msync(clip_text, current_offset + 1, MS_SYNC);
    
    if (current_offset > 0) {
        GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
        gdk_clipboard_set_text(clipboard, clip_text);
        vite_clipboard_clear(vite_clipboard_get_default());
    }
    
    g_array_free(sorted, TRUE);
    munmap(clip_text, total_size + 1);
    close(fd);
    unlink(template_path);
    g_free(template_path);
}



struct _CutState {
    EditorWidget *self;
    size_t start;
    size_t end;
};

static void
on_cut_deleted(double progress, gboolean finished, gpointer user_data)
{
    struct _CutState *state = user_data;

    g_signal_emit_by_name(state->self, "undo-redo-progress", progress, !finished);

    if (finished) {
        document_end_undo_group(state->self->doc);
        g_free(state);
    }
}

static void
on_internal_copy_done(size_t done, size_t total, gpointer user_data)
{
    struct _CutState *state = user_data;
    if (!state) return;

    if (total == 0) {
        g_free(state);
        return;
    }

    double progress = total > 0 ? (double)done / total : 1.0;
    gboolean finished = (done >= total);

    g_signal_emit_by_name(state->self, "undo-redo-progress", progress, !finished);

    if (finished) {
         document_begin_undo_group(state->self->doc);
         if (state->start != state->end) {
             document_set_undo_group_selection(state->self->doc, state->start, state->end);
         }

         size_t doc_len = document_get_length(state->self->doc);
         size_t delete_start = MIN(state->start, doc_len);
         size_t delete_end = MIN(state->end, doc_len);

         if (state->self->cursors->len == 1 && state->start < state->end) {
             EditorCursor *cur = &g_array_index(state->self->cursors, EditorCursor, 0);

             if (delete_start == 0 && delete_end == doc_len && doc_len > 0) {
                 document_delete_entire_async(state->self->doc, on_cut_deleted, state);
                 cur->cursor_offset = 0;
                 cur->selection_anchor = 0;
                 return;
             }
         }

         if (delete_start < delete_end) {
             document_delete(state->self->doc, delete_start, delete_end - delete_start);
             if (state->self->cursors && state->self->cursors->len > 0) {
                 EditorCursor *primary = &g_array_index(state->self->cursors, EditorCursor, 0);
                 primary->cursor_offset = delete_start;
                 primary->selection_anchor = delete_start;
                 state->self->cursor_offset = delete_start;
                 state->self->selection_anchor = delete_start;
             }
         }
         document_end_undo_group(state->self->doc);
         g_free(state);
    }
}

static void
on_internal_copy_progress_only(size_t done, size_t total, gpointer user_data)
{
    struct _CutState *state = user_data;
    if (!state || !state->self) return;

    if (total == 0) {
        g_free(state);
        return;
    }

    double progress = total > 0 ? (double)done / total : 1.0;
    gboolean finished = (done >= total);

    g_signal_emit_by_name(state->self, "undo-redo-progress", progress, !finished);

    if (finished) {
        g_free(state);
    }
}

static void
large_copy_response_cb(AdwAlertDialog *dialog, gchar *response, EditorWidget *self)
{
    gboolean is_cut = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "is_cut"));
    size_t total_size = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(dialog), "total_size"));

    if (g_strcmp0(response, "copy") == 0) {
        perform_copy_internal(self, total_size);
        if (is_cut) {
            document_begin_undo_group(self->doc);
            if (self->cursors && self->cursors->len > 0) {
                 EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);
                 if (primary->cursor_offset != primary->selection_anchor) {
                     document_set_undo_group_selection(self->doc, primary->selection_anchor, primary->cursor_offset);
                 }
            }
            editor_widget_delete_selection(self);
            document_end_undo_group(self->doc);
        }
    } else if (g_strcmp0(response, "internal") == 0) {
        ViteClipboard *clip = vite_clipboard_get_default();
        if (self->cursors && self->cursors->len > 0) {
            EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, 0);
            size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
            size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
            
            if (is_cut) {
                struct _CutState *state = g_new0(struct _CutState, 1);
                state->self = self;
                state->start = start;
                state->end = end;
                vite_clipboard_copy_async(clip, self->doc, start, end, TRUE, on_internal_copy_done, state);
            } else {
                struct _CutState *state = g_new0(struct _CutState, 1);
                state->self = self;
                state->start = start;
                state->end = end;
                vite_clipboard_copy_async(clip, self->doc, start, end, FALSE, on_internal_copy_progress_only, state);
            }
        }
    }
}

static gboolean
editor_widget_copy_full(EditorWidget *self, gboolean is_cut)
{
    if (!self->cursors || self->cursors->len == 0) return FALSE;

    /* Pre-calculate total size */
    size_t total_size = 0;
    for (guint c = 0; c < self->cursors->len; c++) {
         EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
         size_t start = MIN(cur->cursor_offset, cur->selection_anchor);
         size_t end = MAX(cur->cursor_offset, cur->selection_anchor);
         if (filtered_lines_active(self)) {
             total_size += filtered_selection_size(self, start, end);
         } else {
             total_size += (end - start);
         }
    }
    if (self->cursors->len > 1) total_size += (self->cursors->len - 1);
    
    /* Resource Check */
    if (!resource_can_allocate(total_size)) {
         show_allocation_error_dialog(self);
         return FALSE;
    }
    
    /* Warning Threshold: 50MB */
    size_t huge_threshold = 50ULL * 1024 * 1024;
    if (total_size > huge_threshold) {
        size_t free_ram = resource_get_available_ram();
        double size_mb = (double)total_size / (1024.0 * 1024.0);
        double free_mb = (double)free_ram / (1024.0 * 1024.0);
        
        char *msg = g_strdup_printf("Copying %.2f MB. Available RAM is %.2f MB.\n\nThis operation may freeze the application or cause memory crashes.", size_mb, free_mb);
        
        GtkWindow *root = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
        AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
            "Large Copy Operation",
            msg
        ));
        g_free(msg);
        
        /* Pass is_cut state to dialog */
        g_object_set_data(G_OBJECT(dialog), "is_cut", GINT_TO_POINTER(is_cut));
        g_object_set_data(G_OBJECT(dialog), "total_size", GSIZE_TO_POINTER(total_size));
        
        adw_alert_dialog_add_response(dialog, "cancel", "Cancel");
        adw_alert_dialog_add_response(dialog, "internal", "Internal Copy (Zero RAM)");
        adw_alert_dialog_add_response(dialog, "copy", "System Copy");
        
        adw_alert_dialog_set_response_appearance(dialog, "copy", ADW_RESPONSE_DESTRUCTIVE); /* Warn it's heavy */
        adw_alert_dialog_set_response_appearance(dialog, "internal", ADW_RESPONSE_SUGGESTED);
        
        adw_alert_dialog_set_default_response(dialog, "internal");
        adw_alert_dialog_set_close_response(dialog, "cancel");
        
        g_signal_connect_object(dialog, "response", G_CALLBACK(large_copy_response_cb), self, 0);
        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(root));
        
        return FALSE; /* Abort synchronous copy/cut */
    }
    
    perform_copy_internal(self, total_size);
    return TRUE;
}

gboolean
editor_widget_copy(EditorWidget *self)
{
    return editor_widget_copy_full(self, FALSE);
}

void
editor_widget_cut(EditorWidget *self)
{
    if (!self->doc) return;
    if (editor_widget_copy_full(self, TRUE)) {
        /* If we used Internal Reference Copy, we MUST persist to file before deleting! */
        ViteClipboard *clip = vite_clipboard_get_default();
        if (vite_clipboard_has_internal_content(clip) && vite_clipboard_is_reference_valid(clip)) {
             vite_clipboard_persist_to_file(clip);
        }
        
        document_begin_undo_group(self->doc);
        
        /* Save Selection State */
        if (self->cursors && self->cursors->len > 0) {
             EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);
             if (primary->cursor_offset != primary->selection_anchor) {
                 document_set_undo_group_selection(self->doc, primary->selection_anchor, primary->cursor_offset);
             }
        }
        
        editor_widget_delete_selection(self);
        
        document_end_undo_group(self->doc);
    }
}

static void
on_paste_text_received(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    EditorWidget **self_ptr = (EditorWidget **)user_data;
    EditorWidget *self = *self_ptr;
    if (self) {
        g_object_remove_weak_pointer(G_OBJECT(self), (gpointer *)self_ptr);
    }
    g_free(self_ptr);
    if (!self) return;

    GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
    char *text = gdk_clipboard_read_text_finish(clipboard, res, NULL);
    if (!text) return;
    
    /* Translate ↵ back to \n */
    GString *norm_text = g_string_new("");
    char *p = text;
    while (*p) {
        if (strncmp(p, "\xE2\x86\xB5", 3) == 0) {
            g_string_append_c(norm_text, '\n');
            p += 3;
        } else {
            g_string_append_c(norm_text, *p);
            p++;
        }
    }
    g_free(text);
    text = g_string_free(norm_text, FALSE);
    
    size_t len = strlen(text);
    if (len > 0) {
        document_begin_undo_group(self->doc);
        
        /* Save Selection State */
        if (self->cursors && self->cursors->len > 0) {
             EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);
             if (primary->cursor_offset != primary->selection_anchor) {
                 document_set_undo_group_selection(self->doc, primary->selection_anchor, primary->cursor_offset);
             }
        }
        
        /* If we have a selection, delete it first */
        editor_widget_delete_selection(self);
        
        /* Zero-RAM strategy for large system clipboard content */
        if (len >= SYSTEM_PASTE_ZERO_RAM_THRESHOLD) {
            /* Write to temp file and use document_insert_from_fd */
            int fd = write_to_temp_file(text, len);
            g_free(text);
            text = NULL; /* Already freed */
            
            if (fd >= 0) {
                for (guint c = 0; c < self->cursors->len; c++) {
                    EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                    document_insert_from_fd(self->doc, cur->cursor_offset, fd, len);
                    cur->cursor_offset += len;
                    cur->selection_anchor = cur->cursor_offset;
                    /* Reset fd position for next cursor */
                    lseek(fd, 0, SEEK_SET);
                }
                close(fd);
            }
        } else {
            /* Small content - use regular RAM-based insert for speed */
            for (guint c = 0; c < self->cursors->len; c++) {
                EditorCursor *cur = &g_array_index(self->cursors, EditorCursor, c);
                document_insert(self->doc, cur->cursor_offset, text, len);
                cur->cursor_offset += len;
                cur->selection_anchor = cur->cursor_offset;
            }
            g_free(text);
        }
        
        document_end_undo_group(self->doc);
        
        editor_widget_reset_cursor_blink(self);
        editor_widget_update_adjustments(self, -1, -1);
        scroll_to_cursor(self);
        gtk_widget_queue_draw(GTK_WIDGET(self));

        size_t line, col;
        editor_widget_get_cursor_position(self, &line, &col);
        g_signal_emit(self, editor_signals[CURSOR_MOVED], 0, (guint)line, (guint)col);
    } else {
        g_free(text);
    }
}

static void
on_internal_paste_progress(size_t done, size_t total, gpointer user_data)
{
    EditorWidget *self = user_data;
    if (!self) return;

    double progress = total > 0 ? (double)done / total : 1.0;
    gboolean finished = (done >= total);

    g_signal_emit_by_name(self, "undo-redo-progress", progress, !finished);
}

void
editor_widget_paste(EditorWidget *self)
{
    /* Check for Internal Zero-RAM content */
    ViteClipboard *vclip = vite_clipboard_get_default();
    if (vite_clipboard_has_internal_content(vclip)) {
         /* Streaming paste for primary cursor */
         /* We use primary cursor offset */
         EditorCursor *primary = &g_array_index(self->cursors, EditorCursor, 0);
         vite_clipboard_paste_streaming(vclip, self->doc, primary->cursor_offset, on_internal_paste_progress, self);
         return;
    }

    EditorWidget **self_ptr = g_new(EditorWidget *, 1);
    *self_ptr = self;
    g_object_add_weak_pointer(G_OBJECT(self), (gpointer *)self_ptr);

    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
    gdk_clipboard_read_text_async(clipboard, NULL, on_paste_text_received, self_ptr);
}

static void
on_primary_paste_received(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
    EditorWidget **self_ptr = (EditorWidget **)user_data;
    EditorWidget *self = *self_ptr;
    if (self) {
        g_object_remove_weak_pointer(G_OBJECT(self), (gpointer *)self_ptr);
    }
    g_free(self_ptr);
    if (!self) return;

    GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
    char *text = gdk_clipboard_read_text_finish(clipboard, res, NULL);
    
    if (text) {
        /* Translate ↵ back to \n */
        GString *norm_text = g_string_new("");
        char *p = text;
        while (*p) {
            if (strncmp(p, "\xE2\x86\xB5", 3) == 0) {
                g_string_append_c(norm_text, '\n');
                p += 3;
            } else {
                g_string_append_c(norm_text, *p);
                p++;
            }
        }
        g_free(text);
        text = g_string_free(norm_text, FALSE);
        
        size_t len = strlen(text);
        if (len > 0) {
            EditorCursor *primary = editor_widget_get_primary_cursor(self);
            if (primary) {
                document_begin_undo_group(self->doc);
                
                if (primary->cursor_offset != primary->selection_anchor) {
                     document_set_undo_group_selection(self->doc, primary->selection_anchor, primary->cursor_offset);
                }
                
                /* Zero-RAM strategy for large system clipboard content */
                if (len >= SYSTEM_PASTE_ZERO_RAM_THRESHOLD) {
                    /* Write to temp file and use document_insert_from_fd */
                    int fd = write_to_temp_file(text, len);
                    g_free(text);
                    text = NULL; /* Already freed */
                    
                    if (fd >= 0) {
                         document_insert_from_fd(self->doc, primary->cursor_offset, fd, len);
                         primary->cursor_offset += len;
                         primary->selection_anchor = primary->cursor_offset;
                         close(fd);
                    }
                } else {
                     /* Small content - use regular RAM-based insert */
                     document_insert(self->doc, primary->cursor_offset, text, len);
                     primary->cursor_offset += len;
                     primary->selection_anchor = primary->cursor_offset;
                     
                     g_free(text);
                }
                
                document_end_undo_group(self->doc);
                
                editor_widget_reset_cursor_blink(self);
                editor_widget_update_adjustments(self, -1, -1);
                scroll_to_cursor(self);
                gtk_widget_queue_draw(GTK_WIDGET(self));

                size_t line, col;
                editor_widget_get_cursor_position(self, &line, &col);
                g_signal_emit(self, editor_signals[CURSOR_MOVED], 0, (guint)line, (guint)col);
            } else {
                g_free(text);
            }
        } else {
            g_free(text);
        }
    }
}

void
editor_widget_paste_primary(EditorWidget *self)
{
    EditorWidget **self_ptr = g_new(EditorWidget *, 1);
    *self_ptr = self;
    g_object_add_weak_pointer(G_OBJECT(self), (gpointer *)self_ptr);

    GdkClipboard *clipboard = gdk_display_get_primary_clipboard(gtk_widget_get_display(GTK_WIDGET(self)));
    gdk_clipboard_read_text_async(clipboard, NULL, on_primary_paste_received, self_ptr);
}
