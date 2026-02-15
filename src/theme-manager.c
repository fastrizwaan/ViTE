#include "theme-manager.h"
#ifdef HAVE_JSON_GLIB
#include <json-glib/json-glib.h>
#endif
#include <glib/gstdio.h>
#include <string.h>
#include "syntax.h"  // For syntax_set_theme_mode function

static GPtrArray *loaded_themes = NULL;
static GtkCssProvider *current_provider = NULL;

static void theme_info_free(ThemeInfo *theme) {
    if (!theme) return;
    g_free(theme->name);
    g_free(theme->file_path);
#ifdef HAVE_JSON_GLIB
    if (theme->theme_data) json_object_unref((JsonObject*)theme->theme_data);
#endif
    g_free(theme);
}

GPtrArray *theme_manager_load_themes(void) {
    if (loaded_themes) {
        g_ptr_array_unref(loaded_themes);
    }

    loaded_themes = g_ptr_array_new_with_free_func((GDestroyNotify)theme_info_free);

    // Get the themes directory path - look for it in the installation directory
    // First, try to find it relative to the executable
    char *exe_path = g_file_read_link("/proc/self/exe", NULL);
    char *themes_dir = NULL;

    if (exe_path) {
        char *bin_dir = g_path_get_dirname(exe_path);
        // Try multiple possible locations for the themes directory
        // 1. Relative to binary (for installed version)
        themes_dir = g_build_filename(bin_dir, "..", "share", "vite", "themes", NULL);

        // 2. Check if the themes directory appears to exist
        if (!g_file_test(themes_dir, G_FILE_TEST_IS_DIR)) {
            g_free(themes_dir);
            // Try relative to binary in development setup
            themes_dir = g_build_filename(bin_dir, "..", "vscode-themes", NULL);
        }

        // 3. Check if the themes directory exists
        if (!g_file_test(themes_dir, G_FILE_TEST_IS_DIR)) {
            g_free(themes_dir);
            // Try in the current working directory (development)
            themes_dir = g_build_filename(g_get_current_dir(), "vscode-themes", NULL);
        }

        // 4. Check if the themes directory exists
        if (!g_file_test(themes_dir, G_FILE_TEST_IS_DIR)) {
            g_free(themes_dir);
            // Try in the user's config directory
            themes_dir = g_build_filename(g_get_user_data_dir(), "vite", "themes", NULL);
        }

        g_free(exe_path);
    } else {
        // Fallback to current directory
        themes_dir = g_build_filename(g_get_current_dir(), "vscode-themes", NULL);
    }

#ifdef HAVE_JSON_GLIB
    GDir *dir = g_dir_open(themes_dir, 0, NULL);
    if (!dir) {
        g_warning("Could not open themes directory: %s", themes_dir);
        g_free(themes_dir);
        return loaded_themes;
    }

    const gchar *filename;
    while ((filename = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_suffix(filename, ".json")) {
            char *full_path = g_build_filename(themes_dir, filename, NULL);

            // Parse the theme file to extract the name
            GError *error = NULL;
            JsonParser *parser = json_parser_new();

            if (json_parser_load_from_file(parser, full_path, &error)) {
                JsonNode *root = json_parser_get_root(parser);

                if (JSON_NODE_HOLDS_OBJECT(root)) {
                    JsonObject *obj = json_node_get_object(root);

                    if (json_object_has_member(obj, "name")) {
                        const char *name = json_object_get_string_member(obj, "name");

                        ThemeInfo *theme = g_new0(ThemeInfo, 1);
                        theme->name = g_strdup(name);
                        theme->file_path = g_strdup(full_path);
                        theme->theme_data = (void*)json_object_ref(obj);

                        g_ptr_array_add(loaded_themes, theme);
                    }
                }
            } else {
                g_warning("Could not parse theme file %s: %s", full_path, error->message);
                g_error_free(error);
            }

            g_object_unref(parser);
            g_free(full_path);
        }
    }

    g_dir_close(dir);
#else
    g_warning("JSON theme support not compiled in - themes will not be loaded");
#endif

    g_free(themes_dir);

    return loaded_themes;
}

void theme_manager_free_themes(GPtrArray *themes) {
    if (themes) {
        g_ptr_array_unref(themes);
    }
}

const char *theme_manager_get_theme_path(const char *theme_name) {
    if (!loaded_themes || !theme_name) return NULL;

    for (guint i = 0; i < loaded_themes->len; i++) {
        ThemeInfo *theme = g_ptr_array_index(loaded_themes, i);
        if (g_strcmp0(theme->name, theme_name) == 0) {
            return theme->file_path;
        }
    }

    return NULL;
}

#ifdef HAVE_JSON_GLIB
static char* convert_vscode_theme_to_css(void *theme_obj_void) {
    JsonObject *theme_obj = (JsonObject*)theme_obj_void;
    GString *css = g_string_new("");

    // Extract colors from the theme object
    JsonObject *colors = json_object_get_object_member(theme_obj, "colors");
    if (colors) {
        // Process common theme colors
        if (json_object_has_member(colors, "editor.background")) {
            const char *bg_color = json_object_get_string_member(colors, "editor.background");
            g_string_append_printf(css, "textview.view { background-color: %s; }\n", bg_color);
            g_string_append_printf(css, "textview text { background-color: %s; }\n", bg_color);
        }

        if (json_object_has_member(colors, "editor.foreground")) {
            const char *fg_color = json_object_get_string_member(colors, "editor.foreground");
            g_string_append_printf(css, "textview.view { color: %s; }\n", fg_color);
            g_string_append_printf(css, "textview text { color: %s; }\n", fg_color);
        }

        if (json_object_has_member(colors, "editor.lineHighlightBackground")) {
            const char *hl_color = json_object_get_string_member(colors, "editor.lineHighlightBackground");
            g_string_append_printf(css, "textview text selection { background-color: %s; }\n", hl_color);
        }

        if (json_object_has_member(colors, "editor.selectionBackground")) {
            const char *sel_color = json_object_get_string_member(colors, "editor.selectionBackground");
            g_string_append_printf(css, "textview text selection { background-color: %s; }\n", sel_color);
        }

        // Add more color mappings as needed
        if (json_object_has_member(colors, "sideBar.background")) {
            const char *sidebar_bg = json_object_get_string_member(colors, "sideBar.background");
            g_string_append_printf(css, ".sidebar { background-color: %s; }\n", sidebar_bg);
        }

        if (json_object_has_member(colors, "titleBar.activeBackground")) {
            const char *title_bg = json_object_get_string_member(colors, "titleBar.activeBackground");
            g_string_append_printf(css, "headerbar { background-color: %s; }\n", title_bg);
        }

        if (json_object_has_member(colors, "titleBar.activeForeground")) {
            const char *title_fg = json_object_get_string_member(colors, "titleBar.activeForeground");
            g_string_append_printf(css, "headerbar { color: %s; }\n", title_fg);
        }

        if (json_object_has_member(colors, "statusBar.background")) {
            const char *status_bg = json_object_get_string_member(colors, "statusBar.background");
            g_string_append_printf(css, "statusbar { background-color: %s; }\n", status_bg);
        }

        if (json_object_has_member(colors, "statusBar.foreground")) {
            const char *status_fg = json_object_get_string_member(colors, "statusBar.foreground");
            g_string_append_printf(css, "statusbar { color: %s; }\n", status_fg);
        }
    }

    return g_string_free(css, FALSE);
}
#endif

void theme_manager_apply_theme(const char *theme_name) {
    if (!theme_name) return;

    // Remove the current theme if one exists
    if (current_provider) {
        gtk_style_context_remove_provider_for_display(
            gdk_display_get_default(),
            GTK_STYLE_PROVIDER(current_provider)
        );
        g_object_unref(current_provider);
        current_provider = NULL;
    }

    // Handle system default theme
    if (g_strcmp0(theme_name, "system") == 0 ||
        g_strcmp0(theme_name, "System Default") == 0) {
        // System default - no custom CSS applied
        return;
    }

#ifdef HAVE_JSON_GLIB
    // Handle common theme aliases
    const char *actual_theme_name = theme_name;
    if (g_strcmp0(theme_name, "light") == 0) {
        // Look for a light theme among loaded themes
        if (!loaded_themes) {
            theme_manager_load_themes();
        }
        
        for (guint i = 0; i < loaded_themes->len; i++) {
            ThemeInfo *theme = g_ptr_array_index(loaded_themes, i);
            if (g_strstr_len(theme->name, -1, "Light") || g_strstr_len(theme->name, -1, "light") || 
                g_strstr_len(theme->name, -1, "LIGHT")) {
                actual_theme_name = theme->name;
                break;
            }
        }
    } else if (g_strcmp0(theme_name, "dark") == 0) {
        // Look for a dark theme among loaded themes
        if (!loaded_themes) {
            theme_manager_load_themes();
        }
        
        for (guint i = 0; i < loaded_themes->len; i++) {
            ThemeInfo *theme = g_ptr_array_index(loaded_themes, i);
            if (g_strstr_len(theme->name, -1, "Dark") || g_strstr_len(theme->name, -1, "dark") || 
                g_strstr_len(theme->name, -1, "DARK")) {
                actual_theme_name = theme->name;
                break;
            }
        }
    }

    // Find the theme file
    const char *theme_path = theme_manager_get_theme_path(actual_theme_name);
    if (!theme_path) {
        g_warning("Theme '%s' not found", actual_theme_name);
        return;
    }

    // Parse the theme file and convert to CSS
    GError *error = NULL;
    JsonParser *parser = json_parser_new();

    if (json_parser_load_from_file(parser, theme_path, &error)) {
        JsonNode *root = json_parser_get_root(parser);

        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);

            // Convert the theme to CSS
            char *css_data = convert_vscode_theme_to_css((void*)obj);

            if (css_data && strlen(css_data) > 0) {
                // Create a new CSS provider
                current_provider = gtk_css_provider_new();
                gtk_css_provider_load_from_string(current_provider, css_data);

                // Apply the CSS provider to the default display
                gtk_style_context_add_provider_for_display(
                    gdk_display_get_default(),
                    GTK_STYLE_PROVIDER(current_provider),
                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
                );

                g_free(css_data);
            }
        }
    } else {
        g_warning("Could not parse theme file %s: %s", theme_path, error->message);
        g_error_free(error);
    }

    g_object_unref(parser);
#else
    g_warning("JSON theme support not compiled in - theme '%s' will not be applied", theme_name);
#endif

    // Also update the syntax highlighting theme mode
    gboolean is_dark_theme = FALSE;
    if (g_strstr_len(theme_name, -1, "Dark") || g_strstr_len(theme_name, -1, "dark")) {
        is_dark_theme = TRUE;
    }
    syntax_set_theme_mode(is_dark_theme);
}