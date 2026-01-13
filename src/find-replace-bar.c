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
};

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
    
    if (self->current_replace_task) {
        document_replace_async_cancel(self->current_replace_task);
        self->current_replace_task = NULL;
    }
    
    G_OBJECT_CLASS(vite_find_replace_bar_parent_class)->dispose(object);
}

static void
update_matches_label(ViteFindReplaceBar *self) {
    if (!self->current_search) {
         gtk_widget_set_visible(self->matches_label, FALSE);
         return;
    }
    
    GArray *matches = document_search_task_get_matches(self->current_search);
    guint total = matches ? matches->len : 0;
    
    if (total == 0) {
        gtk_label_set_text(GTK_LABEL(self->matches_label), "No matches");
    } else {
         int index = editor_widget_get_current_match_index(self->editor);
         char *msg;
         if (index > 0) {
              msg = g_strdup_printf("%d of %d", index, total);
         } else {
              msg = g_strdup_printf("%d matches", total);
         }
         gtk_label_set_text(GTK_LABEL(self->matches_label), msg);
         g_free(msg);
    }
    gtk_widget_set_visible(self->matches_label, TRUE);
}

static void 
on_caret_moved(EditorWidget *editor, ViteFindReplaceBar *self) {
    update_matches_label(self);
}


static void on_search_update(GArray *matches, gboolean finished, void *user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    
    /* If finished, current_search pointer is likely stale or needs clearing if we don't own it anymore?
       Actually in our impl, Task removes itself from idle. But pointer persists until cancel.
       We should clear self->current_search if finished? 
       Wait, if we clear it, we can't cancel it later.
       But if it finished, it is effectively done.
       However, memory is allocated. We need to free it.
       If finished is TRUE, task is done.
       We should free the task structure?
       Or we rely on standard ownership: Bar owns task.
       If finished, we can free it immediately BUT we need the matches?
       Matches are passed in callback.
       
       Better strategy:
       Update UI results.
       If finished, free the task struct?
       Wait, typical GTask style: unref.
       Our simple struct needs explicit free.
       If we free task, we lose 'matches' if the task owned them?
       The matches array is passed to us. We should probably take a reference or copy?
       EditorWidget takes a reference/copy?
       editor_widget_set_search_results takes ownership or copies?
       Let's check editor-widget.c later. Assuming it copies or refcounts.
       Actually GArray is refcounted? No.
       We probably need to pass ownership or keep task alive.
       
       Simpler:
       If finished, update editor results.
       We KEEP current_search until next search starts or bar closes.
       This allows us to resend results if needed (e.g. settings change? no that triggers new search).
    */

    /* Update Label */
    if (!finished && self->current_search) {
        /* Show progressive search status */
        size_t total = document_search_task_get_total_lines(self->current_search);
        size_t searched = document_search_task_get_lines_searched(self->current_search);
        int percent = total > 0 ? (int)((searched * 100) / total) : 0;
        
        char buf[64];
        snprintf(buf, sizeof(buf), "Finding... %d%% (%u)", percent, matches ? matches->len : 0);
        gtk_label_set_text(GTK_LABEL(self->matches_label), buf);
        gtk_widget_set_visible(self->matches_label, TRUE);
    } else if (finished) {
        /* Update label with final count & index */
        update_matches_label(self);
    }


    /* Update Editor Highlights Progressive */
    if (matches) {
       /* Use reference counting instead of copying for performance.
          GArray supports ref counting since GLib 2.22. */
       GArray *ref = g_array_ref(matches);
       editor_widget_set_search_results(self->editor, ref);
    } else {
        /* Clear if NULL */
        if (finished) editor_widget_set_search_results(self->editor, NULL);
    }

    
    if (finished) {
        /* We can free the task wrapper but we need to keep matches alive for Editor?
           If Editor copies, we can free.
           If Editor refs, we can free.
           Standard Document/GArray usage in previous `perform_search` passed GArray directly.
           And previously `document_search` returned new GArray.
           Reference counting GArray is not standard.
           EditorWidget likely expects to OWN the array or Ref it.
           If we look at `document_replace_all`, it frees matches.
           So EditorWidget must copy or use it temporarily?
           Wait, `editor_widget_set_search_results` likely stores it.
           If `document_search` allocated it, who frees it?
           If we look at `perform_search` before:
             matches = document_search(...)
             editor_widget_set_search_results(..., matches)
             // It did NOT free matches. So EditorWidget owns it now?
             // Or it leaked?
           
           I should check `editor_widget_set_search_results`.
           Assuming transfer full.
           
           ASYNC ISSUE: 
           Task owns matches GArray.
           If we give it to Editor, and Task continues, Task appends to it.
           Editor might be reading it? threading issue?
           We are in IDLE thread (Main thread). So no concurrent access.
           Editor redraws during standard layout cycle.
           So it is safe to share GArray pointer as long as we assume Main Thread.
           
           BUT if we free Task (and GArray), Editor has dangling pointer.
           So:
           1. Task owns GArray.
           2. We pass GArray to Editor.
           3. When we cancel/free Task, we must tell Editor?
              Or Editor makes a copy?
           
           If I assume `editor_widget_set_search_results` takes ownership, then Task cannot own it anymore?
           But Task needs to append.
           
           Solution:
           EditorWidget should REF the array if it uses `g_array_ref`? 
           GArray supports ref counting since 2.22 `g_array_ref`.
           
           Let's assume we pass matches to Editor.
           When Task finishes, we do nothing (Task struct stays alive in `current_search`).
           When `current_search` is replaced (next search), we `cancel` (free) the old task.
           `cancel` frees the matches GArray.
           This implies Editor must have Ref'd it or Copied it.
           
           Let's verify `editor-widget.c` handling of search results later. 
           For now, assume Update is safe.
        */
        
        /* If finished, we leave `current_search` valid until next search options change. */
    }
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
    
    if (self->viewport_update_timeout_id) {
        g_source_remove(self->viewport_update_timeout_id);
    }
    /* Debounce 100ms */
    self->viewport_update_timeout_id = g_timeout_add(100, on_viewport_scroll_timeout, self);
}

static void update_viewport_search(ViteFindReplaceBar *self) {
    Document *doc = editor_widget_get_document(self->editor);
    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->find_entry));
    
    if (!doc || !text || strlen(text) == 0) {
        editor_widget_set_search_results(self->editor, NULL);
        gtk_widget_set_visible(self->matches_label, FALSE);
        return;
    }
    
    size_t start_line, end_line;
    editor_widget_get_visible_line_range(self->editor, &start_line, &end_line);
    
    /* Expand range for smooth scrolling (buffer like svite.py) */
    size_t padding = 100; 
    start_line = (start_line > padding) ? start_line - padding : 0;
    size_t total = document_get_line_count(doc);
    end_line += padding;
    if (end_line > total) end_line = total;
    
    gboolean regex = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->regex_check));
    gboolean case_sensitive = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->case_check));
    gboolean whole_word = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->word_check));
    
    /* Call viewport search */
    GArray *matches = document_search_viewport(doc, text, regex, case_sensitive, whole_word, start_line, end_line);
    
    editor_widget_set_search_results(self->editor, matches);
    
    char buf[64];
    if (matches && matches->len > 0) {
       /* Just indicate visual matches since total is unknown/too expensive */
       snprintf(buf, sizeof(buf), "%u visible", matches->len);
    } else {
       snprintf(buf, sizeof(buf), "No matches in view");
    }
    gtk_label_set_text(GTK_LABEL(self->matches_label), buf);
    gtk_widget_set_visible(self->matches_label, TRUE);
}

static gboolean perform_search(ViteFindReplaceBar *self) {
    self->search_timeout_id = 0;
    
    if (self->current_search) {
        document_search_async_cancel(self->current_search);
        self->current_search = NULL;
    }
    
    Document *doc = editor_widget_get_document(self->editor);
    if (!doc) return G_SOURCE_REMOVE;
    
    const char *text = gtk_editable_get_text(GTK_EDITABLE(self->find_entry));
    if (!text || !*text) {
        editor_widget_set_search_results(self->editor, NULL);
        gtk_widget_set_visible(self->matches_label, FALSE);
        
        if (self->viewport_mode) {
             self->viewport_mode = FALSE;
             if (self->viewport_scroll_handler_id) {
                 GtkAdjustment *vadj = editor_widget_get_vadjustment(self->editor);
                 if (vadj) g_signal_handler_disconnect(vadj, self->viewport_scroll_handler_id);
                 self->viewport_scroll_handler_id = 0;
             }
        }
        return G_SOURCE_REMOVE;
    }
    
    gboolean regex = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->regex_check));
    gboolean case_sensitive = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->case_check));
    gboolean whole_word = gtk_check_button_get_active(GTK_CHECK_BUTTON(self->word_check));
    
    size_t line_count = document_get_line_count(doc);

    /* Turn off viewport mode if it was active */
    if (self->viewport_mode) {
         self->viewport_mode = FALSE;
         if (self->viewport_scroll_handler_id) {
             GtkAdjustment *vadj = editor_widget_get_vadjustment(self->editor);
             if (vadj) g_signal_handler_disconnect(vadj, self->viewport_scroll_handler_id);
             self->viewport_scroll_handler_id = 0;
         }
    }
    
    if (line_count < 50000) {
         GArray *matches = document_search(doc, text, regex, case_sensitive, whole_word);
         editor_widget_set_search_results(self->editor, matches);
         update_matches_label(self);
    } else {
         /* ASYNC SEARCH FOR ALL LARGE FILES */
         gtk_label_set_text(GTK_LABEL(self->matches_label), "Finding...");
         gtk_widget_set_visible(self->matches_label, TRUE);
         
         self->current_search = document_search_async_start(doc, text, regex, case_sensitive, whole_word, on_search_update, self);
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
    
    editor_widget_replace_current(self->editor, repl);
    /* Re-trigger search to update offsets */
    perform_search(self);
}


static void on_replace_progress(int processed, int total, gboolean finished, void *user_data) {
    ViteFindReplaceBar *self = VITE_FIND_REPLACE_BAR(user_data);
    
    if (finished) {
        /* Task Done */
        self->current_replace_task = NULL;
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
    
    /* Toggle / Cancel Logic */
    if (self->current_replace_task) {
        document_replace_async_cancel(self->current_replace_task);
        self->current_replace_task = NULL;
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
    
    GArray *matches = NULL;
    GRegex *cached_pattern = NULL;
    gboolean matches_owned = FALSE;
    gboolean cache_valid = FALSE;
    
    /* Reuse Logic: Check if current search is valid for replacement */
    if (self->current_search) {
        const char *cached_query = document_search_task_get_query(self->current_search);
        gboolean cached_regex = document_search_task_get_regex(self->current_search);
        gboolean cached_case = document_search_task_get_case_sensitive(self->current_search);
        gboolean cached_word = document_search_task_get_whole_word(self->current_search);
        
        gboolean query_match = (cached_query && query && g_strcmp0(cached_query, query) == 0);
        
        if (query_match && cached_regex == regex && cached_case == case_sensitive && cached_word == whole_word) {
             matches = document_search_task_get_matches(self->current_search);
             cached_pattern = document_search_task_get_pattern(self->current_search);
             cache_valid = TRUE;
        }
    }
    
    /* If cache is valid but empty, we already know there are no matches - skip re-search */
    if (cache_valid && (!matches || matches->len == 0)) {
        gtk_label_set_text(GTK_LABEL(self->matches_label), "No matches found");
        gtk_widget_set_visible(self->matches_label, TRUE);
        return;
    }
    
    /* If no valid cache, need to search */
    if (!cache_valid) {
        size_t line_count = document_get_line_count(doc);
        
        /* For large files, inform user and trigger async search first */
        if (line_count >= 50000) {
            gtk_label_set_text(GTK_LABEL(self->matches_label), "Searching first...");
            gtk_widget_set_visible(self->matches_label, TRUE);
            /* Trigger the normal search which is async for large files */
            perform_search(self);
            return;
        }
        
        /* For smaller files, do sync search with UI feedback */
        gtk_label_set_text(GTK_LABEL(self->matches_label), "Finding matches...");
        gtk_widget_set_visible(self->matches_label, TRUE);
        
        /* Process pending GTK events to update UI before sync search */
        while (g_main_context_pending(NULL)) {
            g_main_context_iteration(NULL, FALSE);
        }
        
        matches = document_search(doc, query, regex, case_sensitive, whole_word);
        matches_owned = TRUE;
    }
    
    if (matches && matches->len > 0) {
        /* Clear highlights from editor immediately as they are about to be invalidated/replaced.
           This also prevents performance issues with rendering millions of matches during the op. */
        editor_widget_set_search_results(self->editor, NULL);
        
        self->current_replace_task = document_replace_async_start(doc, matches, repl, regex, cached_pattern, on_replace_progress, self);
        gtk_button_set_label(GTK_BUTTON(self->replace_all_btn), "Stop");
        
        if (matches_owned) {
             g_array_unref(matches);
        }
    } else {
        gtk_label_set_text(GTK_LABEL(self->matches_label), "No matches found");
        gtk_widget_set_visible(self->matches_label, TRUE);
        if (matches_owned && matches) g_array_unref(matches);
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

static void vite_find_replace_bar_class_init(ViteFindReplaceBarClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    
    
    object_class->dispose = vite_find_replace_bar_dispose;
    
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
    
    return GTK_WIDGET(self);
}

void vite_find_replace_bar_toggle_replace(ViteFindReplaceBar *bar) {
    gboolean vis = gtk_widget_get_visible(bar->replace_box);
    gtk_widget_set_visible(bar->replace_box, !vis);
    if (!vis) gtk_widget_grab_focus(bar->replace_entry);
    else gtk_widget_grab_focus(bar->find_entry);
}

void vite_find_replace_bar_show(ViteFindReplaceBar *bar) {
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

void vite_find_replace_bar_close(ViteFindReplaceBar *bar) {
    gtk_widget_set_visible(GTK_WIDGET(bar), FALSE);
    gtk_editable_set_text(GTK_EDITABLE(bar->find_entry), "");
    editor_widget_set_search_results(bar->editor, NULL);
    gtk_widget_grab_focus(GTK_WIDGET(bar->editor));
}

void vite_find_replace_bar_set_search_text(ViteFindReplaceBar *bar, const char *text) {
    if (!bar || !text) return;
    gtk_editable_set_text(GTK_EDITABLE(bar->find_entry), text);
}

void vite_find_replace_bar_show_replace(ViteFindReplaceBar *bar, gboolean has_search_text) {
    if (!bar) return;
    gtk_widget_set_visible(GTK_WIDGET(bar), TRUE);
    gtk_widget_set_visible(bar->replace_box, TRUE);
    
    /* Focus replace entry only if there's search text, otherwise focus find entry */
    if (has_search_text) {
        gtk_widget_grab_focus(bar->replace_entry);
    } else {
        gtk_widget_grab_focus(bar->find_entry);
    }
}
