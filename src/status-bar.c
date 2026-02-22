#include "status-bar.h"
#include "piece-table.h"
#include <glib/gi18n.h>

struct _ViteStatusBar {
    GtkWidget parent_instance;
    GtkWidget *box;
    
    GtkWidget *file_type_btn;
    GtkWidget *file_type_label;
    
    GtkWidget *tab_width_btn;
    GtkWidget *tab_width_label;
    GSimpleActionGroup *indent_group;
    
    GtkWidget *encoding_btn;
    GtkWidget *encoding_label;
    
    GtkWidget *line_ending_btn;
    GtkWidget *line_ending_label;
    GSimpleActionGroup *line_ending_group;
    
    GtkWidget *cursor_label;
    GtkWidget *ins_label;
};

G_DEFINE_TYPE(ViteStatusBar, vite_status_bar, GTK_TYPE_WIDGET)

enum {
    FILE_TYPE_CHANGED,
    LINE_ENDING_CHANGED,
    ENCODING_CHANGED,
    INDENT_WIDTH_CHANGED,
    INDENT_STYLE_CHANGED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void on_search_changed(GtkEditable *entry, gpointer user_data);
static gboolean file_type_filter_func(GtkListBoxRow *row, gpointer user_data);
static void on_file_type_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
static gboolean encoding_filter_func(GtkListBoxRow *row, gpointer user_data);
static void on_encoding_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);

static void
vite_status_bar_dispose(GObject *object)
{
    ViteStatusBar *self = VITE_STATUS_BAR(object);
    g_clear_pointer(&self->box, gtk_widget_unparent);
    G_OBJECT_CLASS(vite_status_bar_parent_class)->dispose(object);
}

static void
vite_status_bar_class_init(ViteStatusBarClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    
    object_class->dispose = vite_status_bar_dispose;
    
    gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
    gtk_widget_class_set_css_name(widget_class, "statusbar");
    
    signals[FILE_TYPE_CHANGED] = g_signal_new("file-type-changed",
                                 G_TYPE_FROM_CLASS(klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL,
                                 NULL,
                                 G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[LINE_ENDING_CHANGED] = g_signal_new("line-ending-changed",
                                 G_TYPE_FROM_CLASS(klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL,
                                 NULL,
                                 G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[ENCODING_CHANGED] = g_signal_new("encoding-changed",
                                 G_TYPE_FROM_CLASS(klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL,
                                 NULL,
                                 G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[INDENT_WIDTH_CHANGED] = g_signal_new("indent-width-changed",
                                 G_TYPE_FROM_CLASS(klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL,
                                 NULL,
                                 G_TYPE_NONE, 1, G_TYPE_INT);

    signals[INDENT_STYLE_CHANGED] = g_signal_new("indent-style-changed",
                                 G_TYPE_FROM_CLASS(klass),
                                 G_SIGNAL_RUN_LAST,
                                 0,
                                 NULL, NULL,
                                 NULL,
                                 G_TYPE_NONE, 1, G_TYPE_INT);
}

/* ... existing helper functions ... */

static void
on_line_ending_activated(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteStatusBar *self = VITE_STATUS_BAR(user_data);
    g_simple_action_set_state(action, parameter);
    
    const char *id = g_variant_get_string(parameter, NULL);

    
    vite_status_bar_set_line_ending(self, id);
    g_signal_emit(self, signals[LINE_ENDING_CHANGED], 0, id);
}

static void
create_line_ending_menu(ViteStatusBar *self)
{
    GMenu *menu = g_menu_new();
    
    self->line_ending_group = g_simple_action_group_new();
    GActionEntry entries[] = {
        { "set", NULL, "s", "'lf'", on_line_ending_activated, { 0 } }
    };
    g_action_map_add_action_entries(G_ACTION_MAP(self->line_ending_group), entries, G_N_ELEMENTS(entries), self);
    gtk_widget_insert_action_group(self->line_ending_btn, "le", G_ACTION_GROUP(self->line_ending_group));
    
    g_menu_append(menu, _("Unix/Linux (LF)"), "le.set::lf");
    g_menu_append(menu, _("Windows (CRLF)"), "le.set::crlf");
    g_menu_append(menu, _("Legacy Mac (CR)"), "le.set::cr");
    
    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->line_ending_btn), popover);
    g_object_unref(menu);
}

static void
create_encoding_menu(ViteStatusBar *self)
{
    GtkWidget *popover = gtk_popover_new();
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->encoding_btn), popover);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_popover_set_child(GTK_POPOVER(popover), box);

    GtkWidget *entry = gtk_search_entry_new();
    gtk_box_append(GTK_BOX(box), entry);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scrolled), 240);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 240);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(box), scrolled);

    GtkWidget *listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(listbox, "boxed-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), listbox);

    for (int i = 0; i < file_encoding_get_count(); i++) {
        const char *disp = file_encoding_get_display_name_at(i);
        const char *id = file_encoding_get_id_at(i);
        if (!disp || !id) continue;

        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *lbl = gtk_label_new(disp);
        gtk_widget_set_margin_start(lbl, 12);
        gtk_widget_set_margin_end(lbl, 12);
        gtk_widget_set_margin_top(lbl, 8);
        gtk_widget_set_margin_bottom(lbl, 8);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);

        g_object_set_data(G_OBJECT(row), "enc-name", (gpointer)disp);
        g_object_set_data(G_OBJECT(row), "enc-id", (gpointer)id);

        gtk_list_box_append(GTK_LIST_BOX(listbox), row);
    }

    gtk_list_box_set_filter_func(GTK_LIST_BOX(listbox), encoding_filter_func, entry, NULL);
    g_signal_connect(entry, "search-changed", G_CALLBACK(on_search_changed), listbox);
    g_signal_connect(listbox, "row-activated", G_CALLBACK(on_encoding_row_activated), self);
}

static GtkWidget *
create_separator(void)
{
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_margin_top(sep, 4);
    gtk_widget_set_margin_bottom(sep, 4);
    gtk_widget_set_margin_start(sep, 4);
    gtk_widget_set_margin_end(sep, 4);
    return sep;
}

static GtkWidget *
create_menu_button(ViteStatusBar *self G_GNUC_UNUSED, GtkWidget **label_out, const char *tooltip, const char *default_text)
{
    GtkWidget *btn = gtk_menu_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_widget_set_tooltip_text(btn, tooltip);
    
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), default_text);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(btn), label);
    
    if (label_out) *label_out = label;
    
    return btn;
}

/* ... existing file type stuff ... */

typedef struct {
    const char *name;
    const char *id;
} FileTypeEntry;

static const FileTypeEntry file_types[] = {
    { N_("Plain Text"), NULL },
    { "C", "c" },
    { "C++", "cpp" },
    { "Python", "python" },
    { "Bash", "bash" },
    { "Rust", "rust" },
    { "Header", "h" },
    { "YAML", "yaml" },
    { "JSON", "json" },
    { "XML", "xml" },
    { "JavaScript", "javascript" },
    { "Desktop Entry", "desktop" },
    { NULL, NULL }
};

static void
on_file_type_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ViteStatusBar *self = VITE_STATUS_BAR(user_data);
    const char *id = g_object_get_data(G_OBJECT(row), "lang-id");
    const char *name = g_object_get_data(G_OBJECT(row), "lang-name");
    
    /* Update label */
    /* Update label */
    if (id == NULL) {
        vite_status_bar_set_file_type(self, NULL);
    } else {
        vite_status_bar_set_file_type(self, _(name));
    }
    
    /* Close popover first to release focus */
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(box), GTK_TYPE_POPOVER);
    if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
    
    /* Emit signal to trigger focus grab in main.c */
    g_signal_emit(self, signals[FILE_TYPE_CHANGED], 0, id);
}

static gboolean
file_type_filter_func(GtkListBoxRow *row, gpointer user_data)
{
    GtkEditable *entry = GTK_EDITABLE(user_data);
    const char *text = gtk_editable_get_text(entry);
    if (!text || !text[0]) return TRUE;
    
    const char *name = g_object_get_data(G_OBJECT(row), "lang-name");
    if (!name) return FALSE;
    
    /* Case insensitive search */
    char *name_lower = g_utf8_strdown(name, -1);
    char *text_lower = g_utf8_strdown(text, -1);
    gboolean match = (strstr(name_lower, text_lower) != NULL);
    g_free(name_lower);
    g_free(text_lower);
    
    return match;
}

static gboolean
encoding_filter_func(GtkListBoxRow *row, gpointer user_data)
{
    GtkEditable *entry = GTK_EDITABLE(user_data);
    const char *text = gtk_editable_get_text(entry);
    if (!text || !text[0]) return TRUE;

    const char *name = g_object_get_data(G_OBJECT(row), "enc-name");
    const char *id = g_object_get_data(G_OBJECT(row), "enc-id");
    if (!name && !id) return FALSE;

    char *name_lower = name ? g_utf8_strdown(name, -1) : NULL;
    char *id_lower = id ? g_utf8_strdown(id, -1) : NULL;
    char *text_lower = g_utf8_strdown(text, -1);

    gboolean match = FALSE;
    if (name_lower && strstr(name_lower, text_lower)) match = TRUE;
    if (!match && id_lower && strstr(id_lower, text_lower)) match = TRUE;

    g_free(name_lower);
    g_free(id_lower);
    g_free(text_lower);
    return match;
}

static void
on_encoding_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ViteStatusBar *self = VITE_STATUS_BAR(user_data);
    const char *id = g_object_get_data(G_OBJECT(row), "enc-id");
    if (!id) return;

    vite_status_bar_set_encoding(self, id);

    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(box), GTK_TYPE_POPOVER);
    if (popover) gtk_popover_popdown(GTK_POPOVER(popover));

    g_signal_emit(self, signals[ENCODING_CHANGED], 0, id);
}

static void
on_indent_width_chk(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteStatusBar *self = VITE_STATUS_BAR(user_data);
    int width = g_variant_get_int32(parameter);
    
    /* Change state */
    g_simple_action_set_state(action, parameter);
    
    /* Emit signal */
    g_signal_emit(self, signals[INDENT_WIDTH_CHANGED], 0, width);
}

static void
on_indent_style_chk(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ViteStatusBar *self = VITE_STATUS_BAR(user_data);
    int style = g_variant_get_int32(parameter); /* 0=Spaces, 1=Tabs */
    
    /* Change state */
    g_simple_action_set_state(action, parameter);
    
    /* Emit signal */
    g_signal_emit(self, signals[INDENT_STYLE_CHANGED], 0, style);
}

static void
create_indent_menu(ViteStatusBar *self)
{
    GMenu *menu = g_menu_new();
    self->indent_group = g_simple_action_group_new();
    
    /* Width Action (Stateful INT) */
    GActionEntry entries[] = {
        { "set-width", NULL, "i", "4", on_indent_width_chk, { 0 } },
        { "set-style", NULL, "i", "1", on_indent_style_chk, { 0 } }
    };
    g_action_map_add_action_entries(G_ACTION_MAP(self->indent_group), entries, G_N_ELEMENTS(entries), self);
    gtk_widget_insert_action_group(self->tab_width_btn, "indent", G_ACTION_GROUP(self->indent_group));
    
    /* Indent Type Section */
    GMenu *s1 = g_menu_new();
    g_menu_append(s1, _("Tabs"), "indent.set-style(1)");
    g_menu_append(s1, _("Spaces"), "indent.set-style(0)");
    g_menu_append_section(menu, _("Indentation Mode"), G_MENU_MODEL(s1));
    g_object_unref(s1);
    
    /* Width Section */
    GMenu *s2 = g_menu_new();
    g_menu_append(s2, "2", "indent.set-width(2)");
    g_menu_append(s2, "4", "indent.set-width(4)");
    g_menu_append(s2, "8", "indent.set-width(8)");
    g_menu_append_section(menu, _("Tab Width"), G_MENU_MODEL(s2));
    g_object_unref(s2);
    
    GtkWidget *popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->tab_width_btn), popover);
    g_object_unref(menu);
}

static void
on_search_changed(GtkEditable *entry G_GNUC_UNUSED, gpointer user_data)
{
    GtkListBox *listbox = GTK_LIST_BOX(user_data);
    gtk_list_box_invalidate_filter(listbox);
}

static void
create_file_type_menu(ViteStatusBar *self)
{
    GtkWidget *popover = gtk_popover_new();
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->file_type_btn), popover);
    
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_popover_set_child(GTK_POPOVER(popover), box);
    
    /* Search Entry */
    GtkWidget *entry = gtk_search_entry_new();
    gtk_box_append(GTK_BOX(box), entry);
    
    /* List Box in Scrolled Window */
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scrolled), 200);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 200);
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(box), scrolled);
    
    GtkWidget *listbox = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(listbox), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(listbox, "boxed-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), listbox);
    
    for (int i = 0; file_types[i].name != NULL; i++) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *lbl = gtk_label_new(_(file_types[i].name));
        gtk_widget_set_margin_start(lbl, 12);
        gtk_widget_set_margin_end(lbl, 12);
        gtk_widget_set_margin_top(lbl, 8);
        gtk_widget_set_margin_bottom(lbl, 8);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
        
        g_object_set_data(G_OBJECT(row), "lang-name", (gpointer)file_types[i].name);
        g_object_set_data(G_OBJECT(row), "lang-id", (gpointer)file_types[i].id);
        
        gtk_list_box_append(GTK_LIST_BOX(listbox), row);
    }
    
    g_signal_connect(listbox, "row-activated", G_CALLBACK(on_file_type_row_activated), self);
    
    gtk_list_box_set_filter_func(GTK_LIST_BOX(listbox), file_type_filter_func, entry, NULL);
    g_signal_connect(entry, "search-changed", G_CALLBACK(on_search_changed), listbox);
}

static void
vite_status_bar_init(ViteStatusBar *self)
{
    self->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_parent(self->box, GTK_WIDGET(self));
    
    gtk_widget_add_css_class(GTK_WIDGET(self), "status-bar");
    
    /* File Type */
    self->file_type_btn = create_menu_button(self, &self->file_type_label, _("File Type"), _("Plain Text"));
    create_file_type_menu(self); /* Setup Popover */
    gtk_box_append(GTK_BOX(self->box), self->file_type_btn);
    gtk_box_append(GTK_BOX(self->box), create_separator());
    
    /* Indentation (Tab Width) */
    self->tab_width_btn = create_menu_button(self, &self->tab_width_label, _("Indentation"), _("Tab: 4"));
    create_indent_menu(self);
    gtk_box_append(GTK_BOX(self->box), self->tab_width_btn);
    gtk_box_append(GTK_BOX(self->box), create_separator());

    /* Encoding */
    self->encoding_btn = create_menu_button(self, &self->encoding_label, _("Encoding"), "UTF-8");
    create_encoding_menu(self);
    gtk_box_append(GTK_BOX(self->box), self->encoding_btn);
    gtk_box_append(GTK_BOX(self->box), create_separator());

    /* Line Ending */
    /* Line Ending */
    self->line_ending_btn = create_menu_button(self, &self->line_ending_label, _("Line Ending"), "LF");
    create_line_ending_menu(self);
    gtk_box_append(GTK_BOX(self->box), self->line_ending_btn);
    
    /* Spacer */
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(self->box), spacer);
    
    /* Cursor Position */
    self->cursor_label = gtk_label_new("Ln 1, Col 1");
    gtk_widget_set_margin_start(self->cursor_label, 8);
    gtk_widget_set_margin_end(self->cursor_label, 8);
    gtk_box_append(GTK_BOX(self->box), self->cursor_label);
    
    gtk_box_append(GTK_BOX(self->box), create_separator());
    
    /* INS/OVR Button */
    self->ins_label = gtk_label_new("INS");
    /* Make it a button */
    GtkWidget *btn = gtk_button_new();
    gtk_widget_add_css_class(btn, "flat");
    gtk_button_set_child(GTK_BUTTON(btn), self->ins_label);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(btn), "win.toggle-insert-mode");
    
    gtk_widget_set_margin_start(btn, 4);
    gtk_widget_set_margin_end(btn, 4);
    
    gtk_box_append(GTK_BOX(self->box), btn);
}

GtkWidget *
vite_status_bar_new(void)
{
    return g_object_new(VITE_TYPE_STATUS_BAR, NULL);
}

void
vite_status_bar_set_insert_mode(ViteStatusBar *self, gboolean insert)
{
    g_return_if_fail(VITE_IS_STATUS_BAR(self));
    if (insert) {
        gtk_label_set_markup(GTK_LABEL(self->ins_label), _("<span font_weight='normal'>INS</span>"));
    } else {
        gtk_label_set_markup(GTK_LABEL(self->ins_label), _("<span font_weight='bold'>OVR</span>"));
    }
}

void
vite_status_bar_set_cursor_position(ViteStatusBar *self, int line, int col)
{
    g_return_if_fail(VITE_IS_STATUS_BAR(self));
    char *text = g_strdup_printf(_("Ln %d, Col %d"), line + 1, col + 1);
    gtk_label_set_text(GTK_LABEL(self->cursor_label), text);
    g_free(text);
}

void
vite_status_bar_set_file_type(ViteStatusBar *self, const char *file_type)
{
    g_return_if_fail(VITE_IS_STATUS_BAR(self));
    if (!file_type) file_type = _("Plain Text");
    /* Bold if not plain text? svite does this. */
    char *markup = g_strdup_printf("<span font_weight='normal'>%s</span>", file_type);
    gtk_label_set_markup(GTK_LABEL(self->file_type_label), markup);
    g_free(markup);
}


void
vite_status_bar_set_encoding(ViteStatusBar *self, const char *encoding_id)
{
    g_return_if_fail(VITE_IS_STATUS_BAR(self));
    if (!encoding_id) encoding_id = "utf-8";
    
    const char *display = file_encoding_to_display_name_from_id(encoding_id);
    
    char *markup = g_strdup_printf("<span font_weight='normal'>%s</span>", display);
    gtk_label_set_markup(GTK_LABEL(self->encoding_label), markup);
    g_free(markup);
}

void
vite_status_bar_set_line_ending(ViteStatusBar *self, const char *line_ending_id)
{
    g_return_if_fail(VITE_IS_STATUS_BAR(self));
    if (!line_ending_id) line_ending_id = "lf";
    
    /* Map ID to Display Name */
    const char *display = "LF";
    if (g_strcmp0(line_ending_id, "crlf") == 0) display = "CRLF";
    else if (g_strcmp0(line_ending_id, "cr") == 0) display = "CR";
    
    char *markup = g_strdup_printf("<span font_weight='normal'>%s</span>", display);
    gtk_label_set_markup(GTK_LABEL(self->line_ending_label), markup);
    g_free(markup);
    
    /* Update Action State directly using ID */
    if (self->line_ending_group) {
        GAction *act = g_action_map_lookup_action(G_ACTION_MAP(self->line_ending_group), "set");
        if (act) {
            g_simple_action_set_state(G_SIMPLE_ACTION(act), g_variant_new_string(line_ending_id));
        }
    }
}

void
vite_status_bar_set_indentation(ViteStatusBar *self, int width, gboolean use_tabs)
{
    g_return_if_fail(VITE_IS_STATUS_BAR(self));
    
    /* Update Label */
    char *text;
    if (use_tabs) {
        text = g_strdup_printf(_("Tab: %d"), width);
    } else {
        text = g_strdup_printf(_("Spaces: %d"), width);
    }
    
    char *markup = g_strdup_printf("<span font_weight='normal'>%s</span>", text);
    gtk_label_set_markup(GTK_LABEL(self->tab_width_label), markup);
    g_free(markup);
    g_free(text);
    
    /* Sync Action State */
    if (self->indent_group) {
        GAction *act = g_action_map_lookup_action(G_ACTION_MAP(self->indent_group), "set-width");
        if (act) g_simple_action_set_state(G_SIMPLE_ACTION(act), g_variant_new_int32(width));
        
        act = g_action_map_lookup_action(G_ACTION_MAP(self->indent_group), "set-style");
        if (act) g_simple_action_set_state(G_SIMPLE_ACTION(act), g_variant_new_int32(use_tabs ? 1 : 0));
    }
}
