#include "find-replace-bar.h"
#include "document.h"

struct _ViteFindReplaceBar {
    GtkBox parent_instance;

    EditorWidget *editor;
    
    /* UI Elements */
    GtkWidget *find_entry;
    GtkWidget *replace_entry;
    GtkWidget *matches_label;
    GtkWidget *replace_box;
    GtkWidget *replace_status_label;
    GtkWidget *replace_all_btn;
    GtkWidget *toggle_repl_btn;
    
    GtkWidget *regex_check;
    GtkWidget *case_check;
    GtkWidget *word_check; 
    
    guint search_timeout_id;
    SearchTask *current_search;
    
    /* Viewport Search */
    gboolean viewport_mode;
    guint viewport_scroll_handler_id;
    guint viewport_update_timeout_id;
    
    ReplaceTask *current_replace_task;

    StreamingReplaceTask *current_streaming_replace;
    
    /* Filter Mode State */
    gboolean filter_mode;
    DocumentFilterTask *current_filter_task;
    FilterResult *current_filter_result;
    guint filter_tick_id;

};

enum {
    SIGNAL_PROGRESS_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS] = {0};

G_DEFINE_TYPE(ViteFindReplaceBar, vite_find_replace_bar, GTK_TYPE_BOX)

static void vite_find_replace_bar_dispose(GObject *object) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(object);
    if (self->search_timeout_id) {
        g_source_remove(self->search_timeout_id);
        self->search_timeout_id = 0;
    }
    
    if (self->viewport_update_timeout_id) {
        g_source_remove(self->viewport_update_timeout_id);
        self->viewport_update_timeout_id = 0;
    }
    
    if (self->viewport_scroll_handler_id && self->editor) {
        GtkAdjustment *vadj = editor_widget_get_vadjustment(self->editor);
        if (vadj) {
            g_signal_handler_disconnect(vadj, self->viewport_scroll_handler_id);
        }
        self->viewport_scroll_handler_id = 0;
    }
    
    if (self->current_search) {
        document_search_async_cancel(self->current_search);
        self->current_search = NULL;
    }
    if (self->editor) {
        editor_widget_set_active_search(self->editor, NULL);
        editor_widget_set_search_results(self->editor, NULL);
    }
    
    if (self->current_replace_task) {
        document_replace_async_cancel(self->current_replace_task);
        self->current_replace_task = NULL;
    }
    
    if (self->current_streaming_replace) {
        document_replace_streaming_cancel(self->current_streaming_replace);
        document_replace_streaming_cancel(self->current_streaming_replace);
        self->current_streaming_replace = NULL;
    }
    
    if (self->current_filter_task) {
        document_filter_async_cancel(self->current_filter_task);
        self->current_filter_task = NULL;
    }
    
    if (self->current_filter_result) {
        filter_result_free(self->current_filter_result);
        self->current_filter_result = NULL;
    }
    
    if (self->filter_tick_id) {
        g_source_remove(self->filter_tick_id);
        self->filter_tick_id = 0;
    }

    if (self->editor) {
        g_signal_handlers_disconnect_by_data(self->editor, self);
    }
    
    G_OBJECT_CLASS(vite_find_replace_bar_parent_class)->dispose(object);
}

static void
update_matches_label(ViteFindReplaceBar *self) {
    if (!self) return;
    
    if (self->filter_mode) {
        /* Filter Mode Label Update */
        if (!self->current_filter_result) {
             /* If task running? */
             if (self->current_filter_task) {
                 gtk_label_set_text(GTK_LABEL(self->matches_label), "Filtering...");
                 gtk_widget_set_visible(self->matches_label, TRUE);
             } else {
                 gtk_widget_set_visible(self->matches_label, FALSE);
             }
             return;
        }
        size_t count = self->current_filter_result->count;
        if (count == 0) {
            gtk_label_set_text(GTK_LABEL(self->matches_label), "No matches");
        } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "%zu matches", count);
            gtk_label_set_text(GTK_LABEL(self->matches_label), buf);
        }
        gtk_widget_set_visible(self->matches_label, TRUE);
        return;
    }
    
    /* Find Mode Label Update */
    if (!self->current_search) {
         gtk_widget_set_visible(self->matches_label, FALSE);
         return;
    }
    
    /* Use match count API - doesn't trigger expensive conversion */
    size_t total = document_search_task_get_match_count(self->current_search);
    
    if (total == 0) {
        gtk_label_set_text(GTK_LABEL(self->matches_label), "No matches");
    } else {
         /* For now, just show total count - getting current match index 
            would require the full GArray which is expensive */
         char buf[64];
         snprintf(buf, sizeof(buf), "%zu matches", total);
         gtk_label_set_text(GTK_LABEL(self->matches_label), buf);
    }
    gtk_widget_set_visible(self->matches_label, TRUE);
}

static void 
on_caret_moved(EditorWidget *editor, ViteFindReplaceBar *self) {
    (void)editor;
    if (!self) return;
    /* Only update if we have an active search */
    if (self->current_search) {
        update_matches_label(self);
    }
}


static void on_search_update(GArray *matches, gboolean finished, void *user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    (void)matches; /* Ignored - we use viewport extraction instead */
    
    if (!self) return;
    if (!self->current_search) return;
    if (!self->editor) return;
    
    /* Verify document is still valid */
    Document *doc = editor_widget_get_document(self->editor);
    if (!doc) {
        /* Document changed - abort this search callback */
        return;
    }
    
    /* Get total match count (works on compact storage without conversion) */
    size_t match_count = document_search_task_get_match_count(self->current_search);
    
    /* Update Label */
    if (!finished) {
        size_t total = document_search_task_get_total_lines(self->current_search);
        size_t searched = document_search_task_get_lines_searched(self->current_search);
        int percent = total > 0 ? (int)((searched * 100) / total) : 0;
        
        char buf[64];
        snprintf(buf, sizeof(buf), "Finding... %d%% (%zu)", percent, match_count);
        gtk_label_set_text(GTK_LABEL(self->matches_label), buf);
        gtk_widget_set_visible(self->matches_label, TRUE);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "%zu matches", match_count);
        gtk_label_set_text(GTK_LABEL(self->matches_label), buf);
        gtk_widget_set_visible(self->matches_label, TRUE);
    }
    
    /* Get viewport range and extract only visible matches using binary search */
    size_t start_offset, end_offset;
    editor_widget_get_visible_offset_range(self->editor, &start_offset, &end_offset);
    
    /* Expand range for smooth scrolling */
    size_t padding = 10000; /* ~10KB buffer */
    start_offset = (start_offset > padding) ? start_offset - padding : 0;
    end_offset += padding;
    
    GArray *viewport_matches = document_search_task_get_viewport_matches(
        self->current_search, start_offset, end_offset);
    
    editor_widget_set_search_results(self->editor, viewport_matches);
    if (viewport_matches) g_array_unref(viewport_matches);
}


static void update_viewport_search(ViteFindReplaceBar *self);

static gboolean on_viewport_scroll_timeout(gpointer user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    self->viewport_update_timeout_id = 0;
    update_viewport_search(self);
    return G_SOURCE_REMOVE;
}

static void on_viewport_scroll(GtkAdjustment *adj, gpointer user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    (void)adj;
    
    if (self->viewport_update_timeout_id) {
        g_source_remove(self->viewport_update_timeout_id);
    }
    /* Debounce 100ms */
    self->viewport_update_timeout_id = g_timeout_add(100, on_viewport_scroll_timeout, self);
}

static void update_viewport_search(ViteFindReplaceBar *self) {
    /* Safety checks - must have valid search and editor */
    if (!self) return;
    if (!self->current_search) return;
    if (!self->editor) return;
    
    /* Check if editor still has a document */
    Document *doc = editor_widget_get_document(self->editor);
    if (!doc) {
        /* Document was closed/changed - cancel search */
        if (self->current_search) {
            document_search_async_cancel(self->current_search);
            self->current_search = NULL;
        }
        editor_widget_set_search_results(self->editor, NULL);
        return;
    }
    
    /* Get viewport range and extract only visible matches using binary search */
    size_t start_offset, end_offset;
    editor_widget_get_visible_offset_range(self->editor, &start_offset, &end_offset);
    
    /* Expand range for smooth scrolling */
    size_t padding = 50000; /* 50KB buffer for smoother scroll */
    start_offset = (start_offset > padding) ? start_offset - padding : 0;
    end_offset += padding;
    
    GArray *viewport_matches = document_search_task_get_viewport_matches(
        self->current_search, start_offset, end_offset);
    
    g_print("[DEBUG] update_viewport: offset %zu-%zu, got %u matches\n", 
            start_offset, end_offset, viewport_matches ? viewport_matches->len : 0);
    
    editor_widget_set_search_results(self->editor, viewport_matches);
    if (viewport_matches) g_array_unref(viewport_matches);
}

static gboolean filter_async_step(gpointer user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    
    if (!self->current_filter_task) {
        self->filter_tick_id = 0;
        return G_SOURCE_REMOVE;
    }
    
    /* Run for up to 2ms */
    gboolean finished = document_filter_async_step(self->current_filter_task, 2000);
    
    if (!finished) {
        /* Update Progress */
        size_t processed = document_filter_task_get_processed(self->current_filter_task);
        size_t total = document_filter_task_get_total(self->current_filter_task);
        size_t matches = document_filter_task_get_match_count(self->current_filter_task);
        
        int percent = (total > 0) ? (int)((processed * 100) / total) : 0;
        char buf[64];
        snprintf(buf, sizeof(buf), "Filtering... %d%% (%zu)", percent, matches);
        gtk_label_set_text(GTK_LABEL(self->matches_label), buf);
        gtk_widget_set_visible(self->matches_label, TRUE);
    } else {
        /* Get results */
        FilterResult *res = document_filter_async_finish(self->current_filter_task);
        self->current_filter_task = NULL;
        self->filter_tick_id = 0;
        
        self->current_filter_result = res;
        
        /* Update Editor */
        const char *pattern = gtk_editable_get_text(GTK_EDITABLE(self->find_entry));
        gboolean regex = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->regex_check));
        gboolean case_sensitive = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->case_check));
        
        if (res->count > 0) {
            /* Transfer ownership of matches to editor */
            editor_widget_set_filtered_lines(self->editor, 
                                           res->matches, 
                                           pattern, regex, case_sensitive);
            res->matches = NULL; 
        } else {
            editor_widget_set_filtered_lines(self->editor, NULL, NULL, FALSE, FALSE);
        }
        
        update_matches_label(self);
        return G_SOURCE_REMOVE;
    }
    
    return G_SOURCE_CONTINUE;
}

static gboolean perform_search(ViteFindReplaceBar *self) {
    self->search_timeout_id = 0;
    
    Document *doc = editor_widget_get_document(self->editor);
    if (!doc) return G_SOURCE_REMOVE;

    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->find_entry));
    
    /* --- FILTER MODE BRANCH --- */
    if (self->filter_mode) {
        /* Cancel previous filter */
        if (self->filter_tick_id) {
            g_source_remove(self->filter_tick_id);
            self->filter_tick_id = 0;
        }
        if (self->current_filter_task) {
            document_filter_async_cancel(self->current_filter_task);
            self->current_filter_task = NULL;
        }
        if (self->current_filter_result) {
            filter_result_free(self->current_filter_result);
            self->current_filter_result = NULL;
        }
        
        if (!text || !*text) {
             editor_widget_set_filtered_lines(self->editor, NULL, NULL, FALSE, FALSE);
             gtk_widget_set_visible(self->matches_label, FALSE);
             return G_SOURCE_REMOVE;
        }
        
        gboolean regex = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->regex_check));
        gboolean case_sensitive = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->case_check));
        
        self->current_filter_task = document_filter_async_start(doc, text, regex, case_sensitive);
        if (self->current_filter_task) {
            self->filter_tick_id = g_idle_add(filter_async_step, self);
            gtk_label_set_text(GTK_LABEL(self->matches_label), "Filtering...");
            gtk_widget_set_visible(self->matches_label, TRUE);
        }
        return G_SOURCE_REMOVE;
    }

    /* --- FIND MODE BRANCH --- */
    
    /* Cancel any previous async search */
    if (self->current_search) {
        document_search_async_cancel(self->current_search);
        self->current_search = NULL;
        editor_widget_set_active_search(self->editor, NULL);
    }
    
    if (!doc) return G_SOURCE_REMOVE;
    
    if (!text || !*text) {
        editor_widget_set_search_results(self->editor, NULL);
        gtk_widget_set_visible(self->matches_label, FALSE);
        
        /* Disconnect scroll handler when not searching */
        if (self->viewport_scroll_handler_id) {
            GtkAdjustment *vadj = editor_widget_get_vadjustment(self->editor);
            if (vadj) g_signal_handler_disconnect(vadj, self->viewport_scroll_handler_id);
            self->viewport_scroll_handler_id = 0;
        }
        return G_SOURCE_REMOVE;
    }
    
    gboolean regex = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->regex_check));
    gboolean case_sensitive = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->case_check));
    gboolean whole_word = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->word_check));
    
    /* UNIFIED APPROACH: Use async search for ALL files.
     * Stores matches in mmap'd file (disk-backed).
     * Uses binary search for O(log N) viewport lookup.
     * Highlights only visible matches to keep UI memory low. */
    gtk_label_set_text(GTK_LABEL(self->matches_label), "Finding...");
    gtk_widget_set_visible(self->matches_label, TRUE);
    
    self->current_search = document_search_async_start(doc, text, regex, case_sensitive, whole_word, on_search_update, self);
    
    /* Set active search on editor for global navigation */
    editor_widget_set_active_search(self->editor, self->current_search);
    
    /* Connect scroll handler to update highlights on scroll */
    if (!self->viewport_scroll_handler_id && self->editor) {
        GtkAdjustment *vadj = editor_widget_get_vadjustment(self->editor);
        if (vadj) {
            self->viewport_scroll_handler_id = g_signal_connect(vadj, "value-changed", 
                G_CALLBACK(on_viewport_scroll), self);
        }
    }
    
    return G_SOURCE_REMOVE;
}


static void on_search_changed(GtkWidget *widget, gpointer user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    if (self->search_timeout_id) g_source_remove(self->search_timeout_id);
    self->search_timeout_id = g_timeout_add(200, (GSourceFunc)perform_search, self);
}

/* Document modification handler */
static void on_document_changed(Document *doc, gboolean modified, void *user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    
    /* If a replace operation is running (or just finishing), don't auto-search. 
       The replace op manages its own state and we don't want to re-scan immediately. */
    if (self->current_replace_task) {
        return;
    }

    /* If document changed, re-trigger search to update/clear highlights.
       Debounce this to avoid hammer during typing. */
    if (self->search_timeout_id) g_source_remove(self->search_timeout_id);
    self->search_timeout_id = g_timeout_add(300, (GSourceFunc)perform_search, self);
}

static void on_next_clicked(GtkButton *btn, gpointer user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    editor_widget_next_match(self->editor);
}

static void on_prev_clicked(GtkButton *btn, gpointer user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    editor_widget_prev_match(self->editor);
}

static void on_replace_clicked(GtkButton *btn, gpointer user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    const char *repl = gtk_editable_get_text(GTK_EDITABLE(self->replace_entry));
    
    gboolean regex = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->regex_check));
    /* We need the Pattern text! Find entry has it. */
    const char *pattern_text = gtk_editable_get_text(GTK_EDITABLE(self->find_entry));
    
    editor_widget_replace_current(self->editor, repl, regex, pattern_text);
    /* Re-trigger search to update offsets */
    perform_search(self);
}


static void on_replace_progress(int processed, int total, gboolean finished, void *user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    
    /* Check for error condition (disk space, etc.) */
    if (processed == -1) {
        self->current_replace_task = NULL;
        self->current_streaming_replace = NULL;
        gtk_button_set_label(GTK_BUTTON(self->replace_all_btn), "Replace All");
        gtk_label_set_text(GTK_LABEL(self->matches_label), "Error: Insufficient disk space in /tmp");
        gtk_widget_set_visible(self->matches_label, TRUE);
        
        return;
    }
    
    if (finished) {
        /* Task Done - clear both task types */
        self->current_replace_task = NULL;
        self->current_streaming_replace = NULL;
        gtk_button_set_label(GTK_BUTTON(self->replace_all_btn), "Replace All");
        
        char *msg = g_strdup_printf("Done (%d replaced)", total);
        gtk_label_set_text(GTK_LABEL(self->matches_label), msg);
        g_free(msg);
        
        /* Force syntax highlight refresh to prevent stale cache issues after massive change */
        editor_widget_refresh_syntax(self->editor);
        
        /* Reset Cursors to 0 to prevent OOB/Stale offset issues */
        editor_widget_reset_cursor_to_start(self->editor);
        
        /* Invalidate outdated search results */
        if (self->current_search) {
             document_search_async_cancel(self->current_search);
             self->current_search = NULL;
        }
        
    } else {
        /* Progress */
        double pct = (total > 0) ? (double)processed / total * 100.0 : 0.0;
        char *msg = g_strdup_printf("Replacing... %.0f%% (%d/%d)", pct, processed, total);
        gtk_label_set_text(GTK_LABEL(self->matches_label), msg);
        gtk_widget_set_visible(self->matches_label, TRUE);
        g_free(msg);
    }
}

static void on_replace_all_clicked(GtkButton *btn, gpointer user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    
    /* Toggle / Cancel Logic - check both task types */
    if (self->current_replace_task) {
        document_replace_async_cancel(self->current_replace_task);
        self->current_replace_task = NULL;
        gtk_button_set_label(GTK_BUTTON(self->replace_all_btn), "Replace All");
        gtk_label_set_text(GTK_LABEL(self->matches_label), "Cancelled");
        return;
    }
    if (self->current_streaming_replace) {
        document_replace_streaming_cancel(self->current_streaming_replace);
        self->current_streaming_replace = NULL;
        gtk_button_set_label(GTK_BUTTON(self->replace_all_btn), "Replace All");
        gtk_label_set_text(GTK_LABEL(self->matches_label), "Cancelled");
        return;
    }

    Document *doc = editor_widget_get_document(self->editor);
    if (!doc) return;
    
    const char *query = gtk_editable_get_text(GTK_EDITABLE(self->find_entry));
    const char *repl = gtk_editable_get_text(GTK_EDITABLE(self->replace_entry));
    
    /* Early return if query is empty - prevents UI freeze */
    if (!query || !*query) {
        gtk_label_set_text(GTK_LABEL(self->matches_label), "Enter search text");
        gtk_widget_set_visible(self->matches_label, TRUE);
        return;
    }
    
    gboolean regex = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->regex_check));
    gboolean case_sensitive = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->case_check));
    gboolean whole_word = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->word_check));
    /* UNIFIED APPROACH: Always use streaming replace.
     * This scans the document line-by-line and replaces on-the-fly,
     * without storing all matches in memory. Works efficiently for any file size. */
    
    /* Clear any search highlights and cancel active search */
    if (self->current_search) {
        document_search_async_cancel(self->current_search);
        self->current_search = NULL;
    }
    editor_widget_set_search_results(self->editor, NULL);
    
    gtk_label_set_text(GTK_LABEL(self->matches_label), "Replacing...");
    gtk_widget_set_visible(self->matches_label, TRUE);
    
    self->current_streaming_replace = document_replace_streaming_start(
        doc, query, repl, regex, case_sensitive, whole_word, 
        on_replace_progress, self);
    
    if (self->current_streaming_replace) {
        gtk_button_set_label(GTK_BUTTON(self->replace_all_btn), "Stop");
    } else {
        gtk_label_set_text(GTK_LABEL(self->matches_label), "Replace error");
    }
}

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    vite_find_replace_bar_close(self);
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    if (keyval == GDK_KEY_Escape) {
        vite_find_replace_bar_close(self);
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (state & GDK_SHIFT_MASK) {
             editor_widget_prev_match(self->editor);
        } else {
             editor_widget_next_match(self->editor);
        }
        return TRUE;
    }
    return FALSE;
}

static void
on_editor_undo_redo_progress(EditorWidget *editor, double progress, gboolean finished, gpointer user_data)
{
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    (void)editor;
    
    /* When undo/redo starts, cancel active searches and filters */
    if (!finished && progress == 0.0) {
        if (self->current_search) {
             document_search_async_cancel(self->current_search);
             self->current_search = NULL;
             /* Clear highlights since content is about to change significantly */
             editor_widget_set_search_results(self->editor, NULL);
        }
        if (self->current_filter_task) {
             document_filter_async_cancel(self->current_filter_task);
             self->current_filter_task = NULL;
        }
        /* For Replace All, we already cancel it in on_replace_all_clicked, 
           but if user triggers undo MID-replace (though UI is blocked), 
           it's good to be safe. */
    }
}

static void vite_find_replace_bar_class_init(ViteFindReplaceBarClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    
    
    object_class->dispose = vite_find_replace_bar_dispose;
    
    signals[SIGNAL_PROGRESS_CHANGED] = g_signal_new("progress-changed",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2, G_TYPE_DOUBLE, G_TYPE_BOOLEAN);
    
    gtk_widget_class_set_css_name(widget_class, "findbar");
}

static void vite_find_replace_bar_init(ViteFindReplaceBar *self) {
    /* Constructed in new */
}

GtkWidget *vite_find_replace_bar_new(EditorWidget *editor) {
    ViteFindReplaceBar *self = g_object_new(VITE_TYPE_FIND_REPLACE_BAR, NULL);
    self->editor = editor;
    
    g_signal_connect(editor, "caret-moved", G_CALLBACK(on_caret_moved), self);

    
    /* Main Layout */
    /* Self is the GtkBox container (defined in G_DEFINE_TYPE or via parent instance init?) 
       Wait, ViteFindReplaceBar is a GtkBox subclass? 
       Let's check header/init... usually yes. 
       If so, we just append to self. 
       Actually, previous code did `gtk_box_new` and `gtk_widget_set_parent`. 
       If ViteFindReplaceBar IS a GtkBox, we should just append to it.
       If it's a generic GtkWidget/GtkBin, we set child.
       
       Checking type definition... usually `G_DECLARE_FINAL_TYPE(ViteFindReplaceBar, ... GtkBox)`.
       Assuming it is a Box based on usage `GTK_BOX(self)`.
       
       Let's assume it IS a GtkBox (common practice).
    */
    
    gtk_widget_add_css_class(GTK_WIDGET(self), "find-bar");
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing(GTK_BOX(self), 0);
    
    /* Row 1: Find + Nav + Toggle + Options + Close */
    GtkWidget *row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(self), row1);
    
    /* Composite Find Entry Box */
    /* We create a Box that *looks* like an entry */
    /* Find Entry Overlay */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_widget_set_hexpand(overlay, TRUE);
    gtk_box_append(GTK_BOX(row1), overlay);

    /* Search Entry */
    self->find_entry = gtk_search_entry_new();
    gtk_widget_set_hexpand(self->find_entry, TRUE);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->find_entry), "Find");
    g_signal_connect(self->find_entry, "search-changed", G_CALLBACK(on_search_changed), self);
    
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(self->find_entry, key_ctrl);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), self->find_entry);

    /* Matches Label (Overlay) */
    self->matches_label = gtk_label_new("");
    gtk_widget_add_css_class(self->matches_label, "dim-label");
    gtk_widget_add_css_class(self->matches_label, "caption"); /* small font */
    gtk_widget_set_halign(self->matches_label, GTK_ALIGN_END);
    gtk_widget_set_valign(self->matches_label, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_end(self->matches_label, 30); /* Avoid clear icon overlap */
    gtk_widget_set_can_target(self->matches_label, FALSE); /* Click-through */
    gtk_widget_set_visible(self->matches_label, FALSE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), self->matches_label);

    /* Navigation (Up/Down) */
    GtkWidget *nav_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(nav_box, "linked");
    GtkWidget *prev_btn = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(prev_btn, "Previous Match");
    g_signal_connect(prev_btn, "clicked", G_CALLBACK(on_prev_clicked), self);
    gtk_box_append(GTK_BOX(nav_box), prev_btn);
    GtkWidget *next_btn = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(next_btn, "Next Match");
    g_signal_connect(next_btn, "clicked", G_CALLBACK(on_next_clicked), self);
    gtk_box_append(GTK_BOX(nav_box), next_btn);
    gtk_box_append(GTK_BOX(row1), nav_box);

    /* Toggle Replace Button (After Nav) */
    GtkWidget *toggle_repl_btn = gtk_button_new_from_icon_name("view-more-symbolic");
    gtk_button_set_icon_name(GTK_BUTTON(toggle_repl_btn), "edit-find-replace-symbolic");
    gtk_widget_set_tooltip_text(toggle_repl_btn, "Toggle Replace");
    gtk_widget_add_css_class(toggle_repl_btn, "flat");
    g_signal_connect_swapped(toggle_repl_btn, "clicked", G_CALLBACK(vite_find_replace_bar_toggle_replace), self);
    gtk_box_append(GTK_BOX(row1), toggle_repl_btn);
    self->toggle_repl_btn = toggle_repl_btn;

    /* Options Menu */
    GtkWidget *options_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(options_btn), "system-run-symbolic");
    gtk_widget_add_css_class(options_btn, "flat");
    
    GtkWidget *popover = gtk_popover_new();
    GtkWidget *pop_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(pop_box, 12);
    gtk_widget_set_margin_bottom(pop_box, 12);
    gtk_widget_set_margin_start(pop_box, 12);
    gtk_widget_set_margin_end(pop_box, 12);
    
    self->regex_check = gtk_check_button_new_with_label("Regular Expressions");
    g_signal_connect(self->regex_check, "toggled", G_CALLBACK(on_search_changed), self);
    gtk_box_append(GTK_BOX(pop_box), self->regex_check);
    
    self->case_check = gtk_check_button_new_with_label("Case Sensitive");
    g_signal_connect(self->case_check, "toggled", G_CALLBACK(on_search_changed), self);
    gtk_box_append(GTK_BOX(pop_box), self->case_check);

    self->word_check = gtk_check_button_new_with_label("Match Whole Word");
    g_signal_connect(self->word_check, "toggled", G_CALLBACK(on_search_changed), self);
    gtk_box_append(GTK_BOX(pop_box), self->word_check);
    
    gtk_popover_set_child(GTK_POPOVER(popover), pop_box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(options_btn), popover);
    gtk_box_append(GTK_BOX(row1), options_btn);
    
    /* Matches Label (Removed from here, moved to find_wrapper) */
    /* self->matches_label handled above */

    /* Close Button */
    GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(close_btn, "flat");
    g_signal_connect(close_btn, "clicked", G_CALLBACK(on_close_clicked), self);
    gtk_box_append(GTK_BOX(row1), close_btn);
    
    /* Row 2: Replace */
    self->replace_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(self->replace_box, 6); /* Add some spacing from row1 */
    gtk_widget_set_visible(self->replace_box, FALSE);
    gtk_box_append(GTK_BOX(self), self->replace_box);
    
    /* Spacer to align with Find... 
       Left side is roughly 6px + width of icon ~20px + margin 8px = ~34px? 
       Actually, `find_wrapper` has an icon. `replace_entry` might also need an icon to align text?
       Or just an icon in replace entry for "Replace"?
       Current replace entry has icon at start `GTK_ENTRY_ICON_PRIMARY`. 
       So text alignment should be roughly similar if icon sizing matches.
    */
    
    self->replace_entry = gtk_entry_new();
    gtk_widget_set_hexpand(self->replace_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(self->replace_entry), "Replace");
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(self->replace_entry), GTK_ENTRY_ICON_PRIMARY, "edit-find-replace-symbolic");
    gtk_box_append(GTK_BOX(self->replace_box), self->replace_entry);
    
    /* Replace Status Label */
    self->replace_status_label = gtk_label_new("");
    gtk_widget_add_css_class(self->replace_status_label, "dim-label");
    gtk_widget_add_css_class(self->replace_status_label, "caption");
    gtk_widget_set_margin_end(self->replace_status_label, 6);
    gtk_widget_set_visible(self->replace_status_label, FALSE);
    gtk_box_append(GTK_BOX(self->replace_box), self->replace_status_label);
    
    GtkWidget *do_repl_btn = gtk_button_new_with_label("Replace");
    g_signal_connect(do_repl_btn, "clicked", G_CALLBACK(on_replace_clicked), self);
    gtk_box_append(GTK_BOX(self->replace_box), do_repl_btn);
    
    self->replace_all_btn = gtk_button_new_with_label("Replace All");
    g_signal_connect(self->replace_all_btn, "clicked", G_CALLBACK(on_replace_all_clicked), self);
    gtk_box_append(GTK_BOX(self->replace_box), self->replace_all_btn);
    
    /* Listen for document changes */
    Document *doc = editor_widget_get_document(editor);
    if (doc) {
        document_add_modification_callback(doc, on_document_changed, self);
    }
    
    /* Listen for undo/redo on the editor to cancel search if needed */
    g_signal_connect(editor, "undo-redo-progress", G_CALLBACK(on_editor_undo_redo_progress), self);
    
    return GTK_WIDGET(self);
}

void vite_find_replace_bar_toggle_replace(ViteFindReplaceBar *bar) {
    gboolean vis = gtk_widget_get_visible(bar->replace_box);
    gtk_widget_set_visible(bar->replace_box, !vis);
    if (!vis) gtk_widget_grab_focus(bar->replace_entry);
    else gtk_widget_grab_focus(bar->find_entry);
}

static void set_filter_mode(ViteFindReplaceBar *bar, gboolean enabled) {
    if (bar->filter_mode == enabled) return;
    
    bar->filter_mode = enabled;
    
    /* Clean up previous state */
    /* If switching FROM Filter Mode -> Clear Filter results */
    if (!enabled) {
         if (bar->current_filter_task) {
             document_filter_async_cancel(bar->current_filter_task);
             bar->current_filter_task = NULL;
         }
         if (bar->filter_tick_id) {
             g_source_remove(bar->filter_tick_id);
             bar->filter_tick_id = 0;
         }
         if (bar->current_filter_result) {
             filter_result_free(bar->current_filter_result);
             bar->current_filter_result = NULL;
         }
         /* Clear Editor Filter State */
         editor_widget_set_filtered_lines(bar->editor, NULL, NULL, FALSE, FALSE);
         
         /* Restore UI for Find */
         gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(bar->find_entry), "Find");
         gtk_widget_set_visible(bar->toggle_repl_btn, TRUE);
         
    } else {
        /* Switching TO Filter Mode */
        /* Clear Find State */
        if (bar->current_search) {
            document_search_async_cancel(bar->current_search);
            bar->current_search = NULL;
        }
        editor_widget_clear_search(bar->editor);
        gtk_widget_set_visible(bar->replace_box, FALSE);
        
        /* Update UI for Filter */
        gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(bar->find_entry), "Filter lines (Ctrl+Alt+F)");
        gtk_widget_set_visible(bar->toggle_repl_btn, FALSE);
    }
    
    /* Re-evaluate with empty/current text */
    /* Clear label */
    gtk_widget_set_visible(bar->matches_label, FALSE);
    
    /* Trigger search/filter if text exists */
    const char *text = gtk_editable_get_text(GTK_EDITABLE(bar->find_entry));
    if (text && *text) {
        perform_search(bar);
    }
}

void vite_find_replace_bar_show(ViteFindReplaceBar *bar) {
    set_filter_mode(bar, FALSE);
    gtk_widget_set_visible(GTK_WIDGET(bar), TRUE);
    gtk_widget_grab_focus(bar->find_entry);
    
    /* Ensure we listen to doc if it changed (e.g. new file) */
    /* Re-fetch doc just in case */
    Document *doc = editor_widget_get_document(bar->editor);
    /* TODO: remove old callback if doc changed? 
       For now assume 1 doc per window lifetime or handle dispose.
       Actually main window switches docs. EditorWidget switches docs?
       EditorWidget seems to have one doc for its life or strict 1:1.
    */
    (void)doc; 
}



void vite_find_replace_bar_show_filter(ViteFindReplaceBar *bar) {
    set_filter_mode(bar, TRUE);
    gtk_widget_set_visible(GTK_WIDGET(bar), TRUE);
    gtk_widget_grab_focus(bar->find_entry);
}

void vite_find_replace_bar_close(ViteFindReplaceBar *bar) {
    gtk_widget_set_visible(GTK_WIDGET(bar), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(bar->find_entry), "");
    
    if (bar->filter_mode) {
         /* Clear filter results */
         editor_widget_set_filtered_lines(bar->editor, NULL, NULL, FALSE, FALSE);
         if (bar->current_filter_task) {
             document_filter_async_cancel(bar->current_filter_task);
             bar->current_filter_task = NULL;
         }
         if (bar->current_filter_result) {
             filter_result_free(bar->current_filter_result);
             bar->current_filter_result = NULL;
         }
         editor_widget_scroll_to_cursor(bar->editor);
    } else {
         if (bar->current_search) {
             document_search_async_cancel(bar->current_search);
             bar->current_search = NULL;
         }
    }
    
    /* Always clear any search state to avoid stale/dangling pointers */
    editor_widget_clear_search(bar->editor);
    
    gtk_widget_grab_focus(GTK_WIDGET(bar->editor));
}

void vite_find_replace_bar_set_search_text(ViteFindReplaceBar *bar, const char *text) {
    if (!bar || !text) return;
    gtk_editable_set_text(GTK_EDITABLE(bar->find_entry), text);
}

void vite_find_replace_bar_show_replace(ViteFindReplaceBar *bar, gboolean has_search_text) {
    if (!bar) return;
    set_filter_mode(bar, FALSE);
    gtk_widget_set_visible(GTK_WIDGET(bar), TRUE);
    gtk_widget_set_visible(bar->replace_box, TRUE);
    
    /* Focus replace entry only if there's search text, otherwise focus find entry */
    if (has_search_text) {
        gtk_widget_grab_focus(bar->replace_entry);
    } else {
        gtk_widget_grab_focus(bar->find_entry);
    }
}
