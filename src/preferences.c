#include "preferences.h"
#include <adwaita.h>
#include <glib/gi18n.h>
#include "theme-manager.h"

/* Forward declaration */
static void on_save_button_visibility_toggled(GObject *object, GParamSpec *pspec, gpointer user_data);

static GtkWidget*
create_spin_row(const char *title, EditorWidget *editor, const char *property_name, int min, int max, int step)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);

    GtkWidget *spin = gtk_spin_button_new_with_range(min, max, step);
    gtk_widget_set_valign(spin, GTK_ALIGN_CENTER);

    g_object_bind_property(editor, property_name, spin, "value", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

    adw_action_row_add_suffix(ADW_ACTION_ROW(row), spin);
    return row;
}

static void on_save_button_visibility_toggled(GObject *object, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    AdwSwitchRow *switch_row = ADW_SWITCH_ROW(object);
    EditorWidget *editor = EDITOR_WIDGET(user_data);
    gboolean visible = adw_switch_row_get_active(switch_row);
    
    /* Get the parent window to update the save button */
    GtkWidget *parent = GTK_WIDGET(editor);
    GtkWindow *window = NULL;
    
    /* Traverse up the widget hierarchy to find the window */
    while (parent) {
        if (GTK_IS_WINDOW(parent)) {
            window = GTK_WINDOW(parent);
            break;
        }
        parent = gtk_widget_get_parent(parent);
    }
    
    if (window) {
        update_save_button_visibility_from_preferences(window, visible);
    }
}

static GtkWidget*
create_switch_row(const char *title, EditorWidget *editor, const char *property_name)
{
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    g_object_bind_property(editor, property_name, row, "active", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
    return row;
}

static void
on_font_chosen(GtkFontDialog *dialog, GAsyncResult *result, EditorWidget *editor)
{
    GError *error = NULL;
    PangoFontDescription *desc = gtk_font_dialog_choose_font_finish(dialog, result, &error);
    if (desc) {
        char *font_name = pango_font_description_to_string(desc);
        g_object_set(editor, "font-name", font_name, NULL);
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
    GtkWidget *expander = adw_expander_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(expander), _("Custom Font"));
    adw_expander_row_set_show_enable_switch(ADW_EXPANDER_ROW(expander), TRUE);
    g_object_bind_property(editor, "use-custom-font", expander, "enable-expansion", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
    
    /* Inner row for font selection */
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Font Name"));
    
    GtkWidget *btn = gtk_button_new_with_label(_("Select..."));
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_font_button_clicked), editor);
    
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), btn);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(expander), row);
    
    /* Bind subtitle or label to display current font? 
       Let's bind row subtitle to property */
    g_object_bind_property(editor, "font-name", row, "subtitle", G_BINDING_SYNC_CREATE);
    
    return expander;
}

static GtkWidget*
create_indent_style_row(EditorWidget *editor)
{
    GtkWidget *row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), _("Indentation"));
    
    const char *items[] = { "Space", "Tab", NULL };
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(gtk_string_list_new(items)));
    
    /* Bind selected index to indent-style property */
    g_object_bind_property(editor, "indent-style", row, "selected", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
    
    return row;
}

static void
on_theme_changed(GObject *combo, GParamSpec *pspec G_GNUC_UNUSED, gpointer user_data)
{
    EditorWidget *editor = EDITOR_WIDGET(user_data);
    guint idx = adw_combo_row_get_selected(ADW_COMBO_ROW(combo));
    const char *name = theme_manager_get_name((int)idx);
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
    g_signal_connect(theme_row, "notify::selected", G_CALLBACK(on_theme_changed), editor);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_theme), theme_row);
    
    /* Group: Display */
    GtkWidget *group_display = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_display), _("Display"));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group_display));
    
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_display), create_switch_row(_("Display Line Numbers"), editor, "show-line-numbers"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_display), create_switch_row(_("Enable Code Folding"), editor, "enable-folding"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_display), create_switch_row(_("Show Overview Map"), editor, "minimap-enabled"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_display), create_switch_row(_("Highlight Current Line"), editor, "highlight-current-line"));
    
    /* Add a new switch for save button visibility */
    GtkWidget *save_btn_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(save_btn_row), _("Show Save Button"));
    g_object_set_data(G_OBJECT(save_btn_row), "editor", editor); /* Store editor reference for later use */
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
    
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_wrap), create_switch_row(_("Display Right Margin"), editor, "show-right-margin"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_wrap), create_spin_row(_("Right Margin Position"), editor, "right-margin-position", 1, 200, 1));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_wrap), create_switch_row(_("Text Wrapping"), editor, "wrap-lines"));
    
    /* Group: Indentation */
    GtkWidget *group_indent = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_indent), _("Indentation"));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group_indent));
    
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_indent), create_switch_row(_("Automatic Indentation"), editor, "auto-indent"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_indent), create_indent_style_row(editor));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_indent), create_spin_row(_("Tab Width"), editor, "tab-width", 1, 16, 1));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_indent), create_spin_row(_("Indent Width"), editor, "indent-width", 1, 16, 1));

    g_signal_connect_swapped(dialog, "closed", G_CALLBACK(gtk_widget_grab_focus), editor);
    adw_dialog_present(dialog, GTK_WIDGET(parent));
}
