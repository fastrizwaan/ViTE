#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include "../src/document.h"

static GMainLoop *loop;

static ReplaceTask *global_task = NULL; // Hack to access it

static void
on_replace_progress(int processed, int total, gboolean finished, void *user_data)
{
    printf("Replace Progress: %d/%d (Finished: %d)\n", processed, total, finished);
    if (finished) {
        printf("Replace Finished!\n");
        if (global_task) {
             document_replace_async_cancel(global_task);
             global_task = NULL;
        }
        g_main_loop_quit(loop);
    }
}

static void
on_search_ready(GArray *matches, gboolean finished, void *user_data)
{
    if (!finished) return;
    
    Document *doc = (Document *)user_data;
    printf("Search Finished. Found %u matches.\n", matches ? matches->len : 0);
    
    if (!matches || matches->len == 0) {
        printf("No matches found. Exiting.\n");
        g_main_loop_quit(loop);
        return;
    }
    
    printf("Starting Replace All...\n");
    GRegex *pattern = NULL; // Not used for literal
    ReplaceTask *task = document_replace_async_start(doc, matches, "x", FALSE, pattern, on_replace_progress, NULL);
    global_task = task;
    
    // Mimic the leak in find-replace-bar.c by NOT freeing the task here (it leaks in the app too)
    // But we are just testing if it crashes.
}

int main(int argc, char **argv) {
    setbuf(stdout, NULL);
    // gtk_init(); /* Skip GTK init to avoid display/gpu noise since we only test model */
    
    // Load undo.h
    const char *filename = "src/undo.h";
    // FILE *f = fopen(filename, "w"); // Don't overwrite it!
    // if (!f) return 1;
    // ...
    // fclose(f);
    
    Document *doc = document_new(filename);
    
    loop = g_main_loop_new(NULL, FALSE);
    
    // Search for "a" (common enough)
    printf("Starting Search for 'a' in %s...\n", filename);
    document_search_async_start(doc, "a", FALSE, TRUE, FALSE, on_search_ready, doc);
    
    g_main_loop_run(loop);
    
    document_free(doc);
    g_main_loop_unref(loop);
    
    return 0;
}
