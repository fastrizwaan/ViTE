#ifndef THEME_MANAGER_H
#define THEME_MANAGER_H

#include <gtk/gtk.h>

typedef struct {
    char *name;
    char *file_path;
    void *theme_data;  // Using void* to avoid needing json-glib in header
} ThemeInfo;

GPtrArray *theme_manager_load_themes(void);
void theme_manager_free_themes(GPtrArray *themes);
const char *theme_manager_get_theme_path(const char *theme_name);
void theme_manager_apply_theme(const char *theme_name);

#endif