#include "preferences.h"
#include <adwaita.h>
#include <glib/gi18n.h>
#include "theme-manager.h"
#include "settings.h"

/* Forward declaration */
static void on_save_button_visibility_toggled(GObject *object, GParamSpec *pspec, gpointer user_data);

static void on_spin_changed(GtkSpinButton *spin, gpointer user_data)
{
    const char *key = (const char *)user_data;
    int value = gtk_spin_button_get_value_as_int(spin);
    ViteSettings *settings = settings_get();
    
    if (g_strcmp0(key, "right-margin-position") == 0) settings->right_margin_position = value;
    else if (g_strcmp0(key, "tab-width") == 0) settings->tab_width = value;
    else if (g_strcmp0(key, "indent-width") == 0) settings->indent_width = value;
    
    settings_save();
    settings_apply_to_all_editors();
}

static GtkWidget*
create_spin_row(const char *title, const char *key, int min, int max, int step)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);

    GtkWidget *spin = gtk_spin_button_new_with_range(min, max, step);
    gtk_widget_set_valign(spin, GTK_ALIGN_CENTER);

    ViteSettings *settings = settings_get();
    int val = 0;
    if (g_strcmp0(key, "right-margin-position") == 0) val = settings->right_margin_position;
    else if (g_strcmp0(key, "tab-width") == 0) val = settings->tab_width;
    else if (g_strcmp0(key, "indent-width") == 0) val = settings->indent_width;
    
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), val);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_spin_changed), (gpointer)key);

    adw_action_row_add_suffix(ADW_ACTION_ROW(row), spin);
    return row;
}

static void on_save_button_visibility_toggled(GObject *object, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    AdwSwitchRow *switch_row = ADW_SWITCH_ROW(object);
    gboolean visible = adw_switch_row_get_active(switch_row);
    
    ViteSettings *settings = settings_get();
    settings->show_save_button = visible;
    settings_save();
    
    /* Update save button in all windows */
    GListModel *toplevels = gtk_window_get_toplevels();
    guint n_windows = g_list_model_get_n_items(toplevels);
    for (guint i = 0; i < n_windows; i++) {
        GObject *win = g_list_model_get_item(toplevels, i);
        if (GTK_IS_WINDOW(win)) {
            update_save_button_visibility_from_preferences(GTK_WINDOW(win), visible);
        }
        g_object_unref(win);
    }
}

static void on_switch_toggled(GObject *object, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    const char *key = (const char *)user_data;
    gboolean active = adw_switch_row_get_active(ADW_SWITCH_ROW(object));
    ViteSettings *settings = settings_get();
    
    if (g_strcmp0(key, "show-line-numbers") == 0) settings->show_line_numbers = active;
    else if (g_strcmp0(key, "enable-folding") == 0) settings->enable_folding = active;
    else if (g_strcmp0(key, "minimap-enabled") == 0) settings->minimap_enabled = active;
    else if (g_strcmp0(key, "highlight-current-line") == 0) settings->highlight_current_line = active;
    else if (g_strcmp0(key, "show-right-margin") == 0) settings->show_right_margin = active;
    else if (g_strcmp0(key, "wrap-lines") == 0) settings->wrap_lines = active;
    else if (g_strcmp0(key, "auto-indent") == 0) settings->auto_indent = active;
    else if (g_strcmp0(key, "use-custom-font") == 0) settings->use_custom_font = active;
    
    settings_save();
    settings_apply_to_all_editors();
}

static GtkWidget*
create_switch_row(const char *title, const char *key)
{
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    
    ViteSettings *settings = settings_get();
    gboolean active = FALSE;
    if (g_strcmp0(key, "show-line-numbers") == 0) active = settings->show_line_numbers;
    else if (g_strcmp0(key, "enable-folding") == 0) active = settings->enable_folding;
    else if (g_strcmp0(key, "minimap-enabled") == 0) active = settings->minimap_enabled;
    else if (g_strcmp0(key, "highlight-current-line") == 0) active = settings->highlight_current_line;
    else if (g_strcmp0(key, "show-right-margin") == 0) active = settings->show_right_margin;
    else if (g_strcmp0(key, "wrap-lines") == 0) active = settings->wrap_lines;
    else if (g_strcmp0(key, "auto-indent") == 0) active = settings->auto_indent;
    
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), active);
    g_signal_connect(row, "notify::active", G_CALLBACK(on_switch_toggled), (gpointer)key);
    return row;
}

static void
on_font_chosen(GtkFontDialog *dialog, GAsyncResult *result, EditorWidget *editor G_GNUC_UNUSED)
{
    GError *error = NULL;
    PangoFontDescription *desc = gtk_font_dialog_choose_font_finish(dialog, result, &error);
    if (desc) {
        char *font_name = pango_font_description_to_string(desc);
        ViteSettings *settings = settings_get();
        g_free(settings->font_name);
        settings->font_name = g_strdup(font_name);
        settings_save();
        settings_apply_to_all_editors();
        
        GtkWidget *row = GTK_WIDGET(g_object_get_data(G_OBJECT(dialog), "font_row"));
        if (row) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), font_name);
        
        g_free(font_name);
        pango_font_description_free(desc);
    } else {
        if (error) {
            g_warning("Font selection failed: %s", error->message);
            g_error_free(error);
        }
    }
}

static void
on_font_button_clicked(GtkButton *btn, EditorWidget *editor)
{
    /* Use GtkFontDialog (GTK 4.10+) */
    GtkFontDialog *dialog = gtk_font_dialog_new();
    
    /* Get current font to set as initial */
    char *current_font = NULL;
    g_object_get(editor, "font-name", &current_font, NULL);
    PangoFontDescription *desc = NULL;
    if (current_font) {
        desc = pango_font_description_from_string(current_font);
        g_free(current_font);
    }
    
    gtk_font_dialog_choose_font(dialog, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn))), desc, NULL, (GAsyncReadyCallback)on_font_chosen, editor);
    
    if (desc) pango_font_description_free(desc);
    g_object_unref(dialog);
}

static GtkWidget*
create_font_expander(EditorWidget *editor)
{
    ViteSettings *settings = settings_get();
    GtkWidget *expander = adw_expander_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(expander), _("Custom Font"));
    adw_expander_row_set_show_enable_switch(ADW_EXPANDER_ROW(expander), TRUE);
    adw_expander_row_set_enable_expansion(ADW_EXPANDER_ROW(expander), settings->use_custom_font);
    g_signal_connect(expander, "notify::enable-expansion", G_CALLBACK(on_switch_toggled), "use-custom-font");
    
    /* Inner row for font selection */
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Font Name"));
    
    GtkWidget *btn = gtk_button_new_with_label(_("Select..."));
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_font_button_clicked), editor);
    
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), btn);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), row);
    
    g_object_set_data(G_OBJECT(btn), "font_row", row);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), settings->font_name);
    
    return expander;
}

static void on_indent_style_changed(GObject *combo, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    ViteSettings *settings = settings_get();
    settings->indent_style = adw_combo_row_get_selected(ADW_COMBO_ROW(combo));
    settings_save();
    settings_apply_to_all_editors();
}

static GtkWidget*
create_indent_style_row(void)
{
    GtkWidget *row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Indentation"));
    
    const char *items[] = { "Space", "Tab", NULL };
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(gtk_string_list_new(items)));
    
    ViteSettings *settings = settings_get();
    adw_combo_row_set_selected(ADW_COMBO_ROW(row), settings->indent_style);
    g_signal_connect(row, "notify::selected", G_CALLBACK(on_indent_style_changed), NULL);
    
    return row;
}

static void
on_theme_changed(GObject *combo, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    EditorWidget *editor = EDITOR_WIDGET(user_data);
    gpointer item = adw_combo_row_get_selected_item(ADW_COMBO_ROW(combo));
    if (!item) return;
    
    const char *name = gtk_string_object_get_string(GTK_STRING_OBJECT(item));
    if (!name) return;
    
    theme_manager_apply_theme(name);
    theme_manager_save_selection(name);
    
    /* Invalidate syntax cache on the current editor */
    SyntaxContext *ctx = editor_widget_get_syntax_context(editor);
    if (ctx) syntax_context_invalidate_cache(ctx);
    gtk_widget_queue_draw(GTK_WIDGET(editor));

    /* Also invalidate all other editors across all tabs */
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(editor));
    if (root) {
        /* Queue a redraw of the entire window so all widgets pick up new colors */
        gtk_widget_queue_draw(GTK_WIDGET(root));
    }
}

void show_preferences_dialog(GtkWindow *parent, EditorWidget *editor)
{
    AdwDialog *dialog = adw_preferences_dialog_new();
    gtk_widget_add_css_class(GTK_WIDGET(dialog), "vite-preferences-dialog");
    
    GtkWidget *page = adw_preferences_page_new();
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(page));
    
    /* Group: Theme */
    GtkWidget *group_theme = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_theme), _("Theme"));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group_theme));
    
    GtkWidget *theme_row = adw_combo_row_new();
    gtk_widget_add_css_class(theme_row, "vite-theme-combo");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(theme_row), _("Color Theme"));
    
    int count = theme_manager_get_count();
    GtkStringList *theme_list = gtk_string_list_new(NULL);
    const ViteTheme *current = theme_manager_get_current();
    guint active_idx = 0;
    for (int i = 0; i < count; i++) {
        const char *n = theme_manager_get_name(i);
        gtk_string_list_append(theme_list, n);
        if (current && g_strcmp0(n, current->name) == 0)
            active_idx = (guint)i;
    }
    adw_combo_row_set_model(ADW_COMBO_ROW(theme_row), G_LIST_MODEL(theme_list));
    adw_combo_row_set_selected(ADW_COMBO_ROW(theme_row), active_idx);
    
    GtkExpression *expr = gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string");
    adw_combo_row_set_expression(ADW_COMBO_ROW(theme_row), expr);
    adw_combo_row_set_enable_search(ADW_COMBO_ROW(theme_row), TRUE);
    adw_combo_row_set_search_match_mode(ADW_COMBO_ROW(theme_row), GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
    g_signal_connect(theme_row, "notify::selected", G_CALLBACK(on_theme_changed), editor);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_theme), theme_row);
    
    /* Group: Display */
    GtkWidget *group_display = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_display), _("Display"));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group_display));
    
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_display), create_switch_row(_("Display Line Numbers"), "show-line-numbers"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_display), create_switch_row(_("Enable Code Folding"), "enable-folding"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_display), create_switch_row(_("Show Overview Map"), "minimap-enabled"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_display), create_switch_row(_("Highlight Current Line"), "highlight-current-line"));
    
    /* Add a new switch for save button visibility */
    GtkWidget *save_btn_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(save_btn_row), _("Show Save Button"));
    adw_switch_row_set_active(ADW_SWITCH_ROW(save_btn_row), settings_get()->show_save_button);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_display), save_btn_row);
    
    /* Connect the switch to update the save button visibility */
    g_signal_connect(save_btn_row, "notify::active", G_CALLBACK(on_save_button_visibility_toggled), editor);
    
    /* Group: Typography/Font */
    GtkWidget *group_font = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_font), _("Typography"));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group_font));
    
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_font), create_font_expander(editor));
    
    /* Group: Line Wrap */
    GtkWidget *group_wrap = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_wrap), _("Line Wrap"));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group_wrap));
    
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_wrap), create_switch_row(_("Display Right Margin"), "show-right-margin"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_wrap), create_spin_row(_("Right Margin Position"), "right-margin-position", 1, 200, 1));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_wrap), create_switch_row(_("Text Wrapping"), "wrap-lines"));
    
    /* Group: Indentation */
    GtkWidget *group_indent = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_indent), _("Indentation"));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group_indent));
    
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_indent), create_switch_row(_("Automatic Indentation"), "auto-indent"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_indent), create_indent_style_row());
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_indent), create_spin_row(_("Tab Width"), "tab-width", 1, 16, 1));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_indent), create_spin_row(_("Indent Width"), "indent-width", 1, 16, 1));

    g_signal_connect_swapped(dialog, "closed", G_CALLBACK(gtk_widget_grab_focus), editor);
    adw_dialog_present(dialog, GTK_WIDGET(parent));
}
