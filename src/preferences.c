#include "preferences.h"
#include <adwaita.h>

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
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(expander), "Custom Font");
    adw_expander_row_set_show_enable_switch(ADW_EXPANDER_ROW(expander), TRUE);
    g_object_bind_property(editor, "use-custom-font", expander, "enable-expansion", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
    
    /* Inner row for font selection */
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Font Name");
    
    GtkWidget *btn = gtk_button_new_with_label("Select...");
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
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Indentation Character");
    
    const char *items[] = { "Space", "Tab", NULL };
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(gtk_string_list_new(items)));
    
    /* Bind selected index to indent-style property */
    g_object_bind_property(editor, "indent-style", row, "selected", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
    
    return row;
}

void show_preferences_dialog(GtkWindow *parent, EditorWidget *editor)
{
    AdwDialog *dialog = adw_preferences_dialog_new();
    
    GtkWidget *page = adw_preferences_page_new();
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(page));
    
    /* Group: Display */
    GtkWidget *group_display = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_display), "Display");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group_display));
    
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_display), create_switch_row("Display Line Numbers", editor, "show-line-numbers"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_display), create_switch_row("Enable Code Folding", editor, "enable-folding"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_display), create_switch_row("Highlight Current Line", editor, "highlight-current-line"));
    
    /* Group: Typography/Font */
    GtkWidget *group_font = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_font), "Typography");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group_font));
    
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_font), create_font_expander(editor));
    
    /* Group: Line Wrap */
    GtkWidget *group_wrap = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_wrap), "Line Wrap");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group_wrap));
    
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_wrap), create_switch_row("Show Right Margin", editor, "show-right-margin"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_wrap), create_spin_row("Margin Position", editor, "right-margin-position", 1, 200, 1));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_wrap), create_switch_row("Wrap Lines Automatically", editor, "wrap-lines"));
    
    /* Group: Indentation */
    GtkWidget *group_indent = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group_indent), "Indentation");
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group_indent));
    
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_indent), create_switch_row("Auto Indentation", editor, "auto-indent"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_indent), create_indent_style_row(editor));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_indent), create_spin_row("Spaces Per Tab", editor, "tab-width", 1, 16, 1));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group_indent), create_spin_row("Spaces Per Indent", editor, "indent-width", 1, 16, 1));

    g_signal_connect_swapped(dialog, "closed", G_CALLBACK(gtk_widget_grab_focus), editor);
    adw_dialog_present(dialog, GTK_WIDGET(parent));
}
