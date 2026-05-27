#include "settings.h"
#include "editor-widget.h"
#include <gtk/gtk.h>
#include <glib.h>
#include <glib/gstdio.h>

static ViteSettings global_settings;
static gboolean is_initialized = FALSE;

static char *
get_settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "vite", "settings.conf", NULL);
}

void
settings_init(void)
{
    if (is_initialized) return;

    /* Set defaults */
    global_settings.show_line_numbers = TRUE;
    global_settings.enable_folding = FALSE;
    global_settings.minimap_enabled = FALSE;
    global_settings.highlight_current_line = TRUE;
    global_settings.show_save_button = FALSE;
    global_settings.font_name = g_strdup("Monospace 11");
    global_settings.use_custom_font = FALSE;
    global_settings.show_right_margin = FALSE;
    global_settings.right_margin_position = 80;
    global_settings.wrap_lines = TRUE;
    global_settings.auto_indent = TRUE;
    global_settings.indent_style = 0; /* Space */
    global_settings.tab_width = 4;
    global_settings.indent_width = 4;

    /* Load from file if exists */
    char *path = get_settings_path();
    GKeyFile *kf = g_key_file_new();
    GError *error = NULL;

    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &error)) {
        if (g_key_file_has_key(kf, "Display", "show_line_numbers", NULL))
            global_settings.show_line_numbers = g_key_file_get_boolean(kf, "Display", "show_line_numbers", NULL);
        if (g_key_file_has_key(kf, "Display", "enable_folding", NULL))
            global_settings.enable_folding = g_key_file_get_boolean(kf, "Display", "enable_folding", NULL);
        if (g_key_file_has_key(kf, "Display", "minimap_enabled", NULL))
            global_settings.minimap_enabled = g_key_file_get_boolean(kf, "Display", "minimap_enabled", NULL);
        if (g_key_file_has_key(kf, "Display", "highlight_current_line", NULL))
            global_settings.highlight_current_line = g_key_file_get_boolean(kf, "Display", "highlight_current_line", NULL);
        if (g_key_file_has_key(kf, "Display", "show_save_button", NULL))
            global_settings.show_save_button = g_key_file_get_boolean(kf, "Display", "show_save_button", NULL);

        if (g_key_file_has_key(kf, "Typography", "use_custom_font", NULL))
            global_settings.use_custom_font = g_key_file_get_boolean(kf, "Typography", "use_custom_font", NULL);
        if (g_key_file_has_key(kf, "Typography", "font_name", NULL)) {
            g_free(global_settings.font_name);
            global_settings.font_name = g_key_file_get_string(kf, "Typography", "font_name", NULL);
        }

        if (g_key_file_has_key(kf, "LineWrap", "show_right_margin", NULL))
            global_settings.show_right_margin = g_key_file_get_boolean(kf, "LineWrap", "show_right_margin", NULL);
        if (g_key_file_has_key(kf, "LineWrap", "right_margin_position", NULL))
            global_settings.right_margin_position = g_key_file_get_integer(kf, "LineWrap", "right_margin_position", NULL);
        if (g_key_file_has_key(kf, "LineWrap", "wrap_lines", NULL))
            global_settings.wrap_lines = g_key_file_get_boolean(kf, "LineWrap", "wrap_lines", NULL);

        if (g_key_file_has_key(kf, "Indentation", "auto_indent", NULL))
            global_settings.auto_indent = g_key_file_get_boolean(kf, "Indentation", "auto_indent", NULL);
        if (g_key_file_has_key(kf, "Indentation", "indent_style", NULL))
            global_settings.indent_style = g_key_file_get_integer(kf, "Indentation", "indent_style", NULL);
        if (g_key_file_has_key(kf, "Indentation", "tab_width", NULL))
            global_settings.tab_width = g_key_file_get_integer(kf, "Indentation", "tab_width", NULL);
        if (g_key_file_has_key(kf, "Indentation", "indent_width", NULL))
            global_settings.indent_width = g_key_file_get_integer(kf, "Indentation", "indent_width", NULL);

    } else {
        if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_warning("Could not load settings file: %s", error->message);
        }
        g_clear_error(&error);
    }

    g_key_file_free(kf);
    g_free(path);
    is_initialized = TRUE;
}

void
settings_cleanup(void)
{
    if (!is_initialized) return;
    g_free(global_settings.font_name);
    is_initialized = FALSE;
}

ViteSettings *
settings_get(void)
{
    if (!is_initialized) {
        settings_init();
    }
    return &global_settings;
}

void
settings_save(void)
{
    char *config_dir = g_build_filename(g_get_user_config_dir(), "vite", NULL);
    g_mkdir_with_parents(config_dir, 0755);
    g_free(config_dir);

    char *path = get_settings_path();
    GKeyFile *kf = g_key_file_new();

    g_key_file_set_boolean(kf, "Display", "show_line_numbers", global_settings.show_line_numbers);
    g_key_file_set_boolean(kf, "Display", "enable_folding", global_settings.enable_folding);
    g_key_file_set_boolean(kf, "Display", "minimap_enabled", global_settings.minimap_enabled);
    g_key_file_set_boolean(kf, "Display", "highlight_current_line", global_settings.highlight_current_line);
    g_key_file_set_boolean(kf, "Display", "show_save_button", global_settings.show_save_button);

    g_key_file_set_boolean(kf, "Typography", "use_custom_font", global_settings.use_custom_font);
    g_key_file_set_string(kf, "Typography", "font_name", global_settings.font_name ? global_settings.font_name : "Monospace 11");

    g_key_file_set_boolean(kf, "LineWrap", "show_right_margin", global_settings.show_right_margin);
    g_key_file_set_integer(kf, "LineWrap", "right_margin_position", global_settings.right_margin_position);
    g_key_file_set_boolean(kf, "LineWrap", "wrap_lines", global_settings.wrap_lines);

    g_key_file_set_boolean(kf, "Indentation", "auto_indent", global_settings.auto_indent);
    g_key_file_set_integer(kf, "Indentation", "indent_style", global_settings.indent_style);
    g_key_file_set_integer(kf, "Indentation", "tab_width", global_settings.tab_width);
    g_key_file_set_integer(kf, "Indentation", "indent_width", global_settings.indent_width);

    GError *error = NULL;
    if (!g_key_file_save_to_file(kf, path, &error)) {
        g_warning("Could not save settings: %s", error->message);
        g_error_free(error);
    }

    g_key_file_free(kf);
    g_free(path);
}

static void
update_editor_properties_recursive(GtkWidget *widget)
{
    if (!widget) return;
    
    if (EDITOR_IS_WIDGET(widget)) {
        EditorWidget *ed = EDITOR_WIDGET(widget);
        g_object_set(G_OBJECT(ed),
            "show-line-numbers", global_settings.show_line_numbers,
            "enable-folding", global_settings.enable_folding,
            "minimap-enabled", global_settings.minimap_enabled,
            "highlight-current-line", global_settings.highlight_current_line,
            "font-name", global_settings.font_name,
            "use-custom-font", global_settings.use_custom_font,
            "show-right-margin", global_settings.show_right_margin,
            "right-margin-position", global_settings.right_margin_position,
            "wrap-lines", global_settings.wrap_lines,
            "auto-indent", global_settings.auto_indent,
            "indent-style", global_settings.indent_style,
            "tab-width", global_settings.tab_width,
            "indent-width", global_settings.indent_width,
            NULL);
        return;
    }

    GtkWidget *child = gtk_widget_get_first_child(widget);
    while (child) {
        update_editor_properties_recursive(child);
        child = gtk_widget_get_next_sibling(child);
    }
}

void
settings_apply_to_all_editors(void)
{
    GListModel *toplevels = gtk_window_get_toplevels();
    guint n_windows = g_list_model_get_n_items(toplevels);
    for (guint i = 0; i < n_windows; i++) {
        GObject *win = g_list_model_get_item(toplevels, i);
        if (GTK_IS_WIDGET(win)) {
            update_editor_properties_recursive(GTK_WIDGET(win));
            /* A hook for window properties can be called here if needed */
        }
        g_object_unref(win);
    }
}
